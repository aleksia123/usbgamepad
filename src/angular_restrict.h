// angular_restrict.h - angular (polar) restriction toward the cardinal axes.
//
// This is the "angular restriction some players like about axial deadzones",
// implemented directly and cleanly instead of as a side effect of zeroing an
// axis. Near a cardinal the stick angle is pulled to the pure axis; the vector
// MAGNITUDE is preserved exactly, so:
//   - thresholds stay CIRCULAR (no squaring like an axial deadzone),
//   - the center deadzone is NOT inflated (magnitude is untouched),
//   - it is symmetric in X and Y (no horizontal angle bias - the acceleration/
//     weighting skew called out in the Halo axial-deadzone diagram).
//
// It also subsumes the R2P right-stick "bulge" fix: a small snap on the look
// stick collapses near-horizontal angles to pure horizontal, removing the
// spurious RY that the old axial deadzone was there to clip.
//
// snap_deg: half-width (degrees, 0..45) of the pull zone around each cardinal.
//   0             -> off (passthrough)
//   |off| <= snap -> angle collapses exactly to the cardinal
//   snap..45      -> angle rescaled continuously so a true 45 deg diagonal is
//                    still reachable at the sector edge (no jump, no lost range)
#ifndef ANGULAR_RESTRICT_H
#define ANGULAR_RESTRICT_H
#include <stdint.h>
#include <math.h>

#ifndef ANG_PI
#define ANG_PI 3.14159265358979323846f
#endif

static inline void angular_restrict_s16(int16_t* x, int16_t* y, uint8_t snap_deg) {
    if (snap_deg == 0) return;                          // gate: passthrough
    float fx = (float)*x, fy = (float)*y;
    float r  = sqrtf(fx * fx + fy * fy);
    if (r < 1.0f) return;                               // center: no angle to pull

    float th   = atan2f(fy, fx);                        // -pi..pi
    const float Q = ANG_PI / 2.0f;                      // 90 deg (cardinal spacing)
    float k    = floorf(th / Q + 0.5f);                 // nearest cardinal index
    float thc  = k * Q;                                 // nearest cardinal angle
    float d    = th - thc;                              // offset in -pi/4..pi/4

    float snap = (float)snap_deg * (ANG_PI / 180.0f);   // snap half-width (rad)
    float quad = ANG_PI / 4.0f;                         // 45 deg sector half-width
    float ad   = fabsf(d);
    float d2;
    if (ad <= snap) {
        d2 = 0.0f;                                      // collapse to cardinal
    } else {
        d2 = (ad - snap) / (quad - snap) * quad;        // rescale survivor to full sector
    }
    float th2 = thc + (d < 0.0f ? -d2 : d2);

    int32_t ox = (int32_t)lrintf(r * cosf(th2));
    int32_t oy = (int32_t)lrintf(r * sinf(th2));
    if (ox < -32768) ox = -32768; else if (ox > 32767) ox = 32767;
    if (oy < -32768) oy = -32768; else if (oy > 32767) oy = 32767;
    *x = (int16_t)ox; *y = (int16_t)oy;
}

#endif // ANGULAR_RESTRICT_H