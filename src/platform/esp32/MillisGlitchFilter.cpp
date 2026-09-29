// Every millis() call in the image lands here: variants/esp32/esp32-common.ini links with -Wl,--wrap=millis.
//
// Field log 2026-09-29 (heltec-v2_1, Wi-Fi busy with MQTT): one millis() read landed exactly 2^36 us (68719476 ms) ahead
// of the reads around it. A caller that stores such a read is ~19 h off from then on - OSThread parks its thread until
// then, Throttle sees a timeout that never happened, UptimeClock held its clock. A real forward step survives a second
// read; a glitch does not.
#include "MillisGlitchFilter.h"

#if MILLIS_GLITCH_FILTER
#include <Arduino.h>

extern "C" unsigned long __real_millis(void);

namespace
{
// A step this large since the previous answer gets a second read before it is believed. millis() is called many times
// a second, so the extra read is rare. 32-bit loads and stores are atomic on every ESP32 core; the fields are shared
// by both cores and ISRs without a lock because a stale lastMillis only costs a redundant re-read.
constexpr uint32_t kConfirmStepMs = 1000;
volatile uint32_t lastMillis;
volatile uint32_t glitchCount;
volatile uint32_t lastGlitchAheadMs;
} // namespace

extern "C" unsigned long ARDUINO_ISR_ATTR __wrap_millis(void)
{
    uint32_t now = __real_millis();
    const uint32_t last = lastMillis;
    if ((uint32_t)(now - last) > kConfirmStepMs) {
        // Keep whichever read stepped less from the previous answer: the glitch is the one far ahead.
        const uint32_t again = __real_millis();
        if ((uint32_t)(again - last) < (uint32_t)(now - last)) {
            const uint32_t ahead = now - again;
            if (ahead > kConfirmStepMs && ahead < 0x80000000u) {
                lastGlitchAheadMs = ahead;
                glitchCount = glitchCount + 1;
            }
            now = again;
        }
    }
    lastMillis = now;
    return now;
}

MillisGlitchStats getMillisGlitchStats()
{
    return {glitchCount, lastGlitchAheadMs};
}
#endif
