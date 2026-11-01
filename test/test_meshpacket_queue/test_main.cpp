// Unit tests for MeshPacketQueue::replaceLowerPriorityPacket() - which packet a full queue gives up
// to make room for a new arrival.
//
// tx_after is an absolute millis() deadline, so every decision here has to subtract before comparing
// or it inverts across the 32-bit wrap. The subtlety the first cases pin is that an *elapsed* time
// only orders two deadlines that have both passed: a deadline still in the future subtracts to a
// near-2^32 elapsed, which reads as the most overdue packet in the queue rather than the least.
//
// maxLen is 1 except where a case needs a late and a not-late packet side by side. Any enqueue into
// a full queue reaches the function, and the suite needs no NodeDB: std::upper_bound over an empty
// range never invokes CompareMeshPacketFunc, and where it does the packets differ in lateness or
// priority, or carry from == 0, which isFromUs() answers without asking NodeDB.

#include "Arduino.h"
#include "TestUtil.h"
#include "UptimeClock.h"
#include "configuration.h"
#include "mesh/MeshPacketQueue.h"
#include "mesh/MeshTypes.h"
#include <cstdlib>
#include <unity.h>

namespace
{

// A packet that is only ever a queue occupant: id and tx_after are all the branch reads.
meshtastic_MeshPacket *makePacket(uint32_t id, uint32_t txAfter)
{
    meshtastic_MeshPacket *p = packetPool.allocZeroed();
    TEST_ASSERT_NOT_NULL(p);
    p->id = id;
    p->tx_after = txAfter;
    p->priority = meshtastic_MeshPacket_Priority_DEFAULT;
    return p;
}

// Drains whatever is still queued back to the pool, so a failing case cannot starve a later one.
void drain(MeshPacketQueue &q)
{
    while (meshtastic_MeshPacket *p = q.dequeue())
        packetPool.release(p);
}

} // namespace

void setUp(void)
{
    Time::setTestMillis(0);
}
void tearDown(void)
{
    Time::useRealClock();
}

// The regression: the incoming packet is not due yet, so it must not displace an overdue one.
// `now - p->tx_after` underflows to ~49.7 days of "elapsed", which an unguarded comparison reads as
// the more urgent packet.
static void test_future_incoming_deadline_does_not_evict_an_overdue_packet(void)
{
    Time::setTestMillis(1000);
    MeshPacketQueue q(1);

    meshtastic_MeshPacket *back = makePacket(0x1001, 900);   // 100ms overdue
    meshtastic_MeshPacket *fresh = makePacket(0x1002, 1100); // 100ms in the future
    TEST_ASSERT_TRUE(q.enqueue(back));

    TEST_ASSERT_FALSE(q.enqueue(fresh));
    TEST_ASSERT_EQUAL_HEX32(0x1001, q.getFront()->id);

    packetPool.release(fresh);
    drain(q);
}

// The ordering the branch does want: both deadlines have passed and the arrival is the more overdue
// of the two, so the queued packet gives up its slot.
static void test_more_overdue_incoming_packet_evicts_the_late_back_packet(void)
{
    Time::setTestMillis(1000);
    MeshPacketQueue q(1);

    meshtastic_MeshPacket *back = makePacket(0x2001, 900);  // 100ms overdue
    meshtastic_MeshPacket *fresh = makePacket(0x2002, 800); // 200ms overdue
    TEST_ASSERT_TRUE(q.enqueue(back));

    TEST_ASSERT_TRUE(q.enqueue(fresh)); // back is released by the queue
    TEST_ASSERT_EQUAL_HEX32(0x2002, q.getFront()->id);

    drain(q);
}

// The other half of that ordering: a less overdue arrival leaves the queue alone.
static void test_less_overdue_incoming_packet_is_rejected(void)
{
    Time::setTestMillis(1000);
    MeshPacketQueue q(1);

    meshtastic_MeshPacket *back = makePacket(0x3001, 800);  // 200ms overdue
    meshtastic_MeshPacket *fresh = makePacket(0x3002, 900); // 100ms overdue
    TEST_ASSERT_TRUE(q.enqueue(back));

    TEST_ASSERT_FALSE(q.enqueue(fresh));
    TEST_ASSERT_EQUAL_HEX32(0x3001, q.getFront()->id);

    packetPool.release(fresh);
    drain(q);
}

// An arrival with no TX delay at all always wins the slot from an overdue packet.
static void test_undelayed_incoming_packet_evicts_the_late_back_packet(void)
{
    Time::setTestMillis(1000);
    MeshPacketQueue q(1);

    meshtastic_MeshPacket *back = makePacket(0x4001, 900);
    meshtastic_MeshPacket *fresh = makePacket(0x4002, 0); // no tx_after
    TEST_ASSERT_TRUE(q.enqueue(back));

    TEST_ASSERT_TRUE(q.enqueue(fresh));
    TEST_ASSERT_EQUAL_HEX32(0x4002, q.getFront()->id);

    drain(q);
}

// Both deadlines were set before the wrap and `now` is after it, so every raw comparison in the
// branch inverts. The decisions must come out the same as they do away from the boundary.
static void test_decisions_survive_the_millis_wrap(void)
{
    // 0xFFFFFF00 and 0xFFFFFE00 are 256ms and 512ms before the wrap; now is 256ms after it.
    Time::setTestMillis(0x00000100);
    MeshPacketQueue q(1);

    meshtastic_MeshPacket *back = makePacket(0x5001, 0xFFFFFF00);  // 512ms overdue
    meshtastic_MeshPacket *older = makePacket(0x5002, 0xFFFFFE00); // 768ms overdue
    TEST_ASSERT_TRUE(q.enqueue(back));
    TEST_ASSERT_TRUE(q.enqueue(older));
    TEST_ASSERT_EQUAL_HEX32(0x5002, q.getFront()->id);
    drain(q);

    // ...and a not-yet-due arrival still loses, with the deadline on the far side of the wrap.
    MeshPacketQueue q2(1);
    meshtastic_MeshPacket *back2 = makePacket(0x5003, 0xFFFFFF00); // 512ms overdue
    meshtastic_MeshPacket *fresh = makePacket(0x5004, 0x00000300); // 512ms in the future
    TEST_ASSERT_TRUE(q2.enqueue(back2));

    TEST_ASSERT_FALSE(q2.enqueue(fresh));
    TEST_ASSERT_EQUAL_HEX32(0x5003, q2.getFront()->id);

    packetPool.release(fresh);
    drain(q2);
}

// A late packet another node has already relayed gives way to one nobody has, of the same priority
// or higher, even while it is still inside its window. Only an overdue back packet used to qualify,
// and the back is the packet clamped last: on a busy ROUTER_LATE that kept the queue full and
// dropped every arrival, ACKs included.
static void test_undelayed_packet_evicts_a_late_packet_still_inside_its_window(void)
{
    Time::setTestMillis(1000);
    MeshPacketQueue q(1);

    meshtastic_MeshPacket *late = makePacket(0x6001, 5000); // 4s still to wait
    TEST_ASSERT_TRUE(q.enqueue(late));

    meshtastic_MeshPacket *fresh = makePacket(0x6002, 0); // same priority, nobody's relay yet
    TEST_ASSERT_TRUE(q.enqueue(fresh));
    TEST_ASSERT_EQUAL_HEX32(0x6002, q.getFront()->id);
    drain(q);

    MeshPacketQueue q2(1);
    TEST_ASSERT_TRUE(q2.enqueue(makePacket(0x6003, 5000)));
    meshtastic_MeshPacket *ack = makePacket(0x6004, 0);
    ack->priority = meshtastic_MeshPacket_Priority_ACK;
    TEST_ASSERT_TRUE(q2.enqueue(ack));
    TEST_ASSERT_EQUAL_HEX32(0x6004, q2.getFront()->id);
    drain(q2);
}

// ...but never to a less important packet, overdue or not.
static void test_late_packet_is_kept_for_a_lower_priority_arrival(void)
{
    Time::setTestMillis(1000);
    MeshPacketQueue q(1);

    meshtastic_MeshPacket *late = makePacket(0x7001, 900); // overdue
    late->priority = meshtastic_MeshPacket_Priority_HIGH;
    TEST_ASSERT_TRUE(q.enqueue(late));

    meshtastic_MeshPacket *fresh = makePacket(0x7002, 0);
    TEST_ASSERT_FALSE(q.enqueue(fresh));
    TEST_ASSERT_EQUAL_HEX32(0x7001, q.getFront()->id);

    packetPool.release(fresh);
    drain(q);
}

// With room made for a higher-priority arrival, the late packet goes before the one nobody has
// relayed yet. The old search for a lower-priority packet that was not late ran first, so it
// dropped the only copy the mesh might get and kept the second one.
static void test_late_packet_is_evicted_before_a_packet_nobody_has_relayed(void)
{
    Time::setTestMillis(1000);
    MeshPacketQueue q(2);

    TEST_ASSERT_TRUE(q.enqueue(makePacket(0x8001, 0)));    // not late
    TEST_ASSERT_TRUE(q.enqueue(makePacket(0x8002, 5000))); // late, inside its window

    meshtastic_MeshPacket *high = makePacket(0x8003, 0);
    high->priority = meshtastic_MeshPacket_Priority_HIGH;
    TEST_ASSERT_TRUE(q.enqueue(high));

    meshtastic_MeshPacket *first = q.dequeue();
    meshtastic_MeshPacket *second = q.dequeue();
    TEST_ASSERT_EQUAL_HEX32(0x8003, first->id);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0x8001, second->id, "the packet nobody has relayed must stay");
    TEST_ASSERT_TRUE(q.empty());

    packetPool.release(first);
    packetPool.release(second);
}

void setup()
{
    delay(10);
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_future_incoming_deadline_does_not_evict_an_overdue_packet);
    RUN_TEST(test_more_overdue_incoming_packet_evicts_the_late_back_packet);
    RUN_TEST(test_less_overdue_incoming_packet_is_rejected);
    RUN_TEST(test_undelayed_incoming_packet_evicts_the_late_back_packet);
    RUN_TEST(test_decisions_survive_the_millis_wrap);
    RUN_TEST(test_undelayed_packet_evicts_a_late_packet_still_inside_its_window);
    RUN_TEST(test_late_packet_is_kept_for_a_lower_priority_arrival);
    RUN_TEST(test_late_packet_is_evicted_before_a_packet_nobody_has_relayed);
    exit(UNITY_END());
}

void loop() {}
