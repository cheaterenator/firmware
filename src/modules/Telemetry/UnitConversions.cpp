#include "UnitConversions.h"

float UnitConversions::CelsiusToFahrenheit(float celsius)
{
    return (celsius * 9) / 5 + 32;
}

float UnitConversions::MetersPerSecondToKnots(float metersPerSecond)
{
    return metersPerSecond * 1.94384;
}

float UnitConversions::HectoPascalToInchesOfMercury(float hectoPascal)
{
    return hectoPascal * 0.029529983071445;
}

float UnitConversions::PressureAtSeaLevel(float pressureHpa, float temperatureC, int32_t altitudeMeters)
{
    if (!std::isfinite(pressureHpa) || pressureHpa <= 0.0f || altitudeMeters == 0)
        return pressureHpa;

    const float h = static_cast<float>(altitudeMeters);
    // Temperature-corrected barometric formula (as used by weather stations to derive QNH from
    // QFE): denom is the mean absolute temperature between the station and sea level, using the
    // standard 6.5 K/km lapse rate.
    const float denom = temperatureC + 0.0065f * h + 273.15f;
    if (denom <= 0.0f)
        return pressureHpa;

    return pressureHpa * std::pow(1.0f - (0.0065f * h) / denom, -5.257f);
}
