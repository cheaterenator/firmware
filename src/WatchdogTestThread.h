#pragma once
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "mesh/Throttle.h"

// ---------------------------------------------------------------------------
// Controlled firmware hang, used to prove that the hardware watchdog really
// resets the MCU (and to measure how long that takes).
//
// nRF52840: WDT0 is configured in nrf52Setup(), enabled on the first pass of
// nrf52Loop() and fed from that same function - i.e. exactly once per main
// loop() iteration. Reload value is APP_WATCHDOG_SECS (90 s) and the behaviour
// is NRF_WDT_BEHAVIOUR_PAUSE_SLEEP_HALT: the counter freezes while the CPU
// sleeps and while a debugger holds the core halted. So anything that blocks
// loop() starves the feed, and the reset lands ~APP_WATCHDOG_SECS after the
// last feed *provided the CPU stays awake* - which is why the default hang is
// a busy spin, not a delay(). RP2040/RP2350 arm an 8 s watchdog from
// rp2040Loop() the same way.
//
// Build flags:
//   -D WATCHDOG_TEST_ENABLED=1              compile the thread in (default 0)
//   -D WATCHDOG_TEST_AFTER_SECS=120         auto-hang N s after boot (0 = manual only)
//   -D WATCHDOG_TEST_MODE=0|1|2             which flavour of hang (see enum below)
// Manual trigger from anywhere (button handler, module, admin path):
//   watchdogTestRequested = true;
// ---------------------------------------------------------------------------

#ifndef WATCHDOG_TEST_ENABLED
#define WATCHDOG_TEST_ENABLED 0
#endif

// Seconds of uptime after which the firmware hangs itself. 0 = only on request.
#ifndef WATCHDOG_TEST_AFTER_SECS
#define WATCHDOG_TEST_AFTER_SECS 0
#endif

enum WatchdogTestMode {
    // Tight spin in thread context, interrupts still serviced. Models the usual
    // "something livelocks inside loop()" failure - BLE/USB keep running, only
    // the watchdog feed stops. The watchdog must reset us.
    WATCHDOG_TEST_MODE_BUSY = 0,

    // Same spin, but with interrupts masked: no RTOS tick, no ISRs, no
    // SoftDevice. Nothing in software can recover from this, so a reset here is
    // proof the watchdog is an independent peripheral (WDT runs off the 32.768
    // kHz LFCLK and is unaffected by CPU priority or PRIMASK).
    WATCHDOG_TEST_MODE_NOIRQ = 1,

    // Negative control: idle inside delay() so the CPU actually sleeps. With
    // PAUSE_SLEEP_HALT the nRF52 counter is frozen for most of that time, so the
    // reset is heavily delayed or never arrives. Documents the one hang class
    // this watchdog configuration does *not* cover (e.g. a deadlock where every
    // task is blocked on a semaphore).
    WATCHDOG_TEST_MODE_SLEEPY = 2,
};

#ifndef WATCHDOG_TEST_MODE
#define WATCHDOG_TEST_MODE WATCHDOG_TEST_MODE_BUSY
#endif

// How often the thread checks for the trigger. Keep it short so a manual
// request reacts promptly; the cost is one no-op wakeup per interval.
#define WATCHDOG_TEST_CHECK_INTERVAL_MS 500

// Set to true from anywhere to hang on the next WatchdogTestThread tick.
inline volatile bool watchdogTestRequested = false;

/**
 * Hang the firmware on purpose. Never returns - only the watchdog, a pin reset
 * or a power cycle gets the board out of here.
 */
inline void watchdogTestHang(uint8_t mode)
{
    LOG_ERROR("WatchdogTest: hanging on purpose (mode %u) - the watchdog must reset us now", mode);
    delay(250); // let that line drain out of the USB CDC / UART FIFO before we stop servicing it

    if (mode == WATCHDOG_TEST_MODE_SLEEPY) {
        while (true)
            delay(1000); // CPU sleeps between ticks, so a PAUSE_SLEEP watchdog barely counts
    }

    if (mode == WATCHDOG_TEST_MODE_NOIRQ) {
#if defined(__arm__) && !defined(ARCH_PORTDUINO)
        __disable_irq(); // from here on nothing in software runs, not even the RTOS tick
#else
        LOG_WARN("WatchdogTest: NOIRQ mode unsupported on this arch, spinning with interrupts on");
#endif
    }

    volatile uint32_t spins = 0;
    while (true)
        spins++; // volatile, so the compiler has to keep the loop around
}

#if WATCHDOG_TEST_ENABLED

/**
 * Arms the controlled hang: either after a fixed uptime, or when something sets
 * watchdogTestRequested. Runs as an ordinary OSThread, so the hang happens
 * inside loop() - the same context that feeds the watchdog.
 */
class WatchdogTestThread : public concurrency::OSThread
{
  public:
    WatchdogTestThread() : OSThread("WatchdogTest")
    {
        bootMs = millis();
#if WATCHDOG_TEST_AFTER_SECS > 0
        LOG_WARN("WatchdogTest armed: hanging %lu s after boot, mode %u", (unsigned long)WATCHDOG_TEST_AFTER_SECS,
                 (unsigned)WATCHDOG_TEST_MODE);
#else
        LOG_WARN("WatchdogTest armed: waiting for a manual trigger, mode %u", (unsigned)WATCHDOG_TEST_MODE);
#endif
    }

  protected:
    int32_t runOnce() override
    {
        if (watchdogTestRequested)
            watchdogTestHang(WATCHDOG_TEST_MODE);

#if WATCHDOG_TEST_AFTER_SECS > 0
        if (!Throttle::isWithinTimespanMs(bootMs, WATCHDOG_TEST_AFTER_SECS * 1000UL))
            watchdogTestHang(WATCHDOG_TEST_MODE);
#endif
        return WATCHDOG_TEST_CHECK_INTERVAL_MS;
    }

  private:
    uint32_t bootMs = 0;
};

#endif // WATCHDOG_TEST_ENABLED
