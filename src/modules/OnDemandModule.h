#pragma once
#include "ProtobufModule.h"
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
 * this node's own periodic broadcasts. Pure request/response, like RoutingModule: no periodic work of
 * its own, so no OSThread.
 *
 * Also owns the sniffer on/off switch (see RoutingModule.cpp): snifferEnabled is RAM-only (declared in
 * NodeDB.h/.cpp), so it always starts false after a boot, and REQUEST_SNIFFER_ENABLE/DISABLE are only
 * ever honored when they arrive from the locally-attached phone (mp.from == 0) - unlike the rest of this
 * protocol, which intentionally answers any node in range.
 */
class OnDemandModule : public ProtobufModule<meshtastic_OnDemand>
{
  public:
    OnDemandModule() : ProtobufModule("OnDemand", meshtastic_PortNum_FWPLUS_APP, &meshtastic_OnDemand_msg) {}

  protected:
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_OnDemand *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;

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
};

extern OnDemandModule *onDemandModule;
