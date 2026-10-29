/*
 * amp_volume – linear gain to the TAS5825M digital volume register (pure C).
 *
 * Requirements: AUD-05, AUD-06, AUD-07 (see the requirements doc).
 * Tests:        test/test_amp_volume/
 *
 * Register 0x4C (DIG_VOL) of the TAS5825M:
 *   code 0x00 = +24 dB, each step -0.5 dB, 0x30 = 0 dB, 0xFE = -103 dB,
 *   0xFF = mute.
 *   dB(code) = 24 - 0.5 * code  for code < 0xFF
 *
 * Conversion from a linear gain g (the product of the user volume from
 * lib/volume and fade_gain from lib/mode_controller):
 *   g not a finite number, or g <= 0   →  0xFF (mute)
 *   g >= 1                             →  0x30 (0 dB; this module never boosts)
 *   otherwise  code = 0x30 + round(-2 * 20*log10(g))   (halves away from zero),
 *              capped at 0xFE: a gain above zero is never reported as mute,
 *              so a fade reaches mute exactly when fade_gain reaches 0.
 *
 * The amplifier is written only when the code changes, so the 0.5 dB
 * quantisation also limits I2C traffic during a fade.
 */
#ifndef AMP_VOLUME_H
#define AMP_VOLUME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMP_VOL_0DB  0x30u
#define AMP_VOL_MUTE 0xFFu

uint8_t amp_volume_code(float gain);

/* amp_volume_code(volume_gain * fade_gain), with the same invalid-input rules
 * applied to each factor. If both factors are valid (finite, > 0) but their
 * float product underflows to 0, the result is 0xFE, not mute. */
uint8_t amp_volume_code_for(float volume_gain, float fade_gain);

/* The register's level in dB; -INFINITY for AMP_VOL_MUTE. */
float amp_volume_db(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* AMP_VOLUME_H */
