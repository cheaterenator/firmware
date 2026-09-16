#include "RoutingModule.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "configuration.h"
#include "main.h"

RoutingModule *routingModule;

bool RoutingModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Routing *r)
{
    bool maybePKI = mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag && mp.channel == 0 && !isBroadcast(mp.to);
    // Beginning of logic whether to drop the packet based on Rebroadcast mode
    if (mp.which_payload_variant == meshtastic_MeshPacket_encrypted_tag &&
        (config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_LOCAL_ONLY ||
         config.device.rebroadcast_mode == meshtastic_Config_DeviceConfig_RebroadcastMode_KNOWN_ONLY)) {
        if (!maybePKI) {
            // Sniffer/OnDemand diag (MT-SW): this and the two returns below are the only paths that
            // skip the portCounters/packetHistoryLog update further down - if those look empty on the
            // console, check whether packets are consistently exiting here instead.
            LOG_DEBUG("OnDemand/Sniffer: skip port/history capture, not maybePKI under LOCAL_ONLY/KNOWN_ONLY "
                      "(from=0x%08x, to=0x%08x)",
                      mp.from, mp.to);
            return false;
        }
        if (!nodeInfoLiteHasUser(nodeDB->getMeshNode(mp.from)) && !nodeInfoLiteHasUser(nodeDB->getMeshNode(mp.to))) {
            LOG_DEBUG("OnDemand/Sniffer: skip port/history capture, neither endpoint has a known user "
                      "(from=0x%08x, to=0x%08x)",
                      mp.from, mp.to);
            return false;
        }
    } else if (owner.is_licensed && ((nodeDB->getLicenseStatus(mp.from) == UserLicenseStatus::NotLicensed) ||
                                     (nodeDB->getLicenseStatus(mp.to) == UserLicenseStatus::NotLicensed))) {
        // Don't let licensed users to rebroadcast packets to or from unlicensed users
        // If we know they are in-fact unlicensed
        LOG_DEBUG("Packet to or from unlicensed user, ignoring packet");
        LOG_DEBUG("OnDemand/Sniffer: skip port/history capture, licensed/unlicensed gate (from=0x%08x, to=0x%08x)", mp.from,
                  mp.to);
        return false;
    }

    printPacket("Routing sniffing", &mp);
    router->sniffReceived(&mp, r);

    // Sniffer/OnDemand support (MT-SW): per-port packet counter and a short ring of recent
    // from/to/port exchanges, read by OnDemandModule's REQUEST_PORT_COUNTER_HISTORY and
    // REQUEST_PACKET_EXCHANGE_HISTORY handlers.
    if (mp.which_payload_variant == meshtastic_MeshPacket_decoded_tag) {
        meshtastic_PortNum port = mp.decoded.portnum;
        if (port < MAX_PORTS) {
            ++portCounters[port];
        } else {
            LOG_WARN("OnDemand/Sniffer: portnum %d out of portCounters range (MAX_PORTS=%u), dropped", (int)port, MAX_PORTS);
        }
        nodeDB->packetHistoryLog.addEntry({mp.from, mp.to, static_cast<uint32_t>(port)});
        LOG_DEBUG("OnDemand/Sniffer: recorded port=%d from=0x%08x to=0x%08x (portCounters[%d]=%u)", (int)port, mp.from, mp.to,
                  (int)port, (port < MAX_PORTS) ? portCounters[port] : 0);
    } else {
        ++portCounters[MAX_PORTS - 1];
        LOG_DEBUG("OnDemand/Sniffer: recorded undecoded/encrypted packet from=0x%08x to=0x%08x (portCounters[%u]=%u)", mp.from,
                  mp.to, MAX_PORTS - 1, portCounters[MAX_PORTS - 1]);
    }

    // FIXME - move this to a non promsicious PhoneAPI module?
    // Note: we are careful not to send back packets that started with the phone back to the phone
    if ((isBroadcast(mp.to) || isToUs(&mp)) && (mp.from != 0)) {
        printPacket("Delivering rx packet", &mp);
        service->handleFromRadio(&mp);
    } else if (snifferEnabled && (mp.from != 0) && !isFromUs(&mp)) {
        // Sniffer mode (MT-SW): forward transit traffic (not broadcast, not to us, not from us) once -
        // the branch above already covers broadcasts and packets addressed to us, so this is exactly
        // the traffic the phone would otherwise never see: something we're only relaying/overhearing.
        meshtastic_MeshPacket *copyPtr = packetPool.allocCopy(mp);
        if (copyPtr) {
            LOG_DEBUG("Sniffer: forwarding overheard transit packet from=0x%08x to=0x%08x to phone", mp.from, mp.to);
            service->sendPacketToPhoneRaw(copyPtr);
        } else {
            LOG_WARN("Sniffer: packetPool exhausted, could not copy overheard packet for sniffing");
        }
    }

    return false; // Let others look at this message also if they want
}

meshtastic_MeshPacket *RoutingModule::allocReply()
{
    assert(currentRequest);

    return NULL;
}

void RoutingModule::sendAckNak(meshtastic_Routing_Error err, NodeNum to, PacketId idFrom, ChannelIndex chIndex, uint8_t hopLimit,
                               bool ackWantsAck, const meshtastic_MeshPacket *relaySource)
{
    auto p = allocAckNak(err, to, idFrom, chIndex, hopLimit, relaySource);
    if (!p)
        return;

    // Sniffer mode (MT-SW): mirror the ACK/NAK we're about to send - sendLocal() below only reaches
    // the phone when `to` is us, so for anything addressed elsewhere this is the only copy the phone
    // would otherwise get.
    if (snifferEnabled) {
        meshtastic_MeshPacket *copyPtr = packetPool.allocCopy(*p);
        if (copyPtr) {
            service->sendPacketToPhoneRaw(copyPtr);
        } else {
            LOG_WARN("Sniffer: packetPool exhausted, could not copy ACK/NAK for sniffing");
        }
    }

    // Allow the caller to set want_ack on this ACK packet if it's important that the ACK be delivered reliably
    p->want_ack = ackWantsAck;

    if (router->sendLocal(p) == ERRNO_SHOULD_RELEASE) // we sometimes send directly to the local node
        packetPool.release(p);
}

uint8_t RoutingModule::getHopLimitForResponse(const meshtastic_MeshPacket &mp)
{
    const int8_t hopsUsed = getHopsAway(mp);
    const uint8_t responseHopLimit = Default::getConfiguredOrDefaultHopLimit(config.lora.hop_limit);
    if (hopsUsed >= 0) {
        if (hopsUsed > static_cast<int32_t>(responseHopLimit)) {
// In event mode, never exceed the configured event hop limit.
#if !USERPREFS_EVENT_MODE    // This falls through to the default.
            return hopsUsed; // If the request used more hops than the limit, use the same amount of hops
#endif
        } else if (mp.hop_start == 0) {
            return 0; // The requesting node wanted 0 hops, so the response also uses a direct/local path.
        } else if (static_cast<uint8_t>(hopsUsed + 2) < responseHopLimit) {
            return hopsUsed + 2; // Use only the amount of hops needed with some margin as the way back may be different
        }
    }
    return responseHopLimit;
}

meshtastic_MeshPacket *RoutingModule::allocAckNak(meshtastic_Routing_Error err, NodeNum to, PacketId idFrom, ChannelIndex chIndex,
                                                  uint8_t hopLimit, const meshtastic_MeshPacket *relaySource)
{
    return MeshModule::allocAckNak(err, to, idFrom, chIndex, hopLimit, relaySource);
}

RoutingModule::RoutingModule() : ProtobufModule("routing", meshtastic_PortNum_ROUTING_APP, &meshtastic_Routing_msg)
{
    isPromiscuous = true;

    // moved the RebroadcastMode logic into handleReceivedProtobuf
    // LocalOnly requires either the from or to to be a known node
    // knownOnly specifically requires the from to be a known node.
    encryptedOk = true;
}
