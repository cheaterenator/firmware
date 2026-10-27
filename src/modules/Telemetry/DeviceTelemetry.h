#pragma once
#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "BaseTelemetryModule.h"
#include "NodeDB.h"
#include "ProtobufModule.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>
#include "FSCommon.h"

class DeviceTelemetryModule : private concurrency::OSThread,
                              public BaseTelemetryModule,
                              public ProtobufModule<meshtastic_Telemetry>
{
    CallbackObserver<DeviceTelemetryModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<DeviceTelemetryModule, const meshtastic::Status *>(this, &DeviceTelemetryModule::handleStatusUpdate);

  public:
    DeviceTelemetryModule()
        : concurrency::OSThread("DeviceTelemetry"),
          ProtobufModule("DeviceTelemetry", meshtastic_PortNum_TELEMETRY_APP, &meshtastic_Telemetry_msg)
    {
        nodeStatusObserver.observe(&nodeStatus->onNewStatus);
        setIntervalFromNow(setStartDelay()); // Wait until NodeInfo is sent
    }
    virtual bool wantUIFrame() { return false; }

    // LocalStats / LocalStatsExtended broadcast cadence (LocalStatsBroadcastConfig in ondemand.proto):
    // due intervalMs + extraDelayMs after lastSentMs, or only extraDelayMs after startMs (our first run) when
    // there is no last send (lastSentMs == 0), so first sends after boot are spread out, not all at once.
    static bool localStatsBroadcastDue(uint32_t nowMs, uint32_t lastSentMs, uint32_t startMs, uint32_t intervalMs,
                                       uint32_t extraDelayMs);

  protected:
    /** Called to handle a particular incoming message
    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;
    /**
     * Send our Telemetry into the mesh
     */
    bool sendTelemetry(NodeNum dest = NODENUM_BROADCAST, bool phoneOnly = false);

  private:
    meshtastic_Telemetry getDeviceTelemetry();
    meshtastic_Telemetry getLocalStatsTelemetry();
	meshtastic_Telemetry getLocalStatsExtendedTelemetry();

    void sendLocalStatsToPhone();
    bool sendLocalStatsToMesh();
    bool sendLocalStatsExtendedToMesh();
    // Role, airtime, and MESH_BROADCAST_GAP_MS since lastMeshBroadcastMs: shared by all our mesh broadcasts.
    bool meshBroadcastAllowed();
    // Sends LocalStats (or LocalStatsExtended) once it is due, then redraws its extra delay. true if sent.
    bool maybeSendLocalStatsToMesh(bool extended, bool enabled, uint32_t intervalSecs, uint32_t &extraDelayPermille);
    uint32_t sendToPhoneIntervalMs = SECONDS_IN_MINUTE * 1000;           // Send to phone every minute
    uint32_t sendStatsToPhoneIntervalMs = 15 * SECONDS_IN_MINUTE * 1000; // Send stats to phone every 15 minutes
    uint32_t lastSentStatsToPhone = 0;
    bool statsHaveBeenSent = false;
    uint32_t firstRunMs = 0;          // stamp of our first runOnce(), where the first LocalStats waits start
    uint32_t lastMeshBroadcastMs = 0; // stamp of our last device metrics / LocalStats(Extended) mesh broadcast
    // Random extra delay of the next LocalStats / LocalStatsExtended emission, in permille of its
    // interval. Redrawn after every emission, so the two drift apart even on equal intervals.
    uint32_t localStatsExtraDelayPermille = 0;
    uint32_t localStatsExtendedExtraDelayPermille = 0;
};