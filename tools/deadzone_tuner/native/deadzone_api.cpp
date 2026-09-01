// deadzone_api.cpp - flat C ABI over deadzone.hpp, exported from a native
// DLL so the WPF tuner (C#) can P/Invoke the exact same math the firmware
// runs, instead of re-implementing it a third time in C#.
#include "deadzone.hpp"

extern "C" {

struct AxisConfigNative {
    int32_t deadzone;
    int32_t antideadzone;
    int32_t maxzone;
};

// type: 0 = Radial, 1 = Axial
__declspec(dllexport) void deadzone_apply_stick_native(
    int16_t *px, int16_t *py,
    int type,
    const AxisConfigNative *radial,
    const AxisConfigNative *x_axis,
    const AxisConfigNative *y_axis)
{
    deadzone::Stick stick;
    stick.type   = (type == 0) ? deadzone::Type::Radial : deadzone::Type::Axial;
    stick.radial = {radial->deadzone, radial->antideadzone, radial->maxzone};
    stick.x_axis = {x_axis->deadzone, x_axis->antideadzone, x_axis->maxzone};
    stick.y_axis = {y_axis->deadzone, y_axis->antideadzone, y_axis->maxzone};

    int16_t x = *px, y = *py;
    stick.apply(x, y);
    *px = x;
    *py = y;
}

} // extern "C"
