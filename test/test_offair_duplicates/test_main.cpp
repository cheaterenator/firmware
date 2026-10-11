// A packet that crosses UDP multicast or MQTT reaches a peer ahead of, or behind, the originator's own
// LoRa transmission. Neither ordering may change what goes on the air:
//
//   repeat detection   a zero-hop copy counts as the originator retransmitting only when it came over
//                      LoRa and an earlier copy did too - otherwise the peer relays the same packet
//                      twice (once per path), for decodable and opaque frames alike
//   own packets        a LoRa relay of our packet heard before our own transmission went out (the peer
//                      got it off-air) must not cancel that transmission
//
// Each case also runs its pure-LoRa control, so a fix that simply stops relaying fails here too.

#include "MeshTypes.h" // BEFORE TestUtil.h
#include "TestUtil.h"
#include <unity.h>

#if !(MESHTASTIC_EXCLUDE_PKI) && !(MESHTASTIC_EXCLUDE_XEDDSA)

#include "support/AuthPipelineHarness.h"

static constexpr meshtastic_MeshPacket_TransportMechanism RX_LORA = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_LORA;
static constexpr meshtastic_MeshPacket_TransportMechanism RX_UDP =
    meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MULTICAST_UDP;
static constexpr meshtastic_MeshPacket_TransportMechanism RX_MQTT = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_MQTT;

void setUp(void)
{
    pipelineHarnessSetUp();
}

void tearDown(void)
{
    pipelineHarnessTearDown();
}

/// A channel broadcast as the originator put it on the air: hop_start == hop_limit, zero hops.
static meshtastic_MeshPacket makeOriginal(NodeNum from, PacketId id)
{
    meshtastic_MeshPacket p = makeChannelBroadcastFrom(from, NODENUM_BROADCAST, id);
    p.hop_start = 3;
    p.hop_limit = 3;
    return p;
}

static meshtastic_MeshPacket via(meshtastic_MeshPacket p, meshtastic_MeshPacket_TransportMechanism transport)
{
    p.transport_mechanism = transport;
    p.via_mqtt = transport == RX_MQTT;
    return p;
}

// ---------------------------------------------------------------------------
// Repeat detection, decodable broadcasts
// ---------------------------------------------------------------------------

void test_lora_original_after_udp_copy_is_not_relayed_again(void)
{
    const meshtastic_MeshPacket p = makeOriginal(REMOTE_NODE, 0x0FA10001);

    runPipelineIngress(via(p, RX_UDP));
    TEST_ASSERT_EQUAL_MESSAGE(1, pipelineRadio->sentCountFor(REMOTE_NODE, p.id), "the UDP copy is new to us: relay it");

    runPipelineIngress(via(p, RX_LORA));
    TEST_ASSERT_EQUAL_MESSAGE(1, pipelineRadio->sentCountFor(REMOTE_NODE, p.id),
                              "the originator's first transmission, heard after its UDP copy, is no retransmission");
}

void test_offair_copy_after_lora_original_is_not_relayed_again(void)
{
    const meshtastic_MeshPacket udp = makeOriginal(REMOTE_NODE, 0x0FA10002);
    runPipelineIngress(via(udp, RX_LORA));
    runPipelineIngress(via(udp, RX_UDP));
    TEST_ASSERT_EQUAL_MESSAGE(1, pipelineRadio->sentCountFor(REMOTE_NODE, udp.id),
                              "a UDP copy of a transmission we already carried is no retransmission");

    const meshtastic_MeshPacket mqtt = makeOriginal(REMOTE_NODE, 0x0FA10003);
    runPipelineIngress(via(mqtt, RX_LORA));
    runPipelineIngress(via(mqtt, RX_MQTT));
    TEST_ASSERT_EQUAL_MESSAGE(1, pipelineRadio->sentCountFor(REMOTE_NODE, mqtt.id),
                              "an MQTT downlink of a transmission we already carried is no retransmission");
}

// Control: with LoRa on both sides this is the originator retrying, and it is carried again.
void test_originator_retransmission_over_lora_is_relayed_again(void)
{
    const meshtastic_MeshPacket p = makeOriginal(REMOTE_NODE, 0x0FA10004);
    runPipelineIngress(via(p, RX_LORA));
    runPipelineIngress(via(p, RX_LORA));
    TEST_ASSERT_EQUAL_MESSAGE(2, pipelineRadio->sentCountFor(REMOTE_NODE, p.id),
                              "the originator's own retransmission is carried again");

    // An off-air copy in between does not hide the LoRa history.
    const meshtastic_MeshPacket q = makeOriginal(REMOTE_NODE, 0x0FA10005);
    runPipelineIngress(via(q, RX_LORA));
    runPipelineIngress(via(q, RX_UDP));
    runPipelineIngress(via(q, RX_LORA));
    TEST_ASSERT_EQUAL_MESSAGE(2, pipelineRadio->sentCountFor(REMOTE_NODE, q.id),
                              "a second LoRa copy after a UDP one is still the originator retrying");
}

// The originator's first transmission is not someone else's rebroadcast either: a relay we queued
// from the UDP copy stays queued.
void test_lora_original_after_udp_copy_keeps_our_queued_relay(void)
{
    const meshtastic_MeshPacket p = makeOriginal(REMOTE_NODE, 0x0FA10006);
    runPipelineIngress(via(p, RX_UDP));
    pipelineRadio->holdInTxQueue(REMOTE_NODE, p.id); // our relay has not gone out yet

    runPipelineIngress(via(p, RX_LORA));
    TEST_ASSERT_TRUE_MESSAGE(pipelineRadio->txQueueHolds(REMOTE_NODE, p.id), "our relay must still go out");
    TEST_ASSERT_EQUAL(0, pipelineRadio->cancelCalls);
}

// ---------------------------------------------------------------------------
// Own packets
// ---------------------------------------------------------------------------

void test_own_packet_is_not_cancelled_by_an_offair_peers_relay(void)
{
    // We sent it (history records it the way FloodingRouter::send does) and it is still queued; a
    // UDP/MQTT peer got it off-air and its LoRa relay reaches us first.
    meshtastic_MeshPacket own = makeOriginal(LOCAL_NODE, 0x0FA10007);
    const meshtastic_MeshPacket peersRelay = makeRelayedCopy(own); // over LoRa, one hop spent
    own.transport_mechanism = meshtastic_MeshPacket_TransportMechanism_TRANSPORT_API;
    own.relay_node = nodeDB->getLastByteOfNodeNum(LOCAL_NODE);
    pipelineRouter->remember(&own);
    pipelineRadio->holdInTxQueue(LOCAL_NODE, own.id);

    runPipelineIngress(peersRelay);
    TEST_ASSERT_EQUAL_MESSAGE(1, pipelineRouter->rxDupe, "precondition: the relay reached the duplicate filter");
    TEST_ASSERT_TRUE_MESSAGE(pipelineRadio->txQueueHolds(LOCAL_NODE, own.id),
                             "our own first transmission must still go out to neighbours out of the peer's range");
    TEST_ASSERT_EQUAL(0, pipelineRouter->txRelayCanceled);
}

// Control: a relay we queued for someone else is still cancelled by a neighbour's rebroadcast.
void test_queued_relay_is_still_cancelled_by_a_neighbours_rebroadcast(void)
{
    const meshtastic_MeshPacket p = makeChannelBroadcastFrom(REMOTE_NODE, NODENUM_BROADCAST, 0x0FA10008);
    runPipelineIngress(p);
    pipelineRadio->holdInTxQueue(REMOTE_NODE, p.id);

    runPipelineIngress(makeRelayedCopy(p));
    TEST_ASSERT_FALSE_MESSAGE(pipelineRadio->txQueueHolds(REMOTE_NODE, p.id), "a CLIENT drops its relay once another is heard");
    TEST_ASSERT_EQUAL(1, pipelineRouter->txRelayCanceled);
}

// ---------------------------------------------------------------------------
// Repeat detection, opaque frames (relayOpaquePacket keeps its own dedup ring)
// ---------------------------------------------------------------------------

void test_opaque_lora_original_after_udp_copy_is_not_relayed_again(void)
{
    const RelayIdentity admin = makeIdentity(ADMIN_NODE);
    const RelayIdentity target = makeIdentity(TARGET_NODE);
    meshtastic_MeshPacket p = makePkiUnicastBetween(admin, target, meshtastic_PortNum_ADMIN_APP, 0x0FA10009);
    p.hop_limit = p.hop_start;

    runPipelineIngress(via(p, RX_UDP));
    runPipelineIngress(via(p, RX_LORA));
    TEST_ASSERT_EQUAL_MESSAGE(1, pipelineRadio->sentCountFor(ADMIN_NODE, p.id),
                              "an opaque frame is carried once whichever path delivers it first");

    meshtastic_MeshPacket q = makePkiUnicastBetween(admin, target, meshtastic_PortNum_ADMIN_APP, 0x0FA1000A);
    q.hop_limit = q.hop_start;
    runPipelineIngress(via(q, RX_LORA));
    runPipelineIngress(via(q, RX_LORA));
    TEST_ASSERT_EQUAL_MESSAGE(2, pipelineRadio->sentCountFor(ADMIN_NODE, q.id),
                              "control: the originator retrying over LoRa is carried again");
}

void setup()
{
    pipelineHarnessCreate();
    UNITY_BEGIN();
    RUN_TEST(test_lora_original_after_udp_copy_is_not_relayed_again);
    RUN_TEST(test_offair_copy_after_lora_original_is_not_relayed_again);
    RUN_TEST(test_originator_retransmission_over_lora_is_relayed_again);
    RUN_TEST(test_lora_original_after_udp_copy_keeps_our_queued_relay);
    RUN_TEST(test_own_packet_is_not_cancelled_by_an_offair_peers_relay);
    RUN_TEST(test_queued_relay_is_still_cancelled_by_a_neighbours_rebroadcast);
    RUN_TEST(test_opaque_lora_original_after_udp_copy_is_not_relayed_again);
    const int result = UNITY_END();
    pipelineHarnessDestroy();
    exit(result);
}

void loop() {}

#else // XEdDSA or PKI excluded

void setUp(void) {}
void tearDown(void) {}
void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    exit(UNITY_END());
}
void loop() {}

#endif
