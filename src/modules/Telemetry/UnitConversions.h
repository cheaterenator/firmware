#pragma once

#include <cmath>
#include <cstdint>

class UnitConversions
{
  public:
    static float CelsiusToFahrenheit(float celsius);
    static float MetersPerSecondToKnots(float metersPerSecond);
    static float HectoPascalToInchesOfMercury(float hectoPascal);

    // Reduce a station (absolute) barometric reading to the equivalent pressure at mean sea
    // level, so readings from sensors at different altitudes become comparable. Uses the
    // temperature-corrected barometric formula; pass 15.0f (ISA standard) when the actual
    // temperature at the sensor isn't known. Returns pressureHpa unchanged if the inputs would
    // put the formula outside its valid domain (e.g. a nonsensical altitude/temperature pairing).
    static float PressureAtSeaLevel(float pressureHpa, float temperatureC, int32_t altitudeMeters);

    // Bound a float before Arduino String(float) renders it: its fixed char[33] + dtostrf overflow
    // near FLT_MAX (stack smash). Clamp to +/-1e9 (<=10 digits) and drop non-finite values.
    static inline float displaySafeFloat(float v)
    {
        if (!std::isfinite(v))
            return 0.0f;
        return v < -1e9f ? -1e9f : (v > 1e9f ? 1e9f : v);
    }
};
