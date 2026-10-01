// Unit tests for LogRecordQueue (src/mesh/LogRecordQueue.cpp), the ring that carries debug-log
// LogRecords from RedirectablePrint::log() - on any task - to the TCP API session that sends them.
//
// What these pin: records come out whole and in order, including when a record's length header or
// payload straddles the end of the ring; a record that does not fit is refused and counted rather
// than overwriting queued data; a queue with no ring (option off, or allocation refused) ignores
// pushes without counting them as drops; and a record too large for the caller's buffer is consumed
// instead of being left at the head.
//
// Regressions guarded: a wrap bug here puts a torn FromRadio frame on the wire, which the client
// reports as a DecodeError and which desynchronizes the 0x94C3 framing for every later packet on
// that TCP session. A record left at the head because pop() refused it would stall the log stream
// for the rest of the session while the queue filled and every new line was dropped.
#include "TestUtil.h"
#include "mesh/LogRecordQueue.h"
#include <cstdint>
#include <unity.h>
#include <vector>

/// Pop one record into a buffer large enough for any test payload.
static std::vector<uint8_t> popRecord(LogRecordQueue &queue)
{
    uint8_t out[64];
    size_t len = queue.pop(out, sizeof(out));
    return std::vector<uint8_t>(out, out + len);
}

/// Assert a popped record matches the pushed bytes exactly.
static void assertRecord(const std::vector<uint8_t> &expected, const std::vector<uint8_t> &actual)
{
    TEST_ASSERT_EQUAL_UINT(expected.size(), actual.size());
    if (!expected.empty())
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.data(), actual.data(), expected.size());
}

void setUp(void) {}
void tearDown(void) {}

void test_records_come_out_in_order()
{
    LogRecordQueue queue;
    TEST_ASSERT_TRUE(queue.begin(64));
    std::vector<uint8_t> a = {1, 2, 3}, b = {4}, c = {5, 6, 7, 8, 9};

    TEST_ASSERT_TRUE(queue.push(a.data(), a.size()));
    TEST_ASSERT_TRUE(queue.push(b.data(), b.size()));
    TEST_ASSERT_TRUE(queue.push(c.data(), c.size()));

    assertRecord(a, popRecord(queue));
    assertRecord(b, popRecord(queue));
    assertRecord(c, popRecord(queue));
    TEST_ASSERT_TRUE(queue.isEmpty());
    TEST_ASSERT_EQUAL_UINT(0, queue.pop(nullptr, 0));
}

// Walk the write position through every offset of a small ring so the 2-byte header and the payload
// each land split across the end at least once.
void test_records_straddling_the_ring_end_stay_intact()
{
    LogRecordQueue queue;
    TEST_ASSERT_TRUE(queue.begin(11));
    for (uint8_t round = 0; round < 40; round++) {
        std::vector<uint8_t> payload = {round, (uint8_t)(round + 1), (uint8_t)(round + 2), (uint8_t)(round + 3), 0xA5};
        TEST_ASSERT_TRUE(queue.push(payload.data(), payload.size()));
        assertRecord(payload, popRecord(queue));
        TEST_ASSERT_TRUE(queue.isEmpty());
    }
    TEST_ASSERT_EQUAL_UINT32(0, queue.takeDropped());
}

void test_record_that_fills_the_ring_exactly_is_accepted()
{
    LogRecordQueue queue;
    TEST_ASSERT_TRUE(queue.begin(10));
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8};

    TEST_ASSERT_TRUE(queue.push(payload.data(), payload.size()));
    TEST_ASSERT_FALSE(queue.push(payload.data(), 0)); // even an empty record needs its header
    assertRecord(payload, popRecord(queue));
    TEST_ASSERT_TRUE(queue.isEmpty());
    TEST_ASSERT_EQUAL_UINT32(1, queue.takeDropped());
}

// Newest is dropped, oldest kept: queued bytes are never overwritten, and the count lets the session
// report the gap once the backlog clears.
void test_full_queue_drops_new_record_and_counts_it()
{
    LogRecordQueue queue;
    TEST_ASSERT_TRUE(queue.begin(10));
    std::vector<uint8_t> first = {1, 2, 3, 4, 5, 6}, second = {7, 8, 9};

    TEST_ASSERT_TRUE(queue.push(first.data(), first.size()));
    TEST_ASSERT_FALSE(queue.push(second.data(), second.size()));
    TEST_ASSERT_FALSE(queue.push(second.data(), second.size()));

    TEST_ASSERT_EQUAL_UINT32(2, queue.takeDropped());
    TEST_ASSERT_EQUAL_UINT32(0, queue.takeDropped());
    assertRecord(first, popRecord(queue));
    TEST_ASSERT_TRUE(queue.isEmpty());

    TEST_ASSERT_TRUE(queue.push(second.data(), second.size()));
    assertRecord(second, popRecord(queue));
}

// With debug_log_api_enabled off, or its allocation refused, the session never begins the queue;
// log lines reaching it then are not losses and must not surface as a "dropped" warning.
void test_queue_without_ring_ignores_pushes()
{
    LogRecordQueue queue;
    std::vector<uint8_t> payload = {1, 2};

    TEST_ASSERT_FALSE(queue.isActive());
    TEST_ASSERT_FALSE(queue.push(payload.data(), payload.size()));
    TEST_ASSERT_TRUE(queue.isEmpty());
    TEST_ASSERT_EQUAL_UINT32(0, queue.takeDropped());

    TEST_ASSERT_TRUE(queue.begin(16));
    TEST_ASSERT_TRUE(queue.push(payload.data(), payload.size()));
    queue.end();
    TEST_ASSERT_FALSE(queue.isActive());
    TEST_ASSERT_TRUE(queue.isEmpty());
    TEST_ASSERT_FALSE(queue.push(payload.data(), payload.size()));
    TEST_ASSERT_EQUAL_UINT32(0, queue.takeDropped());
}

void test_begin_rejects_ring_too_small_for_a_record()
{
    LogRecordQueue queue;
    TEST_ASSERT_FALSE(queue.begin(0));
    TEST_ASSERT_FALSE(queue.begin(2));
    TEST_ASSERT_FALSE(queue.isActive());
    TEST_ASSERT_TRUE(queue.begin(3));
}

void test_record_larger_than_pop_buffer_is_consumed_not_stuck()
{
    LogRecordQueue queue;
    TEST_ASSERT_TRUE(queue.begin(32));
    std::vector<uint8_t> big = {1, 2, 3, 4, 5}, next = {6};
    TEST_ASSERT_TRUE(queue.push(big.data(), big.size()));
    TEST_ASSERT_TRUE(queue.push(next.data(), next.size()));

    uint8_t small[3];
    TEST_ASSERT_EQUAL_UINT(0, queue.pop(small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32(1, queue.takeDropped());
    assertRecord(next, popRecord(queue));
    TEST_ASSERT_TRUE(queue.isEmpty());
}

void setup()
{
    initializeTestEnvironment();
    UNITY_BEGIN();
    RUN_TEST(test_records_come_out_in_order);
    RUN_TEST(test_records_straddling_the_ring_end_stay_intact);
    RUN_TEST(test_record_that_fills_the_ring_exactly_is_accepted);
    RUN_TEST(test_full_queue_drops_new_record_and_counts_it);
    RUN_TEST(test_queue_without_ring_ignores_pushes);
    RUN_TEST(test_begin_rejects_ring_too_small_for_a_record);
    RUN_TEST(test_record_larger_than_pop_buffer_is_consumed_not_stuck);
    exit(UNITY_END());
}

void loop() {}
