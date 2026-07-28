// dither.h - deterministic tangential dither: a small alternating ROTATION of
// the stick vector, injected to keep Halo Infinite's rotational aim assist
// engaged.
//
// LAST stage in the pipeline. What the game receives is exactly what this
// emits - no stage may run after it, because every stage below would partly
// undo it (angular_restrict.h in particular would snap the oscillation
// straight back onto the cardinal it was perturbing away from).
//
// ---------------------------------------------------------------------------
// DITHER, NOT JITTER
// ---------------------------------------------------------------------------
// Jitter would be random noise. That is the wrong tool here: random amplitude
// means the perturbation is different every frame, so the effect can't be
// tuned, can't be reproduced between test runs, and shows up as an erratic
// reticle rather than a controlled one. This is a deterministic square wave -
// it alternates between exactly +amp and -amp on a fixed period. Same input,
// same output, every time, which is the only way an A/B against it means
// anything.
//
// ---------------------------------------------------------------------------
// WHY TANGENTIAL (ROTATION) AND NOT ADDITIVE
// ---------------------------------------------------------------------------
// The obvious implementation - add +/-n counts to X and Y - modulates the
// vector MAGNITUDE. That is the one thing this must not do, because Halo's
// rotational aim assist is keyed on magnitude: an additive dither would make
// assist strength oscillate along with it, which is the opposite of
// "sustaining" it.
//
// Rotating the vector instead perturbs only the ANGLE. Magnitude is preserved
// exactly (a rotation is an isometry), so:
//   - assist strength stays constant while the input keeps changing,
//   - on the movement stick, walking SPEED is untouched - only the direction
//     wobbles by a fraction of a degree,
//   - a zero vector rotates to a zero vector, so this can never create input
//     out of nothing. No drift at rest, no creep, no interaction with any
//     centre deadzone. That property is why the gate below can be this simple.
//
// ---------------------------------------------------------------------------
// RATE
// ---------------------------------------------------------------------------
// The game samples the pad once per frame, so an oscillation faster than the
// frame rate is invisible to it (or worse, aliases into something arbitrary).
// The period below is deliberately well under a typical frame rate: toggling
// every 16 ms gives a ~31 Hz square wave, which is sampled several times per
// half-cycle at 120 fps and up. Report rate does not matter - the phase comes
// from a millisecond clock, not from a report counter, so it is independent of
// how fast the pad happens to be polling.
//
// If nothing else in the tuning works, this constant is the next thing to
// move; amplitude alone is a one-dimensional search through a two-dimensional
// space.
#ifndef DITHER_H
#define DITHER_H
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifndef DITHER_PI
#define DITHER_PI 3.14159265358979323846f
#endif

// Half-period of the square wave, in milliseconds. One full cycle is twice
// this (16 ms -> ~31 Hz).
#define DITHER_HALF_PERIOD_MS 16u

// amp_deg10: amplitude in TENTHS of a degree (10 = 1.0 deg). 0 = off.
//   Scale check against an 8-bit (256-step) pad, where one step is 256 int16
//   counts: at full deflection a 1.0 deg rotation moves the tip by
//   32767*sin(1 deg) = 572 counts, i.e. a bit over two steps - comfortably
//   visible. At half deflection it is ~1.1 steps. Below about a quarter
//   deflection a 1 deg dither falls under one quantisation step and stops
//   registering at all, which is fine: that is not where aim assist matters.
//
// phase: alternates the sign. Caller supplies it so both sticks share one
//   clock and the header stays free of timer dependencies - see hid_app.c.
static inline void dither_s16(int16_t* x, int16_t* y, uint8_t amp_deg10, bool phase)
{
    if (amp_deg10 == 0) return;                      // gate: off
    if (*x == 0 && *y == 0) return;                  // nothing to rotate

    const float a = (phase ? 1.0f : -1.0f)
                  * ((float)amp_deg10 * 0.1f) * (DITHER_PI / 180.0f);
    const float c = cosf(a), s = sinf(a);

    const float fx = (float)*x, fy = (float)*y;
    const float rx = fx * c - fy * s;
    const float ry = fx * s + fy * c;

    int32_t ox = (int32_t)lrintf(rx);
    int32_t oy = (int32_t)lrintf(ry);
    if (ox < -32768) ox = -32768; else if (ox > 32767) ox = 32767;
    if (oy < -32768) oy = -32768; else if (oy > 32767) oy = 32767;
    *x = (int16_t)ox; *y = (int16_t)oy;
}

#endif // DITHER_H