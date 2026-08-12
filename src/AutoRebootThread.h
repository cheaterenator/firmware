#pragma once
#include "concurrency/OSThread.h"
#include "configuration.h"
#include "main.h"
#include "mesh/Throttle.h"

// How often the interval check runs. Independent of AUTO_REBOOT_INTERVAL_SECS.
#define AUTO_REBOOT_CHECK_INTERVAL_MS (60UL * 1000)

// uint32_t millis() wraps after ~49.7 days, which is the hard ceiling Throttle's
// unsigned-subtraction math can measure - AUTO_REBOOT_INTERVAL_SECS must stay under it.
#define AUTO_REBOOT_MAX_INTERVAL_SECS (UINT32_MAX / 1000UL)
#if AUTO_REBOOT_INTERVAL_SECS > 0 && AUTO_REBOOT_INTERVAL_SECS > AUTO_REBOOT_MAX_INTERVAL_SECS
#error "AUTO_REBOOT_INTERVAL_SECS exceeds the ~49.7 day maximum (uint32_t millis() rollover)"
#endif

// Reboots the device on a fixed uptime interval (0 = disabled), via the same
// rebootAtMsec hook used by admin/menu-triggered reboots.
class AutoRebootThread : public concurrency::OSThread
{
  public:
    AutoRebootThread() : OSThread("AutoReboot")
    {
        bootMs = millis();
#if AUTO_REBOOT_INTERVAL_SECS == 0
        disable();
#else
        LOG_INFO("AutoReboot armed: rebooting every %lu seconds of uptime", (unsigned long)AUTO_REBOOT_INTERVAL_SECS);
#endif
    }

  protected:
    int32_t runOnce() override
    {
#if AUTO_REBOOT_INTERVAL_SECS == 0
        return disable();
#else
        if (!Throttle::isWithinTimespanMs(bootMs, AUTO_REBOOT_INTERVAL_SECS * 1000UL)) {
            LOG_INFO("AutoReboot: %lu second interval elapsed, rebooting", (unsigned long)AUTO_REBOOT_INTERVAL_SECS);
            rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
            return disable();
        }
        return AUTO_REBOOT_CHECK_INTERVAL_MS;
#endif
    }

  private:
    uint32_t bootMs = 0;
};
