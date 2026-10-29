/*
 * amp_volume – linear gain to the TAS5825M digital volume register.
 *
 * The contract is in amp_volume.h, the tests in test/test_amp_volume/.
 *
 * Gains are handled as log10 values, so the product of volume and fade is a
 * sum: it can neither underflow to 0 nor overflow to infinity in float.
 */
#include "amp_volume.h"

#include <math.h>
#include <stdbool.h>

#define AMP_VOL_QUIETEST 0xFEu

static bool gain_valid(float g)
{
    return isfinite(g) && g > 0.0f;
}

/* Code for a valid (finite, > 0) gain given as log10(gain). */
static uint8_t code_from_log10(float lg)
{
    if (lg >= 0.0f) {
        return AMP_VOL_0DB; /* never boost */
    }
    /* Steps of 0.5 dB below 0 dB: -2 * 20 * log10(g). */
    const float steps = roundf(-40.0f * lg); /* halves away from zero */
    if (steps >= (float)(AMP_VOL_QUIETEST - AMP_VOL_0DB)) {
        return AMP_VOL_QUIETEST; /* above zero is never mute */
    }
    return (uint8_t)(AMP_VOL_0DB + (unsigned)steps);
}

uint8_t amp_volume_code(float gain)
{
    return gain_valid(gain) ? code_from_log10(log10f(gain)) : AMP_VOL_MUTE;
}

uint8_t amp_volume_code_for(float volume_gain, float fade_gain)
{
    if (!gain_valid(volume_gain) || !gain_valid(fade_gain)) {
        return AMP_VOL_MUTE;
    }
    return code_from_log10(log10f(volume_gain) + log10f(fade_gain));
}

float amp_volume_db(uint8_t code)
{
    if (code == AMP_VOL_MUTE) {
        return -INFINITY;
    }
    return 24.0f - 0.5f * (float)code;
}
