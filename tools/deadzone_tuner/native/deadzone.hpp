// deadzone.hpp - C++ port of ../../../src/deadzone.h for the desktop tuner.
//
// This is a header-only, class-based re-expression of the exact same math
// as src/deadzone.h (which stays plain C for the firmware build). Kept
// separate rather than shared so the firmware's build/toolchain (pico-sdk,
// C99) is untouched; if the two ever drift, src/deadzone.h is the one the
// hardware actually runs.
#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace deadzone {

enum class Type {
    Radial,
    Axial,
};

// One axis' (or the radial) worth of deadzone shaping. See field comments
// in src/deadzone.h for units/ranges - unchanged here.
struct AxisConfig {
    int32_t deadzone     = 2600;
    int32_t antideadzone = 0;
    int32_t maxzone      = 100;

    static constexpr AxisConfig RadialDefault() { return {2600, 0, 100}; }
    static constexpr AxisConfig AxialDefault()  { return {2600, 20, 100}; }
};

inline double clampd(double lo, double v, double hi)
{
    return std::clamp(v, lo, hi);
}

// Independent per-axis deadzone/rescale. Same shape as deadzone_apply_axis().
inline int16_t apply_axis(int16_t value, const AxisConfig &cfg)
{
    if (cfg.deadzone <= 0 && cfg.antideadzone <= 0 && cfg.maxzone >= 100)
        return value;

    int32_t distVal = value >= 0 ? value : -value;
    if (cfg.deadzone > 0 && distVal <= cfg.deadzone)
        return 0;

    double maxAxisValue = value >= 0 ? 32767.0 : -32768.0;
    double ratio        = cfg.maxzone / 100.0;
    double maxZoneNeg   = ratio * -32768.0;
    double maxZonePos   = ratio *  32767.0;
    double maxZone      = value >= 0 ? maxZonePos : maxZoneNeg;

    double tempDead   = cfg.deadzone > 0 ? (cfg.deadzone / 32767.0) * maxAxisValue : 0.0;
    double currentVal = clampd(maxZoneNeg, (double)value, maxZonePos);
    double tempOutput = (currentVal - tempDead) / (maxZone - tempDead);

    if (tempOutput <= 0.0)
        return 0;

    double antiDeadPercent = cfg.antideadzone > 0 ? cfg.antideadzone * 0.01 : 0.0;
    double out = ((1.0 - antiDeadPercent) * tempOutput + antiDeadPercent) * maxAxisValue;
    return (int16_t)clampd(-32768.0, out, 32767.0);
}

// Circular deadzone/rescale: one radius test, angle-preserving rescale
// across both axes together. Same shape as deadzone_apply_radial(); see
// src/deadzone.h for the note on why the maxzone boundary is squarish
// rather than a perfect circle (intentional, matches upstream DS4Windows).
inline void apply_radial(int16_t &px, int16_t &py, const AxisConfig &cfg)
{
    int32_t x = px, y = py;

    if (cfg.deadzone <= 0 && cfg.antideadzone <= 0 && cfg.maxzone >= 100)
        return;

    int64_t distSq = (int64_t)x * x + (int64_t)y * y;
    int64_t deadSq = (int64_t)cfg.deadzone * cfg.deadzone;

    if (cfg.deadzone > 0 && distSq <= deadSq) {
        px = 0;
        py = 0;
        return;
    }

    double r    = std::atan2((double)y, (double)x);
    double cosR = std::fabs(std::cos(r));
    double sinR = std::fabs(std::sin(r));

    double maxXValue = x >= 0 ? 32767.0 : -32768.0;
    double maxYValue = y >= 0 ? 32767.0 : -32768.0;
    double ratio      = cfg.maxzone / 100.0;
    double maxZoneNeg = ratio * -32768.0;
    double maxZonePos = ratio *  32767.0;
    double maxZoneX   = x >= 0 ? maxZonePos : maxZoneNeg;
    double maxZoneY   = y >= 0 ? maxZonePos : maxZoneNeg;

    double tempDeadX = 0.0, tempDeadY = 0.0;
    if (cfg.deadzone > 0) {
        tempDeadX = cosR * (cfg.deadzone / 32767.0) * maxXValue;
        tempDeadY = sinR * (cfg.deadzone / 32767.0) * maxYValue;
    }

    double currentX = clampd(maxZoneNeg, (double)x, maxZonePos);
    double currentY = clampd(maxZoneNeg, (double)y, maxZonePos);
    double outputX  = (currentX - tempDeadX) / (maxZoneX - tempDeadX);
    double outputY  = (currentY - tempDeadY) / (maxZoneY - tempDeadY);

    double antiDeadX = cfg.antideadzone > 0 ? (cfg.antideadzone * 0.01) * cosR : 0.0;
    double antiDeadY = cfg.antideadzone > 0 ? (cfg.antideadzone * 0.01) * sinR : 0.0;

    double outX = outputX > 0.0 ? ((1.0 - antiDeadX) * outputX + antiDeadX) * maxXValue : 0.0;
    double outY = outputY > 0.0 ? ((1.0 - antiDeadY) * outputY + antiDeadY) * maxYValue : 0.0;

    px = (int16_t)clampd(-32768.0, outX, 32767.0);
    py = (int16_t)clampd(-32768.0, outY, 32767.0);
}

// One stick's full configuration (shape + per-mode params), matching
// StickDeadzoneConfig in src/deadzone.h.
class Stick {
public:
    Type type = Type::Radial;
    AxisConfig radial = AxisConfig::RadialDefault();
    AxisConfig x_axis = AxisConfig::AxialDefault();
    AxisConfig y_axis = AxisConfig::AxialDefault();

    // Apply the configured shape in place, mirroring deadzone_apply_stick().
    void apply(int16_t &x, int16_t &y) const
    {
        if (type == Type::Radial) {
            apply_radial(x, y, radial);
        } else {
            x = apply_axis(x, x_axis);
            y = apply_axis(y, y_axis);
        }
    }
};

} // namespace deadzone
