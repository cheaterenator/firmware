#pragma once

#include <cstdint>

#if MILLIS_GLITCH_FILTER
/// millis() reads the -Wl,--wrap=millis filter replaced since boot, and how far ahead of its re-read the last one was.
struct MillisGlitchStats {
    uint32_t count;
    uint32_t lastAheadMs;
};

MillisGlitchStats getMillisGlitchStats();
#endif
