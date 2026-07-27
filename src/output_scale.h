// output_scale.h - proportional scale of the output vector magnitude.
//
// This is the LAST stage in the pipeline. It decides what the game sees as
// "maximum stick", which is a different job from every other stage here: the
// deadzones and the corner cap reshape the gate, this one rescales the whole
// thing.
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS - Halo Infinite's Max Input Threshold
// ---------------------------------------------------------------------------
// Infinite engages look Acceleration once the stick reaches the in-game Max
// Input Threshold. Set MIT to 0 (= threshold at 100% of stick range) and then
// make full range unreachable, and Acceleration can never engage - you are
// aiming on the base Aim Response Curve alone. That is the trick third-party
// pro-controller apps expose as an outer anti-deadzone (GameSir "Anti-Deadzone
// Max 96" = -4%; the Razer clutch does the same thing on a paddle at 90%).
// pct = 96 reproduces it here, in the adapter, for a pad whose own app has no
// such control.
//
// ---------------------------------------------------------------------------
// RESCALE, NOT CLIP - the difference from corner_cap.h
// ---------------------------------------------------------------------------
// corner_cap.h CLIPS: everything past the cap collapses onto it, so the outer
// few percent of physical travel becomes dead - same output, no matter how
// much harder you push. Resolution at the edge is lost.
//
// This stage RESCALES: full physical travel is mapped proportionally into
// 0..pct% of output range. Nothing is dead, the resolution is spread across
// the whole sweep instead of piling up at the rail. Both keep you under MIT;
// they feel different, and which one you want is a feel question, so both are
// available and independently toggleable.
//
// ---------------------------------------------------------------------------
// pct > 100 IS NOT AN IN-GAME SENSITIVITY SLIDER
// ---------------------------------------------------------------------------
// Values above 100 give more output per millimetre of thumb travel, which
// sounds like the fix for "moving the stick from 0 to 70% barely does anything
// on screen". It is not the same operation as raising in-game sensitivity, and
// the difference matters:
//
//   in-game sensitivity scales the ARC's OUTPUT (degrees/second), leaving the
//     input domain intact;
//   this scales the ARC's INPUT, so full output is reached at 100/pct of
//     physical travel - i.e. you hit the Max Input Threshold EARLIER and
//     engage Acceleration sooner.
//
// So pct<100 and pct>100 pull in opposite directions and you cannot have both:
// under 100 isolates the base curve by keeping Acceleration out of reach, over
// 100 buys response at the cost of reaching Acceleration faster. Above 100 the
// tip of the range also saturates (see the radial clamp below), so some travel
// near the rail stops producing new values.
//
// pct = 100 is the neutral point, not the "off" point - unlike every other
// stage in this pipeline, whose no-op value is 0. The enable flag is the real
// gate.
#ifndef OUTPUT_SCALE_H
#define OUTPUT_SCALE_H
#include <stdint.h>
#include <math.h>

static inline void output_scale_s16(int16_t* x, int16_t* y, uint8_t pct)
{
    if (pct == 0 || pct == 100) return;            // gate: neutral / disabled

    const float s  = (float)pct * 0.01f;
    float       fx = (float)*x * s;
    float       fy = (float)*y * s;

    // Clamp RADIALLY, not per-axis. A per-axis clamp would let X pin at full
    // scale while Y kept growing, which bends the angle near the cardinals and
    // manufactures exactly the square-gate artifact the rest of this pipeline
    // is careful to avoid. Scaling both components by the same factor keeps
    // the direction exact. Only bites when pct > 100.
    const float mag = sqrtf(fx * fx + fy * fy);
    if (mag > 32767.0f) {
        const float k = 32767.0f / mag;
        fx *= k; fy *= k;
    }

    int32_t ox = (int32_t)lrintf(fx);
    int32_t oy = (int32_t)lrintf(fy);
    if (ox < -32768) ox = -32768; else if (ox > 32767) ox = 32767;
    if (oy < -32768) oy = -32768; else if (oy > 32767) oy = 32767;
    *x = (int16_t)ox; *y = (int16_t)oy;
}

#endif // OUTPUT_SCALE_H