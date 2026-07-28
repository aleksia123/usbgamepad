#include "pad_config.h"

pad_config_t g_pad_config = PAD_CONFIG_DEFAULTS;

// Clamp one stage value into [lo,hi], but ONLY while that stage is enabled.
// Returns true if the value moved.
//
// The enabled gate is the important half: a disabled stage keeps whatever the
// user parked there (that's why the enable flag and the value are separate
// fields in the first place), while an enabled stage can never sit at a value
// its kernel would treat as "off" - which is exactly the state that used to
// make a toggle look broken in the editor.
static bool clamp_if_enabled(bool enabled, uint8_t* v, uint8_t lo, uint8_t hi)
{
    if (!enabled) return false;
    uint8_t before = *v;
    if (*v < lo) *v = lo;
    else if (*v > hi) *v = hi;
    return *v != before;
}

bool pad_config_sanitize(pad_config_t* cfg)
{
    bool changed = false;

    changed |= clamp_if_enabled(cfg->left_stick_square_to_circle_enabled,
                                &cfg->left_stick_square_to_circle_pct,
                                PAD_SQUARE_TO_CIRCLE_MIN, PAD_SQUARE_TO_CIRCLE_MAX);
    changed |= clamp_if_enabled(cfg->right_stick_square_to_circle_enabled,
                                &cfg->right_stick_square_to_circle_pct,
                                PAD_SQUARE_TO_CIRCLE_MIN, PAD_SQUARE_TO_CIRCLE_MAX);

    changed |= clamp_if_enabled(cfg->left_stick_axial_deadzone_enabled,
                                &cfg->left_stick_axial_deadzone,
                                PAD_DEADZONE_MIN, PAD_DEADZONE_MAX);
    changed |= clamp_if_enabled(cfg->right_stick_axial_deadzone_enabled,
                                &cfg->right_stick_axial_deadzone,
                                PAD_DEADZONE_MIN, PAD_DEADZONE_MAX);

    changed |= clamp_if_enabled(cfg->left_stick_radial_deadzone_enabled,
                                &cfg->left_stick_radial_deadzone,
                                PAD_DEADZONE_MIN, PAD_DEADZONE_MAX);
    changed |= clamp_if_enabled(cfg->right_stick_radial_deadzone_enabled,
                                &cfg->right_stick_radial_deadzone,
                                PAD_DEADZONE_MIN, PAD_DEADZONE_MAX);

    changed |= clamp_if_enabled(cfg->left_stick_angular_restrict_enabled,
                                &cfg->left_stick_angular_restrict_deg,
                                PAD_ANGULAR_MIN, PAD_ANGULAR_MAX);
    changed |= clamp_if_enabled(cfg->right_stick_angular_restrict_enabled,
                                &cfg->right_stick_angular_restrict_deg,
                                PAD_ANGULAR_MIN, PAD_ANGULAR_MAX);

    changed |= clamp_if_enabled(cfg->left_stick_corner_cap_enabled,
                                &cfg->left_stick_corner_cap_pct,
                                PAD_CORNER_CAP_MIN, PAD_CORNER_CAP_MAX);
    changed |= clamp_if_enabled(cfg->right_stick_corner_cap_enabled,
                                &cfg->right_stick_corner_cap_pct,
                                PAD_CORNER_CAP_MIN, PAD_CORNER_CAP_MAX);

    // Output scale is the odd one out: its neutral value (100) sits in the
    // middle of the band rather than at the bottom, so there is no "enabled
    // but dead" state to rescue it from - just hold it inside the band.
    changed |= clamp_if_enabled(cfg->left_stick_output_scale_enabled,
                                &cfg->left_stick_output_scale_pct,
                                PAD_OUTPUT_SCALE_MIN, PAD_OUTPUT_SCALE_MAX);
    changed |= clamp_if_enabled(cfg->right_stick_output_scale_enabled,
                                &cfg->right_stick_output_scale_pct,
                                PAD_OUTPUT_SCALE_MIN, PAD_OUTPUT_SCALE_MAX);

    changed |= clamp_if_enabled(cfg->left_stick_dither_enabled,
                                &cfg->left_stick_dither_amp_deg10,
                                PAD_DITHER_MIN, PAD_DITHER_MAX);
    changed |= clamp_if_enabled(cfg->right_stick_dither_enabled,
                                &cfg->right_stick_dither_amp_deg10,
                                PAD_DITHER_MIN, PAD_DITHER_MAX);

    return changed;
}