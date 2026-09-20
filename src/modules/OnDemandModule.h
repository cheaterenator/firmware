#pragma once
#include "ProtobufModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/ondemand.pb.h"

// Private mesh port for the fw+ diagnostics/sniffer protocol (MT-SW). Defined here rather than as a
// named entry in portnums.proto, so protobufs/ (the meshtastic/protobufs submodule) stays an unmodified
// checkout of upstream; meshtastic_OnDemand itself lives in protobufs-private/meshtastic/ondemand.proto,
// this fork's own file, not part of that submodule. 354 falls inside the range portnums.proto reserves
// for private/unregistered apps (>= 256, see PRIVATE_APP) but isn't literally PRIVATE_APP (256) itself,
// because that value is already claimed on this firmware by GamesModule - two SinglePortModules can't
// share one portnum (see MeshModule::callModules()/wantPacket()).
constexpr meshtastic_PortNum meshtastic_PortNum_FWPLUS_APP = static_cast<meshtastic_PortNum>(354);

/**
 * OnDemand diagnostics/query protocol (MT-SW): request/response port (FWPLUS_APP) that lets any
 * node in range ask this node for stats, recent history, or a ping - on demand, instead of waiting for
 * this node's own periodic broadcasts. Mostly pure request/response, like RoutingModule; the one
 * periodic job it does run is the opt-in NodeStats broadcast below, so it mixes in OSThread just for
 * that (see DeviceTelemetryModule for the same pattern).
 *
 * Also owns the sniffer on/off switch (see RoutingModule.cpp): snifferEnabled is RAM-only (declared in
 * NodeDB.h/.cpp), so it always starts false after a boot, and REQUEST_SNIFFER_ENABLE/DISABLE are only
 * ever honored when they arrive from the locally-attached phone (mp.from == 0) - unlike the rest of this
 * protocol, which intentionally answers any node in range.
 */
class OnDemandModule : public ProtobufModule<meshtastic_OnDemand>, private concurrency::OSThread
{
  public:
    OnDemandModule();

  protected:
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_OnDemand *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;

    meshtastic_OnDemand prepareNodeStats();
    void sendSegmentedNodeList(const meshtastic_MeshPacket &mp);
    uint32_t sinceLastSeen(const meshtastic_NodeInfoLite *n);
    meshtastic_OnDemand preparePingResponse(const meshtastic_MeshPacket &mp);
    meshtastic_OnDemand preparePingResponseAck(const meshtastic_MeshPacket &mp);
    meshtastic_OnDemand preparePortCounterHistory();
    meshtastic_OnDemand preparePacketHistoryLog();
    meshtastic_OnDemand prepareAirActivityHistoryLog();
    meshtastic_OnDemand prepareRxAvgTimeHistory();
    meshtastic_OnDemand prepareRxPacketHistory();
    meshtastic_OnDemand prepareFwPlusVersion();
    meshtastic_OnDemand prepareRoutingErrorResponse();
    meshtastic_OnDemand prepareSnifferState();
    void sendPacketToRequester(const meshtastic_OnDemand &demand_packet, const meshtastic_MeshPacket &mp, bool wantAck = true);
    bool fitsInPacket(const meshtastic_OnDemand &onDemand, size_t maxSize);

    // Periodic unsolicited RESPONSE_NODE_STATS broadcast (independent switch + interval from
    // moduleConfig.telemetry), for listeners that can observe mesh traffic but can't send a request.
    meshtastic_OnDemand prepareNodeStatsBroadcastConfig();
    void applyNodeStatsBroadcastConfig(const meshtastic_NodeStatsBroadcastConfig &cfg);
    void broadcastNodeStats();
    void loadNodeStatsBroadcastConfig();
    void saveNodeStatsBroadcastConfig();

    bool nodeStatsBroadcastEnabled = false;
    uint32_t nodeStatsBroadcastIntervalSecs = 0; // default_ondemand_node_stats_broadcast_interval_secs resolves this in the .cpp
};

extern OnDemandModule *onDemandModule;
