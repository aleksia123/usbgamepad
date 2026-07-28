// square_to_circle.h - undo a square (rectangular) gate remap, with a blend.
//
// FIRST stage in the pipeline. Everything downstream - the radial deadzone,
// the angular restriction, the corner cap - assumes a circular gate, so a
// square-remapped input has to be corrected before any of them run, not after.
//
// ---------------------------------------------------------------------------
// WHAT IT UNDOES
// ---------------------------------------------------------------------------
// A "rectangular"/square boundary algorithm maps the stick's reachable region
// from a circle out to a square. The physical gate is round, so filling the
// square's corners means scaling each direction by 1/max(|cos t|,|sin t|):
// 1.0 at the cardinals, up to sqrt(2) at 45 degrees. Cardinals stay put and
// the diagonals get pushed outward, so the output boundary becomes a rounded
// square and magnitude at 45 degrees exceeds magnitude at 0. That outward push
// between the cardinals is the "bulge", and it is why a circularity test reads
// worse with the algorithm on - the boundary being measured is no longer a
// circle.
//
// The inverse is exact: scale by max(|x|,|y|)/r. 1.0 at the cardinals,
// 1/sqrt(2) at 45 degrees. No information is lost in either direction, because
// the corner (32767, 32767) is representable in int16 - the magnitude 46341 is
// only ever derived, never stored, so nothing clips on the way through.
//
// ---------------------------------------------------------------------------
// WHY IT SHIPS DISABLED
// ---------------------------------------------------------------------------
// If the pad can turn its own square remap off, that is strictly better than
// correcting it here: one fewer rounding step, and no stage to keep in sync
// with a setting that lives somewhere else. This exists for the case where the
// remap can't be disabled - or where you want a PARTIAL correction, which no
// pad-side toggle offers.
//
// ---------------------------------------------------------------------------
// pct - the blend, and the reason this isn't just a boolean
// ---------------------------------------------------------------------------
//   0   -> passthrough (leave the square gate alone)
//   100 -> full circularisation
//   1..99 -> keep that fraction of the diagonal overshoot
//
// The in-between values are the point. A pad-side toggle is binary: full
// square or full circle. Blending gives a dial, which matters when the two
// ends are both defensible - e.g. on the movement stick, uniform magnitude in
// every direction argues for 100 while extra diagonal strafe reach argues for
// 0, and the better answer is probably neither extreme.
#ifndef SQUARE_TO_CIRCLE_H
#define SQUARE_TO_CIRCLE_H
#include <stdint.h>
#include <math.h>

static inline void square_to_circle_s16(int16_t* x, int16_t* y, uint8_t pct)
{
    if (pct == 0) return;                          // gate: passthrough

    const float fx = (float)*x, fy = (float)*y;
    const float r  = sqrtf(fx * fx + fy * fy);
    if (r < 1.0f) return;                          // centre: nothing to scale

    const float ax = fabsf(fx), ay = fabsf(fy);
    const float full = (ax > ay ? ax : ay) / r;    // exact inverse factor
    const float t    = (float)pct * 0.01f;
    const float s    = 1.0f + t * (full - 1.0f);   // lerp(1, full, t)

    int32_t ox = (int32_t)lrintf(fx * s);
    int32_t oy = (int32_t)lrintf(fy * s);
    if (ox < -32768) ox = -32768; else if (ox > 32767) ox = 32767;
    if (oy < -32768) oy = -32768; else if (oy > 32767) oy = 32767;
    *x = (int16_t)ox; *y = (int16_t)oy;
}

#endif // SQUARE_TO_CIRCLE_H