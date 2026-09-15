#pragma once

#include "MeshRadio.h"
#include "concurrency/Lock.h"
#include "concurrency/LockGuard.h"
#include "concurrency/OSThread.h"
#include "configuration.h"
#include <Arduino.h>
#include <functional>

/*
  AirTime records how long the radio was busy and turns that into the two
  percentages the transmit gates and DeviceMetrics use.

  INPUTS - four events change this class's state:

    logAirtime(TX_LOG, ms)      one per completed transmission, ours and relayed
    logAirtime(RX_LOG, ms)      one per well-formed reception. The interface is
                                promiscuous: this counts packets not addressed
                                to us, and every duplicate relay copy.
    logAirtime(RX_ALL_LOG, ms)  one per reception that could NOT be parsed -
                                failed CRC, truncated, region unset, collision
    elapsed time                Time::getUptimeSecs(), read by syncNow() on
                                every public entry point. The only input that
                                removes airtime.

  RX_LOG and RX_ALL_LOG are DISJOINT, and a reception logs AT MOST one of them.
  RX_ALL_LOG is unparseable airtime, not a superset of RX_LOG, so the total is
  TX + RX + RX_ALL - but it under-counts: five drop paths log neither. A packet
  with from == 0 returns unlogged from handleReceiveInterrupt(), unlike every
  neighbouring drop, and SimRadio drops a collision during transmission plus
  three allocation failures. Pre-existing; see the TODO below.

  OUTPUTS:

    channelUtilizationPercent()  % of the last 60s busy, all three types
    smoothedChannelUtilizationPercent()
                                 the same, behind a ~21 min EMA folded per bucket
    utilizationTXPercent()       % of the last hour we transmitted
    isTxAllowedChannelUtil()     gate on the former, 40% or 25% "polite"
    isTxAllowedAirUtil()         gate on the latter, at HALF the duty cycle
    getSilentMinutes()           minutes until the TX figure clears a limit.
                                 Feeds a log line and a client notification; it
                                 gates nothing.
    airtimeReport()              8 x 1h of raw ms per type, for the HTTP report
    getSecondsSinceBoot()        the clock the buckets are keyed to

  The three thresholds are hard-coded members with no config binding.

  STORAGE - two orderings, easily confused:

    channelUtilization[], utilizationTX[]
        Modular rings indexed by absolute uptime phase, (secs / p) % N. The
        index is NOT an age; the oldest bucket is (current + 1) % N. Crossing
        into a bucket zeroes it.

    airtimes.period{TX,RX,RX_ALL}[]
        Shift-ordered, slot 0 newest, index IS age in hours. Slot 0 is a partial
        hour; normalise it by getSecondsSinceBoot() % getSecondsPerPeriod().

  The percentages measure wall time, not time awake. A light-sleeping node still
  hears traffic, and reporting over observed time would make two nodes'
  broadcast readings incomparable.

  channelUtilization spans 60s but reaches the mesh at >= 1h cadence, so remote
  readings are a snapshot rather than an average. Its contention-window consumer
  moves in 20-percentage-point steps, map(chanutil, 0, 100, CWmin, CWmax), so
  small errors never reach the backoff.

  Rotation happens on access, not on the scheduler tick: every public method
  calls syncNow() first and runOnce() only guarantees once a second. A
  scheduler-driven window stops advancing during light sleep. Enforced by
  test_channel_utilization_is_independent_of_scheduler_rate.

  TODO: airtime accuracy. Four known defects remain - the quantised denominator,
  its sawtooth, whole-packet attribution to the completing bucket, and
  getSilentMinutes() reading a modular ring as if the index were an age. Each is
  pinned by a test tagged CHARACTERISATION in test/test_airtime.
*/

#define CHANNEL_UTILIZATION_PERIODS 6
// EMA weight per crossed 10 s bucket: 1/128 is a time constant of about 21 minutes, so one busy
// or quiet minute cannot move the smoothed figure far.
#define CHANNEL_UTILIZATION_EMA_DIVISOR 128
#define SECONDS_PER_PERIOD 3600
#define PERIODS_TO_LOG 8
#define MINUTES_IN_HOUR 60
#define SECONDS_IN_MINUTE 60
#define MS_IN_MINUTE (SECONDS_IN_MINUTE * 1000)
#define MS_IN_HOUR (MINUTES_IN_HOUR * SECONDS_IN_MINUTE * 1000)

// MT-SW: 10-minute-granularity activity window, ported from the historical fw+ AirTime (which kept it
// unlocked and rotated it off a per-second OSThread tick - see Windows::syncNow()'s comment below for
// why that rotation trigger was replaced). Deliberately finer-grained and shorter-range than
// airtimes.period{TX,RX,RX_ALL}[] above (10 x 10min = ~100min vs 8 x 1h), and carries idle_time, which
// the hourly view does not.
#define RX_WINDOW_INTERVAL_SECONDS 600 // fw+ 10min
#define ACTIVITY_WINDOW_COUNT 10       // fw+ 10 x 10min windows
// fw+: average ms of airtime per RX_LOG-or-RX_ALL_LOG packet, over each completed 10-minute window.
// Same RX_WINDOW_INTERVAL_SECONDS cadence as activityWindow above, but a longer independent history -
// 40 windows (~6.67h) vs activityWindow's 10 (~100min). Rolls over in the same syncNow() pass as
// activityWindow, but its own bulk-clear threshold must stay independent: sleeping through, say, 15
// windows clears activityWindow (>= 10) while rxWindowAverages (>= 40) still shifts normally.
#define RX_WINDOW_COUNT 40 // fw+ 40 x 10min probes
// fw+: count of RX+TX mesh packets (not ms of airtime) per completed 10-minute window - originally a
// separate PacketCounter class fed from Router::sniffReceived()/Router::send(), folded in here to
// reuse the same lock and syncNow() rollover as activityWindow/rxWindowAverages above instead of
// building a second independent locking scheme for one more 10-minute-windowed counter. See
// AirTime::logPacketSeen(): unlike everything else in this class, it is NOT called from logAirtime()
// (packet counting, not airtime duration) - Router calls it directly.
#define RXTXALL_ACTIVITY_COUNT 40 // fw+ 40 x 10min packet-count windows

enum reportTypes { TX_LOG, RX_LOG, RX_ALL_LOG };

// fw+: one completed RX_WINDOW_INTERVAL_SECONDS window's worth of tx/rx/rx_bad/idle time, in ms.
// idle_time has no wire representation in meshtastic_AirActivityEntry (ondemand.proto) - it is
// internal bookkeeping only, not currently exposed over OnDemand.
struct ActivityTime {
    uint32_t rx_time;
    uint32_t tx_time;
    uint32_t idle_time;
    uint32_t rx_bad_time;
};

// Arms AirTime's nested-take check. Sound only where the lock is not a real lock: the check runs
// before the take, because a nested take blocks forever and a later check would never run - so
// under preemption it would false-positive on legitimate contention and race on its own write.
// Portduino is where it earns its keep anyway; there Lock::lock() is empty, so a nested take
// succeeds silently and nothing else would notice. On an on-target test build the nesting it
// catches shows up as a hang instead. Test builds only: nothing in this tree defines DEBUG or
// NDEBUG, so either spelling would ship an abort() to every board, and nrf52_promicro_diy_tcxo
// has no flash for it.
#if defined(PIO_UNIT_TESTING) && !defined(HAS_FREE_RTOS)
#define AIRTIME_REENTRY_CHECK
#endif

// Serialised behind `lock` because two FreeRTOS tasks genuinely reach this class at once on nRF52.
// NRF52Bluetooth registers its ToRadio write callback with defer == false, so a phone's packet runs
// PhoneAPI::handleToRadio -> MeshService::sendToMesh -> Router::send on the Bluefruit BLE task,
// which reads utilizationTXPercent() and getSilentMinutes() while loopTask may be inside
// logAirtime() from a reception. That is an unsynchronised read-modify-write of utilizationTX[] and
// secSinceBoot against a summing read. ESP32 hands BLE work to the main task and does not have it.
//
// Two mechanisms keep it serialised:
//
//   - a lock-free inner core (Windows) holds all state and all logic. It has no lock member, and
//     must never reach one through the global `airTime` - `airTime->anyPublicMethod()` from inside
//     a Windows method would take a second Held and hang, because concurrency::Lock is a
//     non-recursive binary semaphore taken with portMAX_DELAY. Nothing does this today; the
//     AIRTIME_REENTRY_CHECK assert is the backstop, and it only builds on host test builds.
//   - a private Held token takes the lock in its constructor and is the only thing that satisfies a
//     core method's `const Held &`, so the lock cannot be forgotten.
//
// Every public method takes the lock exactly once and delegates, with two exceptions: the two
// constexpr accessors below touch no state and take none, and isTxAllowedAirUtil() takes it zero or
// one times, depending on whether the duty-cycle branch is entered at all. Nothing inside locks -
// that includes isTxAllowed*(), which call the core rather than the public accessors.
//
// A new write-path helper belongs to Windows or is a free function, never a method on AirTime: an
// AirTime method locks, and logAirtime() would call it while already holding the lock.
class AirTime : private concurrency::OSThread
{

  public:
    AirTime();

    void logAirtime(reportTypes reportType, uint32_t airtime_ms);
    float channelUtilizationPercent();
    /// channelUtilizationPercent() behind an EMA advanced by elapsed time, for a caller that must
    /// judge load from a trend rather than from one 60-second window.
    float smoothedChannelUtilizationPercent();
    float utilizationTXPercent();

    /// Compatibility shim: no caller in the tree, kept for out-of-tree ones.
    void airtimeRotatePeriod();
    /// Constants, not state: no lock, and usable where a constant expression is required so a
    /// caller's buffer and the count it passes to airtimeReport() cannot drift apart.
    static constexpr uint8_t getPeriodsToLog() { return PERIODS_TO_LOG; }
    static constexpr uint32_t getSecondsPerPeriod() { return SECONDS_PER_PERIOD; }
    uint32_t getSecondsSinceBoot();
    /// Copies `count` buckets into `out`, newest first. Copies rather than returning the array so a
    /// caller cannot hold a handle to buckets that every other entry point rotates underneath it.
    /// False if `out` is null, `count` exceeds the log depth, or the report type is unknown.
    bool airtimeReport(reportTypes reportType, uint32_t *out, size_t count);
    uint8_t getSilentMinutes(float txPercent, float dutyCycle);
    bool isTxAllowedChannelUtil(bool polite = false);
    bool isTxAllowedAirUtil();

    /// Constant, not state - see getPeriodsToLog().
    static constexpr uint8_t getActivityWindowCount() { return ACTIVITY_WINDOW_COUNT; }
    /// Copies `count` 10-minute activity windows into `out`, oldest first (matches activityWindow's
    /// own shift-and-append order). Same copy-out contract as airtimeReport().
    bool activityWindowReport(ActivityTime *out, size_t count);

    /// Constant, not state - see getPeriodsToLog().
    static constexpr uint8_t getRxWindowCount() { return RX_WINDOW_COUNT; }
    /// Copies `count` 10-minute RX-average-airtime windows into `out`, oldest first. Same copy-out
    /// contract as airtimeReport()/activityWindowReport().
    bool rxWindowAveragesReport(uint32_t *out, size_t count);

    /// Count one RX+TX mesh packet into the current 10-minute window. Call once per packet from
    /// Router::sniffReceived() (overheard RX) and Router::send() (our TX) - NOT from logAirtime(),
    /// which this does not call and is not called from.
    void logPacketSeen();
    /// Constant, not state - see getPeriodsToLog().
    static constexpr uint8_t getPacketCountWindowSize() { return RXTXALL_ACTIVITY_COUNT; }
    /// Copies `count` 10-minute RX+TX packet-count windows into `out`, oldest first. Same copy-out
    /// contract as the other *Report() methods.
    bool packetCountWindowReport(uint32_t *out, size_t count);

  private:
    concurrency::Lock lock;

#ifdef AIRTIME_REENTRY_CHECK
    // Set for the lifetime of a Held and checked before the lock is taken, so a nested take is
    // reported rather than hung at. See the macro's definition for why it is host-only.
    bool reentryFlag = false;
#endif

    /// Takes `lock` for its lifetime and doubles as proof that it is held. Only AirTime can
    /// construct one, so a core method taking `const Held &` cannot be called without the lock.
    /// A bare LockGuard would not do: it proves only that *some* lock is held.
    class Held
    {
      public:
        explicit Held(AirTime *a) : owner(armReentryCheck(a)), guard(&a->lock) {}
        ~Held();
        Held(const Held &) = delete;
        Held &operator=(const Held &) = delete;

      private:
        static AirTime *armReentryCheck(AirTime *a);
        AirTime *owner; // declared first, so its initialiser runs before the lock is taken
        concurrency::LockGuard guard;
    };

    /// All state, all logic, no lock. Cannot take one, so cannot nest.
    struct Windows {
        bool firstTime = true;
        // Time::getUptimeSecs() as of the last syncNow(). The windows rotate by the gap since, so
        // they stay correct across a paused scheduler.
        uint32_t secSinceBoot = 0;

        // Modular rings: index is absolute phase, (uptime secs / period) % N, never age.
        uint32_t channelUtilization[CHANNEL_UTILIZATION_PERIODS] = {0}; // 6 x 10s

        // EMA over channelUtilization, folded in syncNow() once per crossed 10 s bucket so its
        // time constant follows elapsed time rather than how often a caller happens to ask.
        float channelUtilAvg = 0.0f;
        bool hasChannelUtilSample = false;
        uint32_t utilizationTX[MINUTES_IN_HOUR] = {0}; // 60 x 60s, our TX only

        // Hour crossings rotated but not yet traced. The core cannot log its own rotations: it
        // only ever runs under the lock, and DEBUG_PORT.log() blocks on a UART write. runOnce()
        // drains this and logs after releasing, so the trace costs the lock nothing.
        uint32_t rotationsPendingLog = 0;

        // Shift-ordered, unlike the rings above: slot 0 is the newest hour and the index is age.
        struct airtimeStruct {
            uint32_t periodTX[PERIODS_TO_LOG] = {0};     // AirTime transmitted
            uint32_t periodRX[PERIODS_TO_LOG] = {0};     // AirTime received and repeated (valid mesh packets)
            uint32_t periodRX_ALL[PERIODS_TO_LOG] = {0}; // AirTime received regardless of validity. May be noise.
        } airtimes;

        // MT-SW/fw+: 10-minute activity window. Shift-ordered like airtimes.period* above, but
        // opposite append end (oldest at [0], newest pushed in at the tail) to match the original
        // fw+ AirTime::updateActivityWindow() shift direction - see the .cpp.
        ActivityTime activityWindow[ACTIVITY_WINDOW_COUNT] = {};
        // Accumulate ms since the current (incomplete) 10-minute window started; folded into
        // activityWindow and zeroed when syncNow() detects the window closed.
        uint32_t txAccum10 = 0, rxAccum10 = 0, rxBadAccum10 = 0;

        // MT-SW/fw+: average ms of airtime per RX_LOG-or-RX_ALL_LOG packet, one entry per completed
        // 10-minute window - same rollover as activityWindow, independent history length (see
        // RX_WINDOW_COUNT's comment for why the two need separate bulk-clear thresholds).
        uint32_t rxWindowAverages[RX_WINDOW_COUNT] = {0};
        // Accumulate ms/count since the current (incomplete) 10-minute window started; folded into
        // rxWindowAverages (as sum/count) and zeroed alongside txAccum10 et al.
        uint32_t rxWindowSum = 0, rxWindowCount = 0;

        // MT-SW/fw+: count of RX+TX mesh packets per completed 10-minute window (see
        // RXTXALL_ACTIVITY_COUNT's comment) - same rollover cadence again, own accumulator.
        uint32_t packetCountWindow[RXTXALL_ACTIVITY_COUNT] = {0};
        uint32_t packetCount10 = 0;

        void logAirtime(reportTypes reportType, uint32_t airtime_ms, const Held &);
        void logPacketSeen(const Held &);
        float channelUtilizationPercent(const Held &);
        /// The bucket sum alone. syncNow() folds the EMA and cannot reach it through
        /// channelUtilizationPercent(), which would re-enter syncNow().
        float channelUtilizationPercentRaw(const Held &);
        float smoothedChannelUtilizationPercent(const Held &);
        /// Fold `steps` readings of `sample` into channelUtilAvg. Closed form, not a loop, so a
        /// multi-day sleep decays by the time elapsed at the cost of one powf.
        void foldChannelUtil(float sample, uint32_t steps, const Held &);
        float utilizationTXPercent(const Held &);
        bool airtimeReport(reportTypes reportType, uint32_t *out, size_t count, const Held &);
        bool activityWindowReport(ActivityTime *out, size_t count, const Held &);
        bool rxWindowAveragesReport(uint32_t *out, size_t count, const Held &);
        bool packetCountWindowReport(uint32_t *out, size_t count, const Held &);
        uint8_t getSilentMinutes(float txPercent, float dutyCycle, const Held &);
        uint8_t getPeriodUtilMinute(const Held &);
        uint8_t getPeriodUtilHour(const Held &);
        // Advance rolling airtime windows from monotonic uptime, not from runOnce() calls.
        void syncNow(const Held &);
    } w;

    uint8_t max_channel_util_percent = 40;
    uint8_t polite_channel_util_percent = 25;
    uint8_t polite_duty_cycle_percent = 50; // half of Duty Cycle allowance is ok for metadata

  protected:
    virtual int32_t runOnce() override;
};

extern AirTime *airTime;
