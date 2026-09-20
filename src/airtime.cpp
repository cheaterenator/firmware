#include "airtime.h"
#include "NodeDB.h"
#include "UptimeClock.h"
#include "configuration.h"
#include <algorithm>
#include <assert.h>
#include <cmath>
#include <string.h>

AirTime *airTime = NULL;

AirTime *AirTime::Held::armReentryCheck(AirTime *a)
{
#ifdef AIRTIME_REENTRY_CHECK
    // Before the lock: a nested take blocks forever, so a later check would never run.
    assert(!a->reentryFlag);
    a->reentryFlag = true;
#endif
    return a;
}

AirTime::Held::~Held()
{
#ifdef AIRTIME_REENTRY_CHECK
    owner->reentryFlag = false;
#else
    (void)owner;
#endif
}

// --- the lock-free core -------------------------------------------------------------------------
// Every method here requires the lock, and says so in its signature. None can take it: Windows has
// no lock to reach.

void AirTime::Windows::logAirtime(reportTypes reportType, uint32_t airtime_ms, const Held &held)
{
    // A packet may be logged immediately after waking from light sleep. Sync first so
    // the packet is counted in the current wall-time bucket, not a stale awake-time bucket.
    syncNow(held);

    // The caller logs, once the lock is released.
    if (reportType == TX_LOG) {
        this->airtimes.periodTX[0] = this->airtimes.periodTX[0] + airtime_ms;
        this->utilizationTX[this->getPeriodUtilHour(held)] += airtime_ms;
        this->txAccum10 += airtime_ms; // MT-SW/fw+
    } else if (reportType == RX_LOG) {
        this->airtimes.periodRX[0] = this->airtimes.periodRX[0] + airtime_ms;
        this->rxAccum10 += airtime_ms; // MT-SW/fw+
        this->rxWindowSum += airtime_ms; // MT-SW/fw+
        this->rxWindowCount++;           // MT-SW/fw+
    } else if (reportType == RX_ALL_LOG) {
        this->airtimes.periodRX_ALL[0] = this->airtimes.periodRX_ALL[0] + airtime_ms;
        this->rxBadAccum10 += airtime_ms; // MT-SW/fw+
        this->rxWindowSum += airtime_ms;  // MT-SW/fw+ (fw+ counts RX_LOG and RX_ALL_LOG together here)
        this->rxWindowCount++;            // MT-SW/fw+
    }

    // Log all airtime type for channel utilization
    this->channelUtilization[this->getPeriodUtilMinute(held)] += airtime_ms;
}

// MT-SW/fw+: NOT called from logAirtime() above - packet counting, not airtime duration. Router calls
// this directly (once per RX+TX mesh packet) via AirTime::logPacketSeen().
void AirTime::Windows::logPacketSeen(const Held &held)
{
    // Same reasoning as logAirtime(): a packet may be logged immediately after waking from light
    // sleep, so sync first to land in the current window, not a stale one.
    syncNow(held);
    this->packetCount10++;
}

uint8_t AirTime::Windows::getPeriodUtilMinute(const Held &)
{
    return (secSinceBoot / 10) % CHANNEL_UTILIZATION_PERIODS;
}

uint8_t AirTime::Windows::getPeriodUtilHour(const Held &)
{
    return (secSinceBoot / 60) % MINUTES_IN_HOUR;
}

void AirTime::Windows::syncNow(const Held &held)
{
    // Monotonic uptime, not RTC/network time: a user, GPS, or NTP clock change must not move
    // airtime accounting. Pure read; the main loop publishes the wrap carry it derives from.
    uint32_t nowSecs = Time::getUptimeSecs();

    if (firstTime) {
        memset(this->utilizationTX, 0, sizeof(this->utilizationTX));
        memset(this->channelUtilization, 0, sizeof(this->channelUtilization));
        memset(this->airtimes.periodTX, 0, sizeof(this->airtimes.periodTX));
        memset(this->airtimes.periodRX, 0, sizeof(this->airtimes.periodRX));
        memset(this->airtimes.periodRX_ALL, 0, sizeof(this->airtimes.periodRX_ALL));
        memset(this->activityWindow, 0, sizeof(this->activityWindow));     // MT-SW/fw+
        memset(this->rxWindowAverages, 0, sizeof(this->rxWindowAverages)); // MT-SW/fw+
        memset(this->packetCountWindow, 0, sizeof(this->packetCountWindow)); // MT-SW/fw+
        this->txAccum10 = this->rxAccum10 = this->rxBadAccum10 = 0;        // MT-SW/fw+
        this->rxWindowSum = this->rxWindowCount = 0;                       // MT-SW/fw+
        this->packetCount10 = 0;                                           // MT-SW/fw+

        this->secSinceBoot = nowSecs;
        firstTime = false;
        return;
    }

    if (nowSecs == this->secSinceBoot) {
        return;
    }

    uint32_t oldSecSinceBoot = this->secSinceBoot;
    this->secSinceBoot = nowSecs;

    // Historical airtime reports use 1-hour buckets. If multiple hours elapsed while
    // asleep, rotate each crossed bucket or clear the whole report window.
    uint32_t elapsedAirtimePeriods = (this->secSinceBoot / SECONDS_PER_PERIOD) - (oldSecSinceBoot / SECONDS_PER_PERIOD);
    if (elapsedAirtimePeriods >= PERIODS_TO_LOG) {
        memset(this->airtimes.periodTX, 0, sizeof(this->airtimes.periodTX));
        memset(this->airtimes.periodRX, 0, sizeof(this->airtimes.periodRX));
        memset(this->airtimes.periodRX_ALL, 0, sizeof(this->airtimes.periodRX_ALL));
    } else {
        // Hand the count to runOnce() rather than tracing each crossing here: this runs under
        // the lock, and a UART write would stall every other caller waiting on it.
        this->rotationsPendingLog += elapsedAirtimePeriods;
        for (uint32_t h = 0; h < elapsedAirtimePeriods; h++) {
            for (int i = PERIODS_TO_LOG - 2; i >= 0; --i) {
                this->airtimes.periodTX[i + 1] = this->airtimes.periodTX[i];
                this->airtimes.periodRX[i + 1] = this->airtimes.periodRX[i];
                this->airtimes.periodRX_ALL[i + 1] = this->airtimes.periodRX_ALL[i];
            }

            this->airtimes.periodTX[0] = 0;
            this->airtimes.periodRX[0] = 0;
            this->airtimes.periodRX_ALL[0] = 0;
        }
    }

    // Channel utilization is a rolling 60-second view split into six 10-second buckets.
    // Clear every bucket crossed while asleep so old airtime decays by real elapsed time.
    uint32_t elapsedUtilPeriods = (this->secSinceBoot / 10) - (oldSecSinceBoot / 10);
    // Fold one reading per crossed bucket, each before that bucket is cleared, so one delayed sync
    // lands where the same number of 10 s syncs would have. Bounded: six clears empty the window.
    const uint32_t steppedUtilPeriods = std::min<uint32_t>(elapsedUtilPeriods, CHANNEL_UTILIZATION_PERIODS);
    for (uint32_t i = 1; i <= steppedUtilPeriods; i++) {
        foldChannelUtil(channelUtilizationPercentRaw(held), 1, held);
        this->channelUtilization[((oldSecSinceBoot / 10) + i) % CHANNEL_UTILIZATION_PERIODS] = 0;
    }
    // Anything past a full window is elapsed time against an already-empty ring, so it folds as
    // idle in closed form rather than looping over a sleep that may have lasted days.
    foldChannelUtil(0.0f, elapsedUtilPeriods - steppedUtilPeriods, held);

    // TX utilization is a rolling 60-minute view used by duty-cycle checks.
    uint32_t elapsedUtilTXPeriods = (this->secSinceBoot / 60) - (oldSecSinceBoot / 60);
    if (elapsedUtilTXPeriods >= MINUTES_IN_HOUR) {
        memset(this->utilizationTX, 0, sizeof(this->utilizationTX));
    } else {
        for (uint32_t i = 1; i <= elapsedUtilTXPeriods; i++) {
            this->utilizationTX[((oldSecSinceBoot / 60) + i) % MINUTES_IN_HOUR] = 0;
        }
    }

    // MT-SW/fw+: 10-minute activity window (tx/rx/rx_bad/idle time) and RX-average-airtime window.
    // Ported from the historical fw+ AirTime, but rotated on this same monotonic-crossing discipline
    // as the blocks above instead of fw+'s original `secSinceBoot % RX_WINDOW_INTERVAL_SECONDS == 0`
    // OSThread-tick check - that trigger silently stalls across light sleep (a paused scheduler skips
    // ticks and never lands exactly on a multiple), the same bug class this class's rewrite fixed for
    // channelUtilization/utilizationTX above.
    //
    // activityWindow (10 slots) and rxWindowAverages (40 slots) roll over on the exact same boundary,
    // but must be bulk-cleared independently: sleeping through 15 windows clears the shorter
    // activityWindow (>= 10) while rxWindowAverages (>= 40) still needs its normal shift-loop.
    uint32_t elapsedActivityWindows =
        (this->secSinceBoot / RX_WINDOW_INTERVAL_SECONDS) - (oldSecSinceBoot / RX_WINDOW_INTERVAL_SECONDS);
    if (elapsedActivityWindows > 0) {
        bool clearActivity = elapsedActivityWindows >= ACTIVITY_WINDOW_COUNT;
        bool clearRxAvg = elapsedActivityWindows >= RX_WINDOW_COUNT;
        bool clearPacketCount = elapsedActivityWindows >= RXTXALL_ACTIVITY_COUNT;

        if (clearActivity)
            memset(this->activityWindow, 0, sizeof(this->activityWindow));
        if (clearRxAvg)
            memset(this->rxWindowAverages, 0, sizeof(this->rxWindowAverages));
        if (clearPacketCount)
            memset(this->packetCountWindow, 0, sizeof(this->packetCountWindow));

        if (!clearActivity || !clearRxAvg || !clearPacketCount) {
            for (uint32_t win = 0; win < elapsedActivityWindows; win++) {
                // Only the first crossing carries time/count actually accumulated since the window
                // opened; any further crossings folded into this same syncNow() call are windows we
                // were asleep for the entirety of - genuinely idle/no-packets, not double-counted from
                // one accumulator.
                uint32_t tx = (win == 0) ? this->txAccum10 : 0;
                uint32_t rx = (win == 0) ? this->rxAccum10 : 0;
                uint32_t rxBad = (win == 0) ? this->rxBadAccum10 : 0;
                uint32_t windowMs = RX_WINDOW_INTERVAL_SECONDS * 1000;
                uint32_t used = tx + rx + rxBad;
                uint32_t idle = (windowMs > used) ? (windowMs - used) : 0;
                uint32_t rxSum = (win == 0) ? this->rxWindowSum : 0;
                uint32_t rxCount = (win == 0) ? this->rxWindowCount : 0;
                uint32_t rxAverage = (rxCount > 0) ? (rxSum / rxCount) : 0;
                uint32_t packetCount = (win == 0) ? this->packetCount10 : 0;

                if (!clearActivity) {
                    for (int i = 0; i < ACTIVITY_WINDOW_COUNT - 1; i++)
                        this->activityWindow[i] = this->activityWindow[i + 1];
                    // Field-by-field, not `= {rx, tx, idle, rxBad};` - assigning a braced-init-list to
                    // an existing struct lvalue is the same class of toolchain landmine hit in
                    // OnDemandModule (see its comments): fine here in practice (no array/string members
                    // to trip on), but not worth re-litigating per-toolchain for a form this short.
                    ActivityTime &slot = this->activityWindow[ACTIVITY_WINDOW_COUNT - 1];
                    slot.rx_time = rx;
                    slot.tx_time = tx;
                    slot.idle_time = idle;
                    slot.rx_bad_time = rxBad;
                }
                if (!clearRxAvg) {
                    for (int i = 0; i < RX_WINDOW_COUNT - 1; i++)
                        this->rxWindowAverages[i] = this->rxWindowAverages[i + 1];
                    this->rxWindowAverages[RX_WINDOW_COUNT - 1] = rxAverage;
                }
                if (!clearPacketCount) {
                    for (int i = 0; i < RXTXALL_ACTIVITY_COUNT - 1; i++)
                        this->packetCountWindow[i] = this->packetCountWindow[i + 1];
                    this->packetCountWindow[RXTXALL_ACTIVITY_COUNT - 1] = packetCount;
                }
            }
        }
        this->txAccum10 = 0;
        this->rxAccum10 = 0;
        this->rxBadAccum10 = 0;
        this->rxWindowSum = 0;
        this->rxWindowCount = 0;
        this->packetCount10 = 0;
    }
}

bool AirTime::Windows::airtimeReport(reportTypes reportType, uint32_t *out, size_t count, const Held &held)
{
    if (!out || count > PERIODS_TO_LOG)
        return false;

    // Reports may be requested before runOnce() executes after wake.
    syncNow(held);

    const uint32_t *src = nullptr;
    if (reportType == TX_LOG) {
        src = this->airtimes.periodTX;
    } else if (reportType == RX_LOG) {
        src = this->airtimes.periodRX;
    } else if (reportType == RX_ALL_LOG) {
        src = this->airtimes.periodRX_ALL;
    }
    if (!src)
        return false;

    memcpy(out, src, count * sizeof(*out));
    return true;
}

bool AirTime::Windows::activityWindowReport(ActivityTime *out, size_t count, const Held &held)
{
    if (!out || count > ACTIVITY_WINDOW_COUNT)
        return false;

    // Reports may be requested before runOnce() executes after wake, same as airtimeReport().
    syncNow(held);

    memcpy(out, this->activityWindow, count * sizeof(*out));
    return true;
}

bool AirTime::Windows::rxWindowAveragesReport(uint32_t *out, size_t count, const Held &held)
{
    if (!out || count > RX_WINDOW_COUNT)
        return false;

    // Reports may be requested before runOnce() executes after wake, same as airtimeReport().
    syncNow(held);

    memcpy(out, this->rxWindowAverages, count * sizeof(*out));
    return true;
}

bool AirTime::Windows::packetCountWindowReport(uint32_t *out, size_t count, const Held &held)
{
    if (!out || count > RXTXALL_ACTIVITY_COUNT)
        return false;

    // Reports may be requested before runOnce() executes after wake, same as airtimeReport().
    syncNow(held);

    memcpy(out, this->packetCountWindow, count * sizeof(*out));
    return true;
}

float AirTime::Windows::channelUtilizationPercentRaw(const Held &)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < CHANNEL_UTILIZATION_PERIODS; i++) {
        sum += this->channelUtilization[i];
    }

    return (float(sum) / float(CHANNEL_UTILIZATION_PERIODS * 10 * 1000)) * 100;
}

float AirTime::Windows::channelUtilizationPercent(const Held &held)
{
    // Gate decisions should see buckets that have decayed across light-sleep time.
    syncNow(held);

    return channelUtilizationPercentRaw(held);
}

void AirTime::Windows::foldChannelUtil(float sample, uint32_t steps, const Held &)
{
    if (steps == 0)
        return;

    if (!hasChannelUtilSample) {
        // Seed from the first reading, or a node booting onto a busy channel reports it quiet
        // for a whole time constant.
        channelUtilAvg = sample;
        hasChannelUtilSample = true;
        steps--;
    }

    if (steps > 0) {
        // Integer power by squaring; powf would link ~1.9 KB of float libm for this one call.
        float base = 1.0f - 1.0f / float(CHANNEL_UTILIZATION_EMA_DIVISOR);
        float retained = 1.0f;
        for (uint32_t e = steps; e != 0; e >>= 1) {
            if (e & 1)
                retained *= base;
            base *= base;
        }
        channelUtilAvg = sample + (channelUtilAvg - sample) * retained;
    }
}

float AirTime::Windows::smoothedChannelUtilizationPercent(const Held &held)
{
    syncNow(held);

    // Nothing folded yet before the first bucket crossing, and 0 would read as an idle channel
    // rather than as no data.
    return hasChannelUtilSample ? channelUtilAvg : channelUtilizationPercentRaw(held);
}

float AirTime::Windows::utilizationTXPercent(const Held &held)
{
    // Duty-cycle checks use this value, so keep it current even outside the periodic thread.
    syncNow(held);

    uint32_t sum = 0;
    for (uint32_t i = 0; i < MINUTES_IN_HOUR; i++) {
        sum += this->utilizationTX[i];
    }

    return (float(sum) / float(MS_IN_HOUR)) * 100;
}

// Minutes we must be silent before sending again. Does not sync, and walks the ring as if the index
// were an age; both are wrong and both are pinned by characterisation tests. See airtime.h's TODO.
uint8_t AirTime::Windows::getSilentMinutes(float txPercent, float dutyCycle, const Held &)
{
    float newTxPercent = txPercent;
    for (int8_t i = MINUTES_IN_HOUR - 1; i >= 0; --i) {
        newTxPercent -= ((float)this->utilizationTX[i] / (MS_IN_MINUTE * MINUTES_IN_HOUR / 100));
        if (newTxPercent < dutyCycle)
            return MINUTES_IN_HOUR - 1 - i;
    }

    return MINUTES_IN_HOUR;
}

// --- the locking shell --------------------------------------------------------------------------
// Each takes the lock exactly once and delegates. Nothing below calls another method on `this`.

void AirTime::logAirtime(reportTypes reportType, uint32_t airtime_ms)
{
    {
        Held held(this);
        w.logAirtime(reportType, airtime_ms, held);
    }

    // Outside the lock: DEBUG_PORT.log() blocks on a UART write, and `lock` is a plain binary
    // semaphore with no priority inheritance, so holding it here would stall the radio thread.
    if (reportType == TX_LOG) {
        LOG_DEBUG("Packet TX: %ums", airtime_ms);
    } else if (reportType == RX_LOG) {
        LOG_DEBUG("Packet RX: %ums", airtime_ms);
    } else if (reportType == RX_ALL_LOG) {
        LOG_DEBUG("Packet RX (noise?) : %ums", airtime_ms);
    }
}

void AirTime::airtimeRotatePeriod()
{
    // Preserve the public helper while keeping all rotation logic in one monotonic-time path.
    Held held(this);
    w.syncNow(held);
}

bool AirTime::airtimeReport(reportTypes reportType, uint32_t *out, size_t count)
{
    Held held(this);
    return w.airtimeReport(reportType, out, count, held);
}

bool AirTime::activityWindowReport(ActivityTime *out, size_t count)
{
    Held held(this);
    return w.activityWindowReport(out, count, held);
}

bool AirTime::rxWindowAveragesReport(uint32_t *out, size_t count)
{
    Held held(this);
    return w.rxWindowAveragesReport(out, count, held);
}

void AirTime::logPacketSeen()
{
    Held held(this);
    w.logPacketSeen(held);
}

bool AirTime::packetCountWindowReport(uint32_t *out, size_t count)
{
    Held held(this);
    return w.packetCountWindowReport(out, count, held);
}

uint32_t AirTime::getSecondsSinceBoot()
{
    // Keep HTTP/debug reporting aligned with the same monotonic clock used by the buckets.
    Held held(this);
    w.syncNow(held);
    return w.secSinceBoot;
}

float AirTime::channelUtilizationPercent()
{
    Held held(this);
    return w.channelUtilizationPercent(held);
}

float AirTime::smoothedChannelUtilizationPercent()
{
    Held held(this);
    return w.smoothedChannelUtilizationPercent(held);
}

float AirTime::utilizationTXPercent()
{
    Held held(this);
    return w.utilizationTXPercent(held);
}

// These lock like everything else, because they call the core rather than the public accessors.
// Both read under the lock and warn after it, for the reason logAirtime() does.
bool AirTime::isTxAllowedChannelUtil(bool polite)
{
    uint8_t percentage = (polite ? polite_channel_util_percent : max_channel_util_percent);
    float utilization;
    {
        Held held(this);
        utilization = w.channelUtilizationPercent(held);
    }

    if (utilization < percentage)
        return true;
    LOG_WARN("Ch. util >%d%%. Skip send", percentage);
    return false;
}

bool AirTime::isTxAllowedAirUtil()
{
    float effectiveDutyCycle = getEffectiveDutyCycle();
    if (!config.lora.override_duty_cycle && effectiveDutyCycle < 100) {
        float limit = effectiveDutyCycle * polite_duty_cycle_percent / 100;
        float utilization;
        {
            Held held(this);
            utilization = w.utilizationTXPercent(held);
        }

        if (utilization < limit)
            return true;
        LOG_WARN("TX air util. >%f%%. Skip send", limit);
        return false;
    }
    return true;
}

uint8_t AirTime::getSilentMinutes(float txPercent, float dutyCycle)
{
    Held held(this);
    return w.getSilentMinutes(txPercent, dutyCycle, held);
}

AirTime::AirTime() : concurrency::OSThread("AirTime") {}

int32_t AirTime::runOnce()
{
    uint32_t rotations;
    {
        Held held(this);
        w.syncNow(held);
        rotations = w.rotationsPendingLog;
        w.rotationsPendingLog = 0;
    }

    // Outside the lock, for the reason logAirtime() gives. Any caller can cross an hour, but only
    // this thread reports it, so a crossing raised elsewhere is traced at most one tick late.
    if (rotations > 0) {
        LOG_DEBUG("Rotate airtimes, crossed %u hour(s)", rotations);
    }

    return (1000 * 1);
}
