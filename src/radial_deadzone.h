// radial_deadzone.h - circular center deadzone with scaled-survivor rescale.
//
// Replaces the old axial_deadzone.h. Judged on the VECTOR magnitude, not each
// axis independently, so the threshold is a CIRCLE (not a plus/square) and it
// introduces no per-axis angle bias. Survivors are rescaled to full range, so
// there is no output jump at the deadzone edge.
//
// dz_units is a radius in the int16 stick scale (0..32768). To convert from a
// 0..255 config byte: dz_units = value * 256.
//
// Note vs. the removed axial deadzone: a radial deadzone deliberately does NOT
// try to clip the R2P right-stick Y "bulge" (small RY on large RX) - that is
// now the job of angular_restrict.h, which snaps near-horizontal angles to the
// pure cardinal and removes the bulge without inflating the deadzone.
#ifndef RADIAL_DEADZONE_H
#define RADIAL_DEADZONE_H
#include <stdint.h>
#include <math.h>

static inline void radial_deadzone_s16(int16_t* x, int16_t* y, uint16_t dz_units) {
    if (dz_units == 0) return;                          // gate: no-op when disabled
    float fx = (float)*x, fy = (float)*y;
    float r  = sqrtf(fx * fx + fy * fy);
    if (r <= (float)dz_units) { *x = 0; *y = 0; return; }  // inside circle -> center
    float span = 32767.0f - (float)dz_units;            // remaining radial travel
    float r2   = (r - (float)dz_units) / span * 32767.0f;  // rescale survivor to full
    float s    = r2 / r;                                 // preserve direction
    int32_t ox = (int32_t)lrintf(fx * s);
    int32_t oy = (int32_t)lrintf(fy * s);
    if (ox < -32768) ox = -32768; else if (ox > 32767) ox = 32767;
    if (oy < -32768) oy = -32768; else if (oy > 32767) oy = 32767;
    *x = (int16_t)ox; *y = (int16_t)oy;
}

#endif // RADIAL_DEADZONE_H