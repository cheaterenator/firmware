// See UptimeClock.h for the full contract.
#include "UptimeClock.h"
#include "DebugConfiguration.h"
#include <Arduino.h>
#include <atomic>

uint32_t Time::getMillis()
{
#ifdef PIO_UNIT_TESTING
    if (Time::useTestClock.load(std::memory_order_relaxed)) {
        if (Time::testGlitchArmed.exchange(false, std::memory_order_relaxed))
            return Time::testGlitchMs.load(std::memory_order_relaxed);
        return Time::testNowMs.load(std::memory_order_relaxed);
    }
#endif
    return millis();
}

namespace
{
struct PublishedSnapshot {
    std::atomic<uint32_t> high{0};
    std::atomic<uint32_t> low{0};
};

// The constexpr atomic initializers make both snapshots available before firmware startup.
PublishedSnapshot published[2];
std::atomic<uint32_t> publishedGeneration{0};
// Claimed for the duration of one serviceMonotonic() call. THE ONLY WRITER contract (UptimeClock.h)
// means this should never be contended; if it ever is, a second caller exists and serviceMonotonic()
// must refuse the read rather than risk a torn (high, low) pair - see its body below.
std::atomic<bool> writerActive{false};
#ifdef PIO_UNIT_TESTING
std::atomic<Time::MonotonicPublishHook> monotonicPublishHook{nullptr};
#endif

// Above half of the 2^32 range, an elapsed delta reads as `now` having ticked backward (clock-domain
// jitter, or a stale read) rather than a genuine gap this close to a full wrap: the documented
// contract only requires one publish somewhere inside each 49.7-day window, so the latest a
// legitimate gap can land is the halfway point (test_monotonic_counts_every_wrap_when_serviced_each
// _window publishes exactly there). Field report: millis() ticking backward by a few ms wrapped this
// subtraction to just under UINT32_MAX and got folded straight into the carry, permanently adding
// ~49.7 days to every later reading.
constexpr uint32_t kMaxPlausibleAdvanceMs = 0x80000000u;

// serviceMonotonic() re-reads millis() before believing an advance larger than this; see there.
constexpr uint32_t kConfirmAdvanceMs = 1000;

// serviceMonotonic() runs every main-loop iteration and a hold lasts until millis() catches up, so its
// error is logged when a hold starts and then once a minute (a field log caught ~50 identical lines a
// second). Written only by serviceMonotonic(), which writerActive keeps single-threaded.
constexpr uint32_t kHoldLogIntervalMs = 60 * 1000;
bool holding = false;
uint32_t holdLoggedAtMs = 0;
uint32_t heldSinceLog = 0;

// Extend a published (high, low) snapshot to `now`; unsigned subtraction is exact across the wrap
// for any gap under 49.7 days. One copy, because reader and writer must agree on it exactly. An
// elapsed delta past kMaxPlausibleAdvanceMs is held at the published base instead of folded in - see
// test_monotonic_ignores_small_backward_tick_of_millis and
// test_monotonic_reader_ignores_small_backward_tick_without_a_publish.
uint64_t extendPublished(uint32_t high, uint32_t low, uint32_t now)
{
    // (0, 0) is the boot-time reset sentinel, not a real prior reading - nothing has been published
    // yet, so there is no "elapsed since" to be implausible about. `now` becomes the base outright,
    // whatever its value; every later call has a genuine low to measure against.
    if (high == 0 && low == 0)
        return now;
    const uint32_t elapsed = (uint32_t)(now - low);
    if (elapsed > kMaxPlausibleAdvanceMs)
        return ((uint64_t)high << 32) | low;
    return (((uint64_t)high << 32) | low) + elapsed;
}

// A generation change means the writer completed a publish while this copy was being read. A
// paused publish leaves the generation unchanged and writes only the inactive snapshot.
void readPublished(uint32_t &high, uint32_t &low)
{
    for (;;) {
        const uint32_t before = publishedGeneration.load(std::memory_order_acquire);
        PublishedSnapshot &snapshot = published[before & 1u];
        high = snapshot.high.load(std::memory_order_relaxed);
        low = snapshot.low.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (publishedGeneration.load(std::memory_order_relaxed) == before)
            return;
    }
}
} // namespace

uint64_t Time::getMillisMonotonic()
{
    uint32_t high, low;
    readPublished(high, low);
    // The reader writes nothing back; it just extends the last published carry to now.
    return extendPublished(high, low, getMillis());
}

uint32_t Time::getUptimeSecs()
{
    return (uint32_t)(getMillisMonotonic() / 1000);
}

void Time::serviceMonotonic()
{
    bool expected = false;
    if (!writerActive.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        // THE ONLY WRITER contract (UptimeClock.h) was just violated: a second call arrived while
        // the first was still in flight. Reading the active snapshot here anyway is exactly how a
        // wrap gets folded into the carry twice, so drop this call instead - the in-flight call
        // still completes its own publish, and the legitimate caller's next iteration catches up.
        LOG_ERROR("UptimeClock: serviceMonotonic() re-entered concurrently (generation=%u) - "
                  "dropping this call to avoid a torn read of the published snapshot",
                  publishedGeneration.load(std::memory_order_relaxed));
        return;
    }

    const uint32_t generation = publishedGeneration.load(std::memory_order_relaxed);
    PublishedSnapshot &active = published[generation & 1u];
    const uint32_t low = active.low.load(std::memory_order_relaxed);
    const uint32_t high = active.high.load(std::memory_order_relaxed);
    uint32_t now = getMillis();
    const bool everPublished = high != 0 || low != 0; // see extendPublished()'s (0, 0) sentinel case
    if (everPublished && (uint32_t)(now - low) > kConfirmAdvanceMs) {
        // A single millis() read has been seen landing 2^36 us (~19 h) ahead of the reads around it
        // (heltec-v2_1, field log 2026-09-29). Published, it is the base every later, correct read looks
        // backward from, and the clock holds for those 19 h. A real step survives a second read and a
        // glitch does not, so keep whichever read advanced less.
        const uint32_t again = getMillis();
        if ((uint32_t)(again - low) < (uint32_t)(now - low)) {
            const uint32_t ahead = now - again;
            if (ahead > kConfirmAdvanceMs && ahead < kMaxPlausibleAdvanceMs)
                LOG_WARN("UptimeClock: dropped a millis() read %ums ahead of its re-read (%u vs %u)", ahead, now, again);
            now = again;
        }
    }
    const uint32_t elapsed = (uint32_t)(now - low);
    if (everPublished && elapsed > kMaxPlausibleAdvanceMs) {
        // Same bound extendPublished() applies, checked again here so the one safe, low-frequency
        // call site (once per main-loop iteration) can log it - extendPublished() itself must stay
        // silent, since getMillisMonotonic() reaches it from arbitrarily hot, concurrent read paths.
        heldSinceLog++;
        if (!holding || (uint32_t)(now - holdLoggedAtMs) >= kHoldLogIntervalMs) {
            LOG_ERROR("UptimeClock: holding at high=%u low=%u - implausible advance of %ums since last "
                      "publish (now=%u, %u held publishes since the last log), suspected backward tick",
                      high, low, elapsed, now, heldSinceLog);
            holdLoggedAtMs = now;
            heldSinceLog = 0;
        }
        holding = true;
    } else {
        holding = false;
    }
    const uint64_t next = extendPublished(high, low, now);

    PublishedSnapshot &inactive = published[(generation + 1u) & 1u];
    inactive.high.store((uint32_t)(next >> 32), std::memory_order_relaxed);
    inactive.low.store((uint32_t)next, std::memory_order_relaxed);
#ifdef PIO_UNIT_TESTING
    if (const auto hook = monotonicPublishHook.load(std::memory_order_relaxed))
        hook();
#endif
    publishedGeneration.store(generation + 1u, std::memory_order_release);
    writerActive.store(false, std::memory_order_release);
}

#ifdef PIO_UNIT_TESTING
void Time::resetMonotonicForTests()
{
    publishedGeneration.store(0, std::memory_order_relaxed);
    for (auto &snapshot : published) {
        snapshot.high.store(0, std::memory_order_relaxed);
        snapshot.low.store(0, std::memory_order_relaxed);
    }
    writerActive.store(false, std::memory_order_relaxed);
    monotonicPublishHook.store(nullptr, std::memory_order_relaxed);
    testGlitchArmed.store(false, std::memory_order_relaxed);
    holding = false;
    heldSinceLog = 0;
}

void Time::setMonotonicPublishHookForTests(MonotonicPublishHook hook)
{
    monotonicPublishHook.store(hook, std::memory_order_relaxed);
}
#endif
