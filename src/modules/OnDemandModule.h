#pragma once
#include "ProtobufModule.h"
#include "mesh/generated/meshtastic/ondemand.pb.h"

/**
 * OnDemand diagnostics/query protocol (MT-SW): request/response port (ON_DEMAND_APP) that lets any
 * node in range ask this node for stats, recent history, or a ping - on demand, instead of waiting for
 * this node's own periodic broadcasts. Pure request/response, like RoutingModule: no periodic work of
 * its own, so no OSThread.
 */
class OnDemandModule : public ProtobufModule<meshtastic_OnDemand>
{
  public:
    OnDemandModule() : ProtobufModule("OnDemand", meshtastic_PortNum_ON_DEMAND_APP, &meshtastic_OnDemand_msg) {}

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
    void sendPacketToRequester(const meshtastic_OnDemand &demand_packet, const meshtastic_MeshPacket &mp, bool wantAck = true);
    bool fitsInPacket(const meshtastic_OnDemand &onDemand, size_t maxSize);
};

extern OnDemandModule *onDemandModule;
