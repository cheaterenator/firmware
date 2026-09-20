#include "OnDemandModule.h"
#include "Default.h"
#include "airtime.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "PowerStatus.h"
#include "RadioLibInterface.h"
#include "Router.h"
#include "SPILock.h"
#include "Throttle.h"
#include "TransmitHistory.h"
#include "UptimeClock.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "main.h"
#include "memGet.h"
#include <cstring>
#include <pb_encode.h>

OnDemandModule *onDemandModule;

static const size_t MAX_PACKET_SIZE = 190;
#define NUM_ONLINE_SECS (60 * 60 * 2)
#define ONDEMAND_MAGIC_USB_BATTERY_LEVEL 101
// Capability signal for the companion app: bump whenever a query/command is added to this protocol so
// the app can tell (via REQUEST_FW_PLUS_VERSION) whether the connected node supports it. 4 = adds
// REQUEST_NODE_STATS_BROADCAST_CONFIG/_SET_ (periodic push of RESPONSE_NODE_STATS, see
// applyNodeStatsBroadcastConfig()).
#define FW_PLUS_VERSION 3

static constexpr uint16_t TX_HISTORY_KEY_ONDEMAND_NODE_STATS = 0x8006;
// A configured interval below this floor is clamped up - mirrors min_default_telemetry_interval_secs'
// role for moduleConfig.telemetry, applied here since this broadcast isn't part of that message.
static constexpr uint32_t MIN_NODE_STATS_BROADCAST_INTERVAL_SECS = min_default_telemetry_interval_secs;

OnDemandModule::OnDemandModule() : ProtobufModule("OnDemand", meshtastic_PortNum_FWPLUS_APP, &meshtastic_OnDemand_msg),
                                    concurrency::OSThread("OnDemand")
{
    nodeStatsBroadcastIntervalSecs = default_ondemand_node_stats_broadcast_interval_secs;
    loadNodeStatsBroadcastConfig();
}

bool OnDemandModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_OnDemand *t)
{
    if (t->which_variant != meshtastic_OnDemand_request_tag) {
        LOG_DEBUG("OnDemand: rx from=0x%08x, which_variant=%d (not a request, ignoring)", mp.from, (int)t->which_variant);
        return false;
    }

    LOG_INFO("OnDemand: rx request_type=%d from=0x%08x channel=%d", (int)t->variant.request.request_type, mp.from, mp.channel);

    switch (t->variant.request.request_type) {
    case meshtastic_OnDemandType_REQUEST_NODE_STATS:
        sendPacketToRequester(prepareNodeStats(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_PACKET_EXCHANGE_HISTORY:
        sendPacketToRequester(preparePacketHistoryLog(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_PORT_COUNTER_HISTORY:
        sendPacketToRequester(preparePortCounterHistory(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_NODES_ONLINE:
        sendSegmentedNodeList(mp);
        break;
    case meshtastic_OnDemandType_REQUEST_PING:
        sendPacketToRequester(preparePingResponse(mp), mp, false);
        break;
    case meshtastic_OnDemandType_REQUEST_PING_ACK:
        sendPacketToRequester(preparePingResponseAck(mp), mp, true);
        break;
    case meshtastic_OnDemandType_REQUEST_ROUTING_ERRORS:
        sendPacketToRequester(prepareRoutingErrorResponse(), mp, true);
        break;
    case meshtastic_OnDemandType_REQUEST_FW_PLUS_VERSION:
        sendPacketToRequester(prepareFwPlusVersion(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_AIR_ACTIVITY_HISTORY:
        sendPacketToRequester(prepareAirActivityHistoryLog(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_RX_AVG_TIME:
        sendPacketToRequester(prepareRxAvgTimeHistory(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_PACKET_RX_HISTORY:
        sendPacketToRequester(prepareRxPacketHistory(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_SNIFFER_ENABLE:
    case meshtastic_OnDemandType_REQUEST_SNIFFER_DISABLE:
        // Local-only: sniffer forwards overheard mesh traffic to whatever's on the other end of this
        // link, so only the phone actually attached to this node (mp.from == 0) may flip it - a remote
        // mesh node asking for it is silently ignored (falls through to the state response below,
        // reporting whatever the flag already was).
        if (mp.from == 0) {
            snifferEnabled = (t->variant.request.request_type == meshtastic_OnDemandType_REQUEST_SNIFFER_ENABLE);
            LOG_INFO("OnDemand: sniffer_enabled set to %d (local request)", snifferEnabled);
        } else {
            LOG_WARN("OnDemand: ignoring sniffer enable/disable from remote node 0x%08x (local-only)", mp.from);
        }
        sendPacketToRequester(prepareSnifferState(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_SNIFFER_STATE:
        sendPacketToRequester(prepareSnifferState(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_NODE_STATS_BROADCAST_CONFIG:
        sendPacketToRequester(prepareNodeStatsBroadcastConfig(), mp);
        break;
    case meshtastic_OnDemandType_REQUEST_SET_NODE_STATS_BROADCAST_CONFIG:
        // Unlike REQUEST_SNIFFER_ENABLE/DISABLE, honored from any node in range (this only toggles
        // an unsolicited stats broadcast, not a phone-traffic tap).
        if (t->variant.request.has_node_stats_broadcast_config) {
            applyNodeStatsBroadcastConfig(t->variant.request.node_stats_broadcast_config);
        } else {
            LOG_WARN("OnDemand: REQUEST_SET_NODE_STATS_BROADCAST_CONFIG from=0x%08x had no config payload, ignoring",
                      mp.from);
        }
        sendPacketToRequester(prepareNodeStatsBroadcastConfig(), mp);
        break;
    default: {
        meshtastic_OnDemand unknown = meshtastic_OnDemand_init_zero;
        unknown.which_variant = meshtastic_OnDemand_response_tag;
        unknown.variant.response.response_type = meshtastic_OnDemandType_UNKNOWN_TYPE;
        sendPacketToRequester(unknown, mp);
    }
    }
    return false; // let other (promiscuous) modules still see this packet
}

meshtastic_MeshPacket *OnDemandModule::allocReply()
{
    return NULL; // We always answer directly via sendPacketToRequester(), not the want_response path
}

bool OnDemandModule::fitsInPacket(const meshtastic_OnDemand &onDemand, size_t maxSize)
{
    uint8_t buffer[512];
    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    if (!pb_encode(&stream, meshtastic_OnDemand_fields, &onDemand))
        return false;
    return stream.bytes_written <= maxSize;
}

uint32_t OnDemandModule::sinceLastSeen(const meshtastic_NodeInfoLite *n)
{
    uint32_t now = getTime();
    int32_t delta = (int32_t)(now - n->last_heard);
    return delta < 0 ? 0 : (uint32_t)delta;
}

// Two-pass, entirely stack-allocated: first pass only counts how many segments the response will need
// (no sending, no allocation), second pass builds and sends each segment as it goes. No `new`/heap use
// for the response itself - each meshtastic_OnDemand is a local reused across the loop, and
// allocDataProtobuf()/sendPacketToRequester() go through the existing packetPool, same as any other
// module's reply.
void OnDemandModule::sendSegmentedNodeList(const meshtastic_MeshPacket &mp)
{
    int totalNodes = nodeDB->getNumMeshNodes();
    NodeNum ourNodeNum = nodeDB->getNodeNum();

    uint32_t totalPackets = 0;
    {
        int idx = 0;
        while (idx < totalNodes) {
            meshtastic_OnDemand probe = meshtastic_OnDemand_init_zero;
            probe.which_variant = meshtastic_OnDemand_response_tag;
            probe.variant.response.which_response_data = meshtastic_OnDemandResponse_node_list_tag;
            auto &listRef = probe.variant.response.response_data.node_list;
            listRef.node_list_count = 0;
            bool addedAny = false;

            while (idx < totalNodes) {
                meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(idx);
                if (!node || sinceLastSeen(node) >= NUM_ONLINE_SECS || node->num == ourNodeNum) {
                    idx++;
                    continue;
                }
                // memset, not `= meshtastic_NodeEntry_init_zero;` - see the comment in prepareNodeStats()
                // for why an assignment (as opposed to a declaration initializer) from a macro with bare
                // "" array-member literals is not portable across toolchains.
                memset(&listRef.node_list[listRef.node_list_count], 0, sizeof(listRef.node_list[listRef.node_list_count]));
                listRef.node_list_count++;
                addedAny = true;
                if (!fitsInPacket(probe, MAX_PACKET_SIZE)) {
                    listRef.node_list_count--;
                    break;
                }
                idx++;
            }
            if (!addedAny)
                break; // guard against an infinite loop
            totalPackets++;
        }
    }

    LOG_INFO("OnDemand: NodesList request - totalNodes=%d (from nodeDB), totalPackets=%u segments needed", totalNodes,
             totalPackets);
    if (totalPackets == 0) {
        // Either nodeDB has nothing but ourselves/stale entries (sinceLastSeen >= NUM_ONLINE_SECS), or
        // totalNodes itself is 0 - either way, no segment is ever sent (see the while-loop guard
        // below), so the requester sees no response at all rather than an empty list.
        LOG_WARN("OnDemand: NodesList has 0 online nodes to report - no response will be sent");
    }

    int currentIndex = 0;
    uint32_t packetIndex = 1;
    while (currentIndex < totalNodes && packetIndex <= totalPackets) {
        meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
        onDemand.which_variant = meshtastic_OnDemand_response_tag;
        onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_NODES_ONLINE;
        onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_node_list_tag;
        onDemand.has_packet_index = true;
        onDemand.has_packet_total = true;
        onDemand.packet_index = packetIndex;
        onDemand.packet_total = totalPackets;

        auto &listRef = onDemand.variant.response.response_data.node_list;
        listRef.node_list_count = 0;

        while (currentIndex < totalNodes) {
            meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(currentIndex);
            if (!node || sinceLastSeen(node) >= NUM_ONLINE_SECS || node->num == ourNodeNum) {
                currentIndex++;
                continue;
            }

            meshtastic_NodeEntry entry = meshtastic_NodeEntry_init_zero;
            entry.node_id = node->num;
            entry.last_heard = sinceLastSeen(node);
            entry.hops = node->has_hops_away ? node->hops_away : 0;
            entry.snr = (!node->has_hops_away || node->hops_away == 0) ? node->snr : 0;
            // NodeInfoLite has no nested `user` (fields are flattened directly onto it).
            strncpy(entry.long_name, node->long_name, sizeof(entry.long_name) - 1);
            strncpy(entry.short_name, node->short_name, sizeof(entry.short_name) - 1);

            listRef.node_list[listRef.node_list_count] = entry;
            listRef.node_list_count++;

            if (!fitsInPacket(onDemand, MAX_PACKET_SIZE)) {
                listRef.node_list_count--;
                break;
            }
            currentIndex++;
        }

        sendPacketToRequester(onDemand, mp);
        packetIndex++;
    }
}

meshtastic_OnDemand OnDemandModule::prepareNodeStats()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_NODE_STATS;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_node_stats_tag;
    auto &s = onDemand.variant.response.response_data.node_stats;
    // meshtastic_OnDemand_init_zero above only zeroes the `request` arm of the outer oneof (the
    // smallest union member); `response_data.node_stats` is still uninitialized stack memory until we
    // zero it explicitly. Without this, every optional field we don't set below (ch1..ch3
    // voltage/current, num_tx_relay, fw_plus_version, ...) would carry garbage has_/value pairs onto
    // the wire instead of being correctly omitted.
    // memset rather than `s = meshtastic_NodeStats_init_zero;`: that macro expands to a flat brace
    // list with a bare "" for the char firmware_version[18] member, which some toolchains (seen on
    // nrf52_promicro's arm-none-eabi-g++) reject as an operator= argument even though the same macro
    // is fine as a declaration initializer. meshtastic_NodeStats is a plain POD struct, so an
    // all-zero-bytes memset is bit-identical to what the macro would produce.
    memset(&s, 0, sizeof(s));

    s.has_battery_level = s.has_channel_utilization = s.has_air_util_tx = s.has_uptime_seconds =
        s.has_num_packets_tx = s.has_num_packets_rx = s.has_num_packets_rx_bad = s.has_num_online_nodes =
            s.has_num_total_nodes = s.has_num_rx_dupe = s.has_num_tx_relay_canceled = s.has_reboots =
                s.has_memory_free_cheap = s.has_memory_total = s.has_cpu_usage_percent = s.has_flood_counter =
                    s.has_nexthop_counter = s.has_firmware_version = s.has_blocked_by_hoplimit = true;

    s.battery_level = (!powerStatus->getHasBattery() || powerStatus->getIsCharging())
                          ? ONDEMAND_MAGIC_USB_BATTERY_LEVEL
                          : powerStatus->getBatteryChargePercent();
    // Same guard DeviceTelemetryModule uses (see GH #7958): only report a voltage reading when we
    // actually have a battery, so an absent-battery -1 mV sentinel never leaks onto the wire as -0.001V.
    int32_t batteryMv = powerStatus->getBatteryVoltageMv();
    if (powerStatus->getHasBattery() && batteryMv > 0) {
        s.has_voltage = true;
        s.voltage = batteryMv / 1000.0f;
    }
    s.channel_utilization = airTime->channelUtilizationPercent();
    s.air_util_tx = airTime->utilizationTXPercent();
    s.uptime_seconds = Time::getUptimeSecs();
    if (RadioLibInterface::instance) {
        s.num_packets_tx = RadioLibInterface::instance->txGood;
        s.num_packets_rx = RadioLibInterface::instance->rxGood + RadioLibInterface::instance->rxBad;
        s.num_packets_rx_bad = RadioLibInterface::instance->rxBad;
    }
    s.num_online_nodes = nodeDB->getNumOnlineMeshNodes(true);
    s.num_total_nodes = nodeDB->getNumMeshNodes();
    if (router) {
        s.num_rx_dupe = router->rxDupe;
        s.num_tx_relay_canceled = router->txRelayCanceled;
        s.flood_counter = router->flood_counter;
        s.nexthop_counter = router->nexthop_counter;
        s.blocked_by_hoplimit = router->blocked_by_hoplimit;
    }
    s.reboots = myNodeInfo.reboot_count;
    s.memory_free_cheap = memGet.getFreeHeap();
    s.memory_total = memGet.getHeapSize();
    s.cpu_usage_percent = CpuHwUsagePercent;
    strncpy(s.firmware_version, optstr(APP_VERSION_SHORT), sizeof(s.firmware_version) - 1);

#if defined(ARCH_ESP32)
    s.has_flash_used_bytes = s.has_flash_total_bytes = s.has_memory_psram_free = s.has_memory_psram_total = true;
    spiLock->lock();
    s.flash_used_bytes = FSCom.usedBytes();
    s.flash_total_bytes = FSCom.totalBytes();
    spiLock->unlock();
    s.memory_psram_free = memGet.getFreePsram();  // 0 on hardware without PSRAM - expected
    s.memory_psram_total = memGet.getPsramSize();
#endif

    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareAirActivityHistoryLog()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_AIR_ACTIVITY_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_air_activity_history_tag;
    auto &hist = onDemand.variant.response.response_data.air_activity_history;

    // AirTime::getActivityWindowCount() (10) matches ondemand.options' AirActivityHistory max_count:10.
    constexpr uint8_t n = AirTime::getActivityWindowCount();
    ActivityTime windows[n] = {};
    bool ok = airTime->activityWindowReport(windows, n);
    hist.air_activity_history_count = ok ? n : 0;
    for (uint8_t i = 0; i < hist.air_activity_history_count; i++) {
        hist.air_activity_history[i].tx_time = windows[i].tx_time;
        hist.air_activity_history[i].rx_time = windows[i].rx_time;
        hist.air_activity_history[i].rxBad_time = windows[i].rx_bad_time;
        // idle_time has no wire slot in meshtastic_AirActivityEntry (ondemand.proto) - not sent.
    }
    LOG_INFO("OnDemand: AirActivityHistory response has %u/%u window(s) (activityWindowReport ok=%d)",
             hist.air_activity_history_count, n, ok);
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareRxAvgTimeHistory()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_RX_AVG_TIME;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_rx_avg_time_history_tag;
    auto &hist = onDemand.variant.response.response_data.rx_avg_time_history;

    // AirTime::getRxWindowCount() (40) matches ondemand.options' RxAvgTimeHistory max_count:40.
    constexpr uint8_t n = AirTime::getRxWindowCount();
    uint32_t averages[n] = {0};
    bool ok = airTime->rxWindowAveragesReport(averages, n);
    hist.rx_avg_history_count = ok ? n : 0;
    for (uint8_t i = 0; i < hist.rx_avg_history_count; i++) {
        hist.rx_avg_history[i] = averages[i];
    }
    LOG_INFO("OnDemand: RxAvgTimeHistory response has %u/%u window(s) (rxWindowAveragesReport ok=%d)",
             hist.rx_avg_history_count, n, ok);
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareRxPacketHistory()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PACKET_RX_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_rx_packet_history_tag;
    auto &hist = onDemand.variant.response.response_data.rx_packet_history;

    // Count of RX+TX mesh packets per 10-minute window - see AirTime::logPacketSeen() (fed from
    // Router::sniffReceived()/Router::send(), not logAirtime()). getPacketCountWindowSize() (40)
    // matches ondemand.options' RxPacketHistory max_count:40.
    constexpr uint8_t n = AirTime::getPacketCountWindowSize();
    uint32_t counts[n] = {0};
    bool ok = airTime->packetCountWindowReport(counts, n);
    hist.rx_packet_history_count = ok ? n : 0;
    for (uint8_t i = 0; i < hist.rx_packet_history_count; i++) {
        hist.rx_packet_history[i] = counts[i];
    }
    LOG_INFO("OnDemand: RxPacketHistory response has %u/%u window(s) (packetCountWindowReport ok=%d)",
             hist.rx_packet_history_count, n, ok);
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePacketHistoryLog()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PACKET_EXCHANGE_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_exchange_packet_log_tag;
    auto &log = onDemand.variant.response.response_data.exchange_packet_log;
    log.exchange_list_count = PacketHistoryLog::CAPACITY;
    uint32_t nonEmpty = 0;
    for (size_t i = 0; i < PacketHistoryLog::CAPACITY; i++) {
        const auto &e = nodeDB->packetHistoryLog.entries[i];
        log.exchange_list[i].from_node = e.from_node;
        log.exchange_list[i].to_node = e.to_node;
        log.exchange_list[i].port_num = e.port_num;
        if (e.from_node || e.to_node || e.port_num)
            nonEmpty++;
    }
    // If this is 0, RoutingModule::handleReceivedProtobuf() never called packetHistoryLog.addEntry()
    // for this node - check the "OnDemand/Sniffer: skip port/history capture" / "recorded ..." lines in
    // RoutingModule.cpp for why (rebroadcast-mode gate, licensed-user gate, or simply no traffic yet).
    LOG_INFO("OnDemand: PacketHistoryLog response has %u/%u non-empty entries", nonEmpty, (unsigned)PacketHistoryLog::CAPACITY);
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePortCounterHistory()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PORT_COUNTER_HISTORY;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_port_counter_history_tag;

    uint8_t entryCount = 0;
    for (uint32_t i = 0; i < MAX_PORTS && entryCount < 20; i++) {
        if (portCounters[i] > 0) {
            onDemand.variant.response.response_data.port_counter_history.port_counter_history[entryCount].port = i;
            onDemand.variant.response.response_data.port_counter_history.port_counter_history[entryCount].count =
                portCounters[i];
            entryCount++;
        }
    }
    onDemand.variant.response.response_data.port_counter_history.port_counter_history_count = entryCount;
    // If entryCount is 0, no packet ever hit the `if (port < MAX_PORTS) ++portCounters[port];` line in
    // RoutingModule::handleReceivedProtobuf() - same root cause as an empty PacketHistoryLog response.
    LOG_INFO("OnDemand: PortCounterHistory response has %u port(s) with traffic", entryCount);
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareRoutingErrorResponse()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_ROUTING_ERRORS;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_routing_errors_tag;
    auto &errs = onDemand.variant.response.response_data.routing_errors;
    // meshtastic_Routing_Error currently runs 0..39 (see mesh.pb.h); packetErrorCounters and this
    // response are both sized to 40 to match, with no gap left for a future error code to overflow.
    errs.routing_errors_count = 40;
    if (router) {
        for (uint16_t i = 0; i < 40; i++) {
            errs.routing_errors[i].num = i;
            errs.routing_errors[i].counter = router->packetErrorCounters[i];
        }
    }
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePingResponse(const meshtastic_MeshPacket &mp)
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PING;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_ping_tag;
    // Same reasoning as prepareNodeStats(): zero the union member explicitly before conditionally
    // populating it, so the has_rx_rssi/has_snr flags are reliably false when we don't set them below,
    // rather than whatever was on the stack. memset for the same cross-toolchain reason noted there
    // (moot for this particular struct - no array members - but kept consistent).
    memset(&onDemand.variant.response.response_data.ping, 0, sizeof(onDemand.variant.response.response_data.ping));
	int8_t hops = getHopsAway(mp);
    if (mp.from != 0x0 && mp.from != nodeDB->getNodeNum() && mp.hop_limit == mp.hop_start) {
        onDemand.variant.response.response_data.ping.has_rx_rssi = true;
        onDemand.variant.response.response_data.ping.has_snr = true;
        onDemand.variant.response.response_data.ping.rx_rssi = mp.rx_rssi;
        onDemand.variant.response.response_data.ping.snr = mp.rx_snr;
    }
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::preparePingResponseAck(const meshtastic_MeshPacket &mp)
{
    meshtastic_OnDemand onDemand = preparePingResponse(mp);
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PING_ACK;
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareFwPlusVersion()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_FW_PLUS_VERSION;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_fw_plus_version_tag;
    onDemand.variant.response.response_data.fw_plus_version.version_number = FW_PLUS_VERSION;
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareSnifferState()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_SNIFFER_STATE;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_sniffer_state_tag;
    onDemand.variant.response.response_data.sniffer_state.enabled = snifferEnabled;
    return onDemand;
}

meshtastic_OnDemand OnDemandModule::prepareNodeStatsBroadcastConfig()
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_NODE_STATS_BROADCAST_CONFIG;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_node_stats_broadcast_config_tag;
    onDemand.variant.response.response_data.node_stats_broadcast_config.enabled = nodeStatsBroadcastEnabled;
    onDemand.variant.response.response_data.node_stats_broadcast_config.interval_secs = nodeStatsBroadcastIntervalSecs;
    return onDemand;
}

void OnDemandModule::applyNodeStatsBroadcastConfig(const meshtastic_NodeStatsBroadcastConfig &cfg)
{
    nodeStatsBroadcastEnabled = cfg.enabled;
    nodeStatsBroadcastIntervalSecs = Default::getConfiguredOrMinimumValue(
        Default::getConfiguredOrDefault(cfg.interval_secs, default_ondemand_node_stats_broadcast_interval_secs),
        MIN_NODE_STATS_BROADCAST_INTERVAL_SECS);
    LOG_INFO("OnDemand: NodeStats broadcast config set to enabled=%d interval_secs=%u", nodeStatsBroadcastEnabled,
             nodeStatsBroadcastIntervalSecs);
    saveNodeStatsBroadcastConfig();
}

void OnDemandModule::broadcastNodeStats()
{
    meshtastic_MeshPacket *p = allocDataProtobuf(prepareNodeStats());
    if (!p) {
        LOG_WARN("OnDemand: packetPool exhausted, dropping periodic NodeStats broadcast");
        return;
    }
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    LOG_INFO("OnDemand: broadcasting NodeStats to mesh (interval_secs=%u)", nodeStatsBroadcastIntervalSecs);
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

#ifdef FSCom
namespace
{
// Own on-disk format, not a pb_encode of NodeStatsBroadcastConfig - two scalars don't need a
// protobuf round-trip; a magic+version header plus a packed struct suffices (see TransmitHistory).
constexpr const char *NODE_STATS_BROADCAST_CONFIG_FILENAME = "/prefs/ondemand_stats_bcast.dat";
constexpr uint32_t NODE_STATS_BROADCAST_CONFIG_MAGIC = 0x4E534243; // "NSBC"
constexpr uint8_t NODE_STATS_BROADCAST_CONFIG_VERSION = 1;

struct __attribute__((packed)) NodeStatsBroadcastConfigFile {
    uint32_t magic;
    uint8_t version;
    uint8_t enabled;
    uint32_t intervalSecs;
};
} // namespace

void OnDemandModule::loadNodeStatsBroadcastConfig()
{
    spiLock->lock();
    auto file = FSCom.open(NODE_STATS_BROADCAST_CONFIG_FILENAME, FILE_O_READ);
    if (file) {
        NodeStatsBroadcastConfigFile stored{};
        bool ok = file.read((uint8_t *)&stored, sizeof(stored)) == sizeof(stored);
        if (ok && stored.magic == NODE_STATS_BROADCAST_CONFIG_MAGIC && stored.version == NODE_STATS_BROADCAST_CONFIG_VERSION) {
            nodeStatsBroadcastEnabled = stored.enabled;
            nodeStatsBroadcastIntervalSecs = Default::getConfiguredOrMinimumValue(
                Default::getConfiguredOrDefault(stored.intervalSecs, default_ondemand_node_stats_broadcast_interval_secs),
                MIN_NODE_STATS_BROADCAST_INTERVAL_SECS);
            LOG_INFO("OnDemand: loaded NodeStats broadcast config from disk: enabled=%d interval_secs=%u",
                     nodeStatsBroadcastEnabled, nodeStatsBroadcastIntervalSecs);
        } else {
            LOG_WARN("OnDemand: invalid NodeStats broadcast config file, using defaults");
        }
        file.close();
    } else {
        LOG_INFO("OnDemand: no NodeStats broadcast config file found, using defaults (enabled=%d interval_secs=%u)",
                 nodeStatsBroadcastEnabled, nodeStatsBroadcastIntervalSecs);
    }
    spiLock->unlock();
}

void OnDemandModule::saveNodeStatsBroadcastConfig()
{
    spiLock->lock();
    FSCom.mkdir("/prefs");
    if (FSCom.exists(NODE_STATS_BROADCAST_CONFIG_FILENAME)) {
        FSCom.remove(NODE_STATS_BROADCAST_CONFIG_FILENAME);
    }
    auto file = FSCom.open(NODE_STATS_BROADCAST_CONFIG_FILENAME, FILE_O_WRITE);
    if (file) {
        NodeStatsBroadcastConfigFile stored{};
        stored.magic = NODE_STATS_BROADCAST_CONFIG_MAGIC;
        stored.version = NODE_STATS_BROADCAST_CONFIG_VERSION;
        stored.enabled = nodeStatsBroadcastEnabled;
        stored.intervalSecs = nodeStatsBroadcastIntervalSecs;
        file.write((uint8_t *)&stored, sizeof(stored));
        file.flush();
        file.close();
        LOG_DEBUG("OnDemand: saved NodeStats broadcast config to disk");
    } else {
        LOG_WARN("OnDemand: failed to open NodeStats broadcast config file for writing");
    }
    spiLock->unlock();
}
#else
// No filesystem available on this arch - config stays in-memory only for the boot session.
void OnDemandModule::loadNodeStatsBroadcastConfig() {}
void OnDemandModule::saveNodeStatsBroadcastConfig() {}
#endif

int32_t OnDemandModule::runOnce()
{
    // Idle poll cadence while disabled/waiting - cheap, and fine-grained enough for any interval
    // this broadcast is configured to (floored at MIN_NODE_STATS_BROADCAST_INTERVAL_SECS minutes).
    static constexpr int32_t POLL_MS = 60 * 1000;

    if (!nodeStatsBroadcastEnabled || config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN) {
        return POLL_MS;
    }

    uint32_t lastSent = transmitHistory ? transmitHistory->getLastSentToMeshMillis(TX_HISTORY_KEY_ONDEMAND_NODE_STATS) : 0;
    if ((lastSent == 0 || Throttle::hasElapsed(lastSent, nodeStatsBroadcastIntervalSecs * 1000UL)) &&
        airTime->isTxAllowedChannelUtil(true) && airTime->isTxAllowedAirUtil()) {
        broadcastNodeStats();
        if (transmitHistory)
            transmitHistory->setLastSentToMesh(TX_HISTORY_KEY_ONDEMAND_NODE_STATS);
    }
    return POLL_MS;
}

void OnDemandModule::sendPacketToRequester(const meshtastic_OnDemand &demand_packet, const meshtastic_MeshPacket &mp,
                                           bool wantAck)
{
    meshtastic_MeshPacket *p = allocDataProtobuf(demand_packet);
    if (!p) { // pool exhausted; drop like any other module would
        LOG_WARN("OnDemand: packetPool exhausted, dropping response_type=%d to=0x%08x",
                 (int)demand_packet.variant.response.response_type, mp.from);
        return;
    }
    p->to = getFrom(&mp); // mp.from is always 0 for phone-originated requests; resolve to our own node
    p->decoded.want_response = false;
    p->want_ack = wantAck;
    p->channel = mp.channel;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    LOG_INFO("OnDemand: tx response_type=%d to=0x%08x channel=%d want_ack=%d payload_size=%u",
             (int)demand_packet.variant.response.response_type, p->to, p->channel, wantAck, p->decoded.payload.size);
    service->sendToMesh(p, RX_SRC_LOCAL, false);
}
