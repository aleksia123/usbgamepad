// pad_config.h - stick processing profile, read by the Phase 3 pipeline in
// hid_app.c.
//
// g_pad_config is a mutable runtime global: the device boots into a CDC
// config session (see boot_mode.h / cdc_config.h) where PAD_CONFIG.SET
// commands write these fields directly, applied on the very next processed
// XInput report with no other change needed in hid_app.c. Edits are
// persisted to flash (pad_config_store.h) on a debounce, so they survive
// power cycles; PAD_CONFIG_DEFAULTS below is also the fallback used whenever
// the stored flash blob is missing or fails validation.
//
// Every stick stage below is an explicit enable flag + a value: the value is
// kept even while disabled, so toggling a stage off and back on in the UI
// doesn't lose the tuned number. Triggers are intentionally NOT configurable
// here - they pass through raw (see hid_app.c), which is normal operation
// for a controller.
//
// ---------------------------------------------------------------------------
// ENABLE FLAG vs VALUE (this used to be a footgun)
// ---------------------------------------------------------------------------
// Every stage kernel (axial_deadzone.h, radial_deadzone.h, angular_restrict.h,
// corner_cap.h) early-returns on a value of 0, because a zero-width deadzone
// or a zero-degree snap is mathematically a no-op. That is correct maths and
// wrong UX: the old defaults shipped every value at 0, so flipping a stage ON
// in the editor did nothing at all and looked like broken firmware.
//
// Two changes fix that, and they have to stay in sync:
//   1. Every default VALUE below is non-zero and useful, so a bare toggle is
//      immediately audible on the stick.
//   2. pad_config_sanitize() clamps a value into its stage's usable band
//      whenever that stage is ENABLED - so "enabled with a dead value" is not
//      a representable state, no matter where the struct came from (flash
//      blob written by an older build, a hand-typed PAD_CONFIG.SET, etc).
// A DISABLED stage is never touched by the clamp: parking a stage keeps its
// tuned number intact, which is the whole point of the split.
#ifndef PAD_CONFIG_H
#define PAD_CONFIG_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    // Square-gate correction (see square_to_circle.h). Runs FIRST, because
    // every stage below assumes a circular gate. 0 = passthrough, 100 = fully
    // circular, in between keeps that fraction of the diagonal overshoot.
    bool     left_stick_square_to_circle_enabled;
    uint8_t  left_stick_square_to_circle_pct;
    bool     right_stick_square_to_circle_enabled;
    uint8_t  right_stick_square_to_circle_pct;

    // Per-axis pre-clean deadzone (see axial_deadzone.h). dz value is a
    // half-width, 0..255; int16 units = value*256.
    bool     left_stick_axial_deadzone_enabled;
    uint8_t  left_stick_axial_deadzone;
    bool     right_stick_axial_deadzone_enabled;
    uint8_t  right_stick_axial_deadzone;

    // Circular center deadzone (see radial_deadzone.h). Radius 0..255;
    // int16 units = value*256.
    bool     left_stick_radial_deadzone_enabled;
    uint8_t  left_stick_radial_deadzone;
    bool     right_stick_radial_deadzone_enabled;
    uint8_t  right_stick_radial_deadzone;

    // Angular (cardinal-snap) restriction (see angular_restrict.h).
    // Half-width in degrees, 0..45.
    bool     left_stick_angular_restrict_enabled;
    uint8_t  left_stick_angular_restrict_deg;
    bool     right_stick_angular_restrict_enabled;
    uint8_t  right_stick_angular_restrict_deg;

    // Diagonal corner cap (see corner_cap.h). Percent-over-full: 100 = hard
    // unit circle, 101..142 = soft cap at that percent of full scale.
    bool     left_stick_corner_cap_enabled;
    uint8_t  left_stick_corner_cap_pct;
    bool     right_stick_corner_cap_enabled;
    uint8_t  right_stick_corner_cap_pct;

    // Final proportional magnitude rescale (see output_scale.h). Percent of
    // full output range, 50..150; 100 is NEUTRAL, not off. Under 100 keeps
    // the game's Max Input Threshold unreachable so look Acceleration never
    // engages; over 100 trades range for response.
    bool     left_stick_output_scale_enabled;
    uint8_t  left_stick_output_scale_pct;
    bool     right_stick_output_scale_enabled;
    uint8_t  right_stick_output_scale_pct;

    // Tangential dither (see dither.h). Runs LAST - nothing may follow it.
    // Amplitude in TENTHS of a degree of alternating rotation; magnitude is
    // preserved exactly, only the angle oscillates.
    bool     left_stick_dither_enabled;
    uint8_t  left_stick_dither_amp_deg10;
    bool     right_stick_dither_enabled;
    uint8_t  right_stick_dither_amp_deg10;
} pad_config_t;

// ---------------------------------------------------------------------------
// Usable bands per stage. These are the SAME numbers the web editor uses for
// its slider ranges (see the STAGES table in the editor HTML) - keep them in
// sync or the UI and the firmware will disagree about what a slider means.
// ---------------------------------------------------------------------------

// Deadzones: the wire value is a half-width/radius in units of 256 int16
// counts, so fraction-of-full-scale = value/128. The byte would allow 255
// (199% of full travel - the stick would be permanently dead), so the useful
// band stops at 64 = 50%. 2 is the smallest value that does anything
// measurable above sampling noise.
#define PAD_DEADZONE_MIN   2u
#define PAD_DEADZONE_MAX   64u

// Angular restriction: half-width of the pull zone around each cardinal.
// 45 is the sector edge (everything snaps to a cardinal, diagonals become
// unreachable), so that is the hard ceiling from angular_restrict.h itself.
#define PAD_ANGULAR_MIN    1u
#define PAD_ANGULAR_MAX    45u

// Corner cap: percent of full scale at which the vector magnitude is capped.
// Below 100 the cap would bite on pure cardinals too (a global gain cut, not
// a corner trim), which is not what this stage is for. Above ~141.4 the cap
// circle sits entirely outside the reachable square (max diagonal magnitude
// is sqrt(2)*32767 = 46340 = 141.4% of full scale), so it can never trigger -
// 142 is the last value that isn't inert.
#define PAD_CORNER_CAP_MIN 100u
#define PAD_CORNER_CAP_MAX 142u

// Output scale: percent of full output range. 100 is the NEUTRAL value and
// sits in the middle of the band, so unlike the other stages an enabled
// output-scale stage at its neutral value is a legitimate (if inert) state -
// sanitize only holds it inside 50..150 rather than forcing it off neutral.
// 96 is the Max-Input-Threshold isolation value; 88..92 suits a raw (square)
// gate, where the diagonals reach further.
// Square->circle blend, percent. 0 is passthrough, so an ENABLED stage sitting
// at 0 would be a silent no-op - hence a floor of 1. 100 is the exact inverse
// of a 1/max(|cos|,|sin|) square remap.
#define PAD_SQUARE_TO_CIRCLE_MIN 1u
#define PAD_SQUARE_TO_CIRCLE_MAX 100u

// Dither amplitude, tenths of a degree. Floor of 1 because 0 is the no-op.
// Ceiling of 50 (5.0 deg): past that the perturbation stops reading as a
// sustained input and starts reading as the aim wandering on its own - at full
// deflection 5 deg already moves the tip by ~2850 counts, over 11 quantisation
// steps on an 8-bit pad.
#define PAD_DITHER_MIN 1u
#define PAD_DITHER_MAX 50u

#define PAD_OUTPUT_SCALE_MIN     50u
#define PAD_OUTPUT_SCALE_MAX     150u
#define PAD_OUTPUT_SCALE_NEUTRAL 100u

// Compiled-in defaults - used to initialize g_pad_config (pad_config.c) and
// as the fallback whenever the flash-persisted blob is absent or invalid
// (pad_config_store.c). Keep this the single source of truth for defaults.
//
// Enable flags: only the left radial deadzone starts ON, which preserves the
// tuned strafe behaviour this build already shipped with. Every other stage
// starts OFF but PRE-LOADED with a sensible value, so switching one on in the
// editor is a real, immediately noticeable change rather than a silent no-op.
// Measured baseline for this build: with the pad's own rectangular boundary
// algorithm turned off and the stick recalibrated, the gate reads ~4.3% average
// circularity error and rests within a count of centre on both axes. There is
// nothing left for any of these stages to correct, so every one of them ships
// DISABLED - the pipeline is a passthrough until a measurement says otherwise.
// The values below are what each stage lands on when you switch it on, chosen
// so a bare toggle does something useful rather than nothing.
#define PAD_CONFIG_DEFAULTS { \
    /* Square->circle: only needed if the pad's square remap can't be turned \
       off, or if a partial correction is wanted. Ours can, so: off. */ \
    .left_stick_square_to_circle_enabled = false, \
    .left_stick_square_to_circle_pct = 100, \
    .right_stick_square_to_circle_enabled = false, \
    .right_stick_square_to_circle_pct = 100, \
    /* Axial: per-axis pre-clean. Small - this stage squares off the gate and \
       is the one most likely to hurt diagonals, so it starts conservative. */ \
    .left_stick_axial_deadzone_enabled = false, \
    .left_stick_axial_deadzone = 8,   /* 8*256/32768 = 6.3% per axis */ \
    .right_stick_axial_deadzone_enabled = false, \
    .right_stick_axial_deadzone = 6,  /* 4.7% - look stick wants less */ \
    \
    /* Radial: circular centre deadzone, the one stage that is on by default. */ \
    /* Was on at 14 (10.9%) with no measurement behind it. The stick now rests \
       within a count of centre, so a centre deadzone of any size is dead \
       weight; 5 (3.9%) is the value to use if drift ever appears. */ \
    .left_stick_radial_deadzone_enabled = false, \
    .left_stick_radial_deadzone = 5, \
    .right_stick_radial_deadzone_enabled = false, \
    .right_stick_radial_deadzone = 8, /* 6.3% */ \
    \
    /* Angular: cardinal snap. A few degrees is all it takes; large values \
       start eating real diagonal aim. */ \
    .left_stick_angular_restrict_enabled = false, \
    .left_stick_angular_restrict_deg = 5,  /* strafe cardinals */ \
    .right_stick_angular_restrict_enabled = false, \
    .right_stick_angular_restrict_deg = 4, /* R2P Y-bulge trim */ \
    \
    /* Corner cap: bounds the diagonal tip only. 100 = hard unit circle. \
       With the BBW rectangle boundary algorithm OFF the R2P gate is already \
       circular, so there is no natural overshoot left to trim and this stage \
       is inert on this pad - kept for square-gate inputs. */ \
    .left_stick_corner_cap_enabled = false, \
    .left_stick_corner_cap_pct = 100, \
    .right_stick_corner_cap_enabled = false, \
    .right_stick_corner_cap_pct = 100, \
    \
    /* Output scale: off by default. The right stick is pre-loaded with 96, \
       the value that puts Infinite's Max Input Threshold out of reach so the \
       base aim curve can be evaluated without Acceleration. */ \
    .left_stick_output_scale_enabled = false, \
    .left_stick_output_scale_pct = 100, \
    .right_stick_output_scale_enabled = false, \
    .right_stick_output_scale_pct = 96, \
    \
    /* Dither: off. Amplitudes differ by stick because the side effects do - \
       on the left it wobbles movement DIRECTION by a fraction of a degree at \
       constant speed (cheap), on the right it visibly moves the reticle \
       (expensive), so the look stick starts half as large. */ \
    .left_stick_dither_enabled = false, \
    .left_stick_dither_amp_deg10 = 20, /* 2.0 deg */ \
    .right_stick_dither_enabled = false, \
    .right_stick_dither_amp_deg10 = 10, /* 1.0 deg */ \
}

// Defined once in pad_config.c. Written by cdc_config.c (core0, in response
// to PAD_CONFIG.SET), read every frame by hid_app.c's process_xinput()
// (core1). No lock: individual uint8_t/bool field writes are atomic on ARM
// and no per-frame invariant spans two fields, so the worst case from a torn
// read is one transitional frame mixing an old and a new field value.
extern pad_config_t g_pad_config;

// Clamps every ENABLED stage's value into its usable band (the PAD_*_MIN /
// PAD_*_MAX pairs above); values belonging to DISABLED stages are left
// exactly as-is so a parked tuning survives the round trip.
//
// Idempotent and allocation-free. Call it on every path that can introduce a
// config from outside this build:
//   - after pad_config_store_load()  (blob written by an older firmware)
//   - after PAD_CONFIG.SET           (any host, including a hand-typed line)
// Returns true if it changed anything, so the caller can report the clamp
// back to the host instead of silently disagreeing with the UI.
bool pad_config_sanitize(pad_config_t* cfg);

#endif // PAD_CONFIG_H