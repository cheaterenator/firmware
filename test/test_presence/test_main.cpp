#include "TestUtil.h"
#include "modules/OnDemandModule.h"
#include <cstring>
#include <pb_encode.h>
#include <unity.h>

// User presence (Presence in ondemand.proto). OnDemandModule::presenceBroadcastDue() is the broadcast cadence
// and takes plain values, so no fake clock is needed; OnDemandModule::isPresencePacket() is what Router and
// MQTT use to keep presence off MQTT.

static constexpr uint32_t MIN_MS = 60000U;
static constexpr meshtastic_Presence_State UNKNOWN = meshtastic_Presence_State_UNKNOWN;
static constexpr meshtastic_Presence_State OFFLINE = meshtastic_Presence_State_OFFLINE;
static constexpr meshtastic_Presence_State ONLINE = meshtastic_Presence_State_ONLINE;

// After a boot nothing has been broadcast: the current state goes out once it has held 5 minutes.
static void test_due_afterBoot_waitsSettle()
{
    TEST_ASSERT_FALSE(OnDemandModule::presenceBroadcastDue(1000U + 5 * MIN_MS - 1, OFFLINE, UNKNOWN, 1000U, false, 0U));
    TEST_ASSERT_TRUE(OnDemandModule::presenceBroadcastDue(1000U + 5 * MIN_MS, OFFLINE, UNKNOWN, 1000U, false, 0U));
}

// A state the last broadcast already said is never repeated, however long ago that was.
static void test_due_sameAsSent_never()
{
    TEST_ASSERT_FALSE(OnDemandModule::presenceBroadcastDue(1000U + 600 * MIN_MS, ONLINE, ONLINE, 1000U, true, 1000U));
}

// A change 10 minutes after a broadcast waits for the 30-minute gap, not just 5 minutes.
static void test_due_changeSoonAfterSend_waitsGap()
{
    constexpr uint32_t sent = 1000U;
    constexpr uint32_t changed = sent + 10 * MIN_MS;
    TEST_ASSERT_FALSE(OnDemandModule::presenceBroadcastDue(changed + 5 * MIN_MS, OFFLINE, ONLINE, changed, true, sent));
    TEST_ASSERT_FALSE(OnDemandModule::presenceBroadcastDue(sent + 30 * MIN_MS - 1, OFFLINE, ONLINE, changed, true, sent));
    TEST_ASSERT_TRUE(OnDemandModule::presenceBroadcastDue(sent + 30 * MIN_MS, OFFLINE, ONLINE, changed, true, sent));
}

// A change long after the last broadcast waits only the 5-minute settle.
static void test_due_changeLongAfterSend_waitsSettleOnly()
{
    constexpr uint32_t sent = 1000U;
    constexpr uint32_t changed = sent + 40 * MIN_MS;
    TEST_ASSERT_FALSE(OnDemandModule::presenceBroadcastDue(changed + 5 * MIN_MS - 1, OFFLINE, ONLINE, changed, true, sent));
    TEST_ASSERT_TRUE(OnDemandModule::presenceBroadcastDue(changed + 5 * MIN_MS, OFFLINE, ONLINE, changed, true, sent));
}

// Each new flip restarts the settle: the state must hold 5 minutes in one piece.
static void test_due_flipRestartsSettle()
{
    constexpr uint32_t sent = 1000U;
    constexpr uint32_t changed = sent + 33 * MIN_MS; // dropped at +30, back at +31, dropped again at +33
    TEST_ASSERT_FALSE(OnDemandModule::presenceBroadcastDue(sent + 35 * MIN_MS, OFFLINE, ONLINE, changed, true, sent));
    TEST_ASSERT_TRUE(OnDemandModule::presenceBroadcastDue(sent + 38 * MIN_MS, OFFLINE, ONLINE, changed, true, sent));
}

// Timestamps from before millis() wrapped to 0 still measure correctly.
static void test_due_survivesMillisWrap()
{
    constexpr uint32_t sent = UINT32_MAX - 999U;     // 1,000 ms before the wrap
    constexpr uint32_t changed = sent + 40 * MIN_MS; // wrapped
    TEST_ASSERT_FALSE(OnDemandModule::presenceBroadcastDue(changed + 5 * MIN_MS - 1, OFFLINE, ONLINE, changed, true, sent));
    TEST_ASSERT_TRUE(OnDemandModule::presenceBroadcastDue(changed + 5 * MIN_MS, OFFLINE, ONLINE, changed, true, sent));
}

static meshtastic_Data encodeData(const meshtastic_OnDemand &onDemand, meshtastic_PortNum portnum = meshtastic_PortNum_FWPLUS_APP)
{
    meshtastic_Data d = meshtastic_Data_init_zero;
    d.portnum = portnum;
    pb_ostream_t stream = pb_ostream_from_buffer(d.payload.bytes, sizeof(d.payload.bytes));
    TEST_ASSERT_TRUE(pb_encode(&stream, meshtastic_OnDemand_fields, &onDemand));
    d.payload.size = stream.bytes_written;
    return d;
}

// As OnDemandModule::runPresence() builds a broadcast.
static meshtastic_OnDemand presenceBroadcast(meshtastic_Presence_State state)
{
    meshtastic_OnDemand onDemand = meshtastic_OnDemand_init_zero;
    onDemand.which_variant = meshtastic_OnDemand_response_tag;
    onDemand.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_PRESENCE;
    onDemand.variant.response.which_response_data = meshtastic_OnDemandResponse_presence_tag;
    memset(&onDemand.variant.response.response_data.presence, 0, sizeof(onDemand.variant.response.response_data.presence));
    onDemand.variant.response.response_data.presence.state = state;
    return onDemand;
}

static void test_isPresence_broadcast()
{
    TEST_ASSERT_TRUE(OnDemandModule::isPresencePacket(encodeData(presenceBroadcast(ONLINE))));
    TEST_ASSERT_TRUE(OnDemandModule::isPresencePacket(encodeData(presenceBroadcast(OFFLINE))));
}

// An answer to REQUEST_PRESENCE: with secs_since_change, and for a node that has presence switched off
// (UNKNOWN = 0, so response_data.presence encodes empty).
static void test_isPresence_answer()
{
    meshtastic_OnDemand answer = presenceBroadcast(ONLINE);
    answer.variant.response.response_data.presence.has_secs_since_change = true;
    answer.variant.response.response_data.presence.secs_since_change = 123456;
    TEST_ASSERT_TRUE(OnDemandModule::isPresencePacket(encodeData(answer)));
    TEST_ASSERT_TRUE(OnDemandModule::isPresencePacket(encodeData(presenceBroadcast(UNKNOWN))));
}

// Fields ahead of the response (segment index/total) are skipped.
static void test_isPresence_afterSegmentFields()
{
    meshtastic_OnDemand onDemand = presenceBroadcast(ONLINE);
    onDemand.has_packet_index = true;
    onDemand.packet_index = 1;
    onDemand.has_packet_total = true;
    onDemand.packet_total = 1;
    TEST_ASSERT_TRUE(OnDemandModule::isPresencePacket(encodeData(onDemand)));
}

static void test_isPresence_otherOnDemandTraffic()
{
    meshtastic_OnDemand lan = meshtastic_OnDemand_init_zero;
    lan.which_variant = meshtastic_OnDemand_response_tag;
    lan.variant.response.response_type = meshtastic_OnDemandType_RESPONSE_LAN_STATS;
    lan.variant.response.which_response_data = meshtastic_OnDemandResponse_lan_stats_tag;
    TEST_ASSERT_FALSE(OnDemandModule::isPresencePacket(encodeData(lan)));

    meshtastic_OnDemand request = meshtastic_OnDemand_init_zero;
    request.which_variant = meshtastic_OnDemand_request_tag;
    request.variant.request.request_type = meshtastic_OnDemandType_REQUEST_PRESENCE;
    TEST_ASSERT_FALSE(OnDemandModule::isPresencePacket(encodeData(request)));
}

static void test_isPresence_otherPortOrGarbage()
{
    TEST_ASSERT_FALSE(
        OnDemandModule::isPresencePacket(encodeData(presenceBroadcast(ONLINE), meshtastic_PortNum_TEXT_MESSAGE_APP)));

    meshtastic_Data garbage = meshtastic_Data_init_zero;
    garbage.portnum = meshtastic_PortNum_FWPLUS_APP;
    const uint8_t bytes[] = {0x22, 0x7f, 0x08}; // response claims 127 bytes, has 1
    memcpy(garbage.payload.bytes, bytes, sizeof(bytes));
    garbage.payload.size = sizeof(bytes);
    TEST_ASSERT_FALSE(OnDemandModule::isPresencePacket(garbage));

    meshtastic_Data empty = meshtastic_Data_init_zero;
    empty.portnum = meshtastic_PortNum_FWPLUS_APP;
    TEST_ASSERT_FALSE(OnDemandModule::isPresencePacket(empty));
}

// The broadcast stays as small as the protocol allows: response tag+len, response_type, presence tag+len, state.
static void test_broadcast_isNineBytes()
{
    TEST_ASSERT_EQUAL_UINT(9, encodeData(presenceBroadcast(ONLINE)).payload.size);
}

void setUp(void) {}
void tearDown(void) {}

extern "C" {
void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_due_afterBoot_waitsSettle);
    RUN_TEST(test_due_sameAsSent_never);
    RUN_TEST(test_due_changeSoonAfterSend_waitsGap);
    RUN_TEST(test_due_changeLongAfterSend_waitsSettleOnly);
    RUN_TEST(test_due_flipRestartsSettle);
    RUN_TEST(test_due_survivesMillisWrap);
    RUN_TEST(test_isPresence_broadcast);
    RUN_TEST(test_isPresence_answer);
    RUN_TEST(test_isPresence_afterSegmentFields);
    RUN_TEST(test_isPresence_otherOnDemandTraffic);
    RUN_TEST(test_isPresence_otherPortOrGarbage);
    RUN_TEST(test_broadcast_isNineBytes);
    exit(UNITY_END());
}

void loop() {}
}
