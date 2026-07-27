// corner_cap.h - soft cap on right-stick vector magnitude (diagonal overshoot).
//
// This is the old stick_radial.h "Stage 2", now standalone. The old "Stage 1"
// (per-axis gain + hard unit-circle correction, the "radial gain" stage) and
// its runtime RightStickCal calibrator are intentionally REMOVED - the raw
// diagonal overshoot is kept and only its PEAK is bounded here.
//
// cap_pct: 0 = off, 100 = hard unit circle, 101..255 = soft cap at that percent
// of full scale. Trims only the vector tip beyond the cap; anything shorter
// passes untouched, so it preserves the natural mid-angle gate shape while
// pulling the sharpest corner reach in (e.g. +31.8% -> +20% at cap_pct=120).
#ifndef CORNER_CAP_H
#define CORNER_CAP_H
#include <stdint.h>
#include <math.h>

static inline void apply_corner_cap_s16(int16_t* x, int16_t* y, uint8_t cap_pct) {
    if (cap_pct == 0) return;                           // gate: no cap
    float fx = (float)*x, fy = (float)*y;
    float cap = 32767.0f * ((float)cap_pct / 100.0f);
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag > cap) {
        float s = cap / mag;
        fx *= s; fy *= s;
    }
    int32_t ox = (int32_t)lrintf(fx);
    int32_t oy = (int32_t)lrintf(fy);
    if (ox < -32768) ox = -32768; else if (ox > 32767) ox = 32767;
    if (oy < -32768) oy = -32768; else if (oy > 32767) oy = 32767;
    *x = (int16_t)ox; *y = (int16_t)oy;
}

#endif // CORNER_CAP_H