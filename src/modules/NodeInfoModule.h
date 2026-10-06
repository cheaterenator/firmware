#pragma once
#include "ProtobufModule.h"
#include "concurrency/Periodic.h"
#include <map>
#include <memory>

/// After we answer a sender's NodeInfo request, further requests from it go unanswered for this long.
#ifndef USERPREFS_NODEINFO_REPLY_SUPPRESS_SECS
#define USERPREFS_NODEINFO_REPLY_SUPPRESS_SECS (12 * 60 * 60)
#endif

/**
 * NodeInfo module for sending/receiving NodeInfos into the mesh
 */
class NodeInfoModule : public ProtobufModule<meshtastic_User>, private concurrency::OSThread
{
    /// The id of the last packet we sent, to allow us to cancel it if we make something fresher
    PacketId prevPacketId = 0;

    uint32_t currentGeneration = 0;

  public:
    /** Constructor
     * name is for debugging output
     */
    NodeInfoModule();

    /**
     * Send our NodeInfo into the mesh. True only when a packet was handed to the router.
     */
    bool sendOurNodeInfo(NodeNum dest = NODENUM_BROADCAST, bool wantReplies = false, uint8_t channel = 0,
                         bool _shorterTimeout = false);

    /**
     * Schedule an immediate NodeInfo periodic check.
     * Used when external conditions change (for example time source quality).
     */
    void triggerImmediateNodeInfoCheck();

    /**
     * Note a client's NodeInfo request on its way to a remote. If no NodeInfo from that remote has
     * arrived NodeInfoRequestFallbackMs later, prompt it with PKI_UNKNOWN_PUBKEY - see runRequestFallback().
     */
    void noteOutgoingNodeInfoRequest(const meshtastic_MeshPacket &p);

#ifdef PIO_UNIT_TESTING
    int32_t runRequestFallbackForTests() { return runRequestFallback(); }
    /// Test-only reads of the routine-broadcast countdown a send re-arms. concurrency::OSThread is a
    /// private base, so only this class can reach it - a test shim cannot.
    unsigned long broadcastCountdownMsForTests() const { return interval; }
    void armBroadcastCountdownForTests(unsigned long ms) { setIntervalFromNow(ms); }
    /// The deadline the scheduler actually reads. interval alone cannot tell a deadline moved to
    /// now from one recomputed off a stale last_run, which is the regression worth catching.
    unsigned long broadcastDeadlineMsForTests() const { return _cached_next_run; }
    /// Pretend the periodic thread last ran ageMs ago, so those two deadlines differ by ageMs.
    void ageLastRunForTests(unsigned long ageMs) { runned(millis() - ageMs); }
#endif

  protected:
    /** Called to handle a particular incoming message

    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_User *p) override;

    /** Messages can be received that have the want_response bit set.  If set, this callback will be invoked
     * so that subclasses can (optionally) send a response back to the original sender.  */
    virtual meshtastic_MeshPacket *allocReply() override;

    /** Does our periodic broadcast */
    virtual int32_t runOnce() override;

  private:
    bool shorterTimeout = false;
    /// Set across sendOurNodeInfo()'s own allocReply(), so the transmit stamp waits for an accepted send.
    bool deferHistoryStamp = false;
    bool suppressReplyForCurrentRequest = false;
    /// Sender -> uptime seconds (Time::getUptimeSecs()) at our last reply. Seconds, not millis:
    /// the suppression window is hours wide. Stamped in allocReply(), read in handleReceivedProtobuf().
    std::map<NodeNum, uint32_t> lastNodeInfoSeen;

    /// A client's NodeInfo request still waiting for any NodeInfo from its target. Once the fallback
    /// has fired, sentMs is re-stamped and the entry holds off another one for that target.
    struct PendingNodeInfoRequest {
        NodeNum target = 0;
        PacketId requestId = 0;
        uint32_t sentMs = 0;
        ChannelIndex channel = 0;
        uint8_t hopLimit = 0;
        bool fallbackSent = false;
    };
    PendingNodeInfoRequest pendingRequests[4];
    std::unique_ptr<concurrency::Periodic> requestFallbackTimer;

    int32_t runRequestFallback();
    void pruneLastNodeInfoCache();
};

extern NodeInfoModule *nodeInfoModule;
