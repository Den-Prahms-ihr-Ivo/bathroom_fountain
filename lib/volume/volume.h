/*
 * volume – encoder steps to output gain, with a cap and flash-friendly saving
 * (pure C, no hardware).
 *
 * Requirements: AUD-05 (see the requirements doc).
 * Tests:        test/test_volume/
 *
 * The volume is an integer step 0 … cfg.max_step (the cap, at most cfg.steps).
 *   step 0                 mute, gain 0.0
 *   step s in 1 … steps    gain in dB = cfg.min_db * (steps - s) / (steps - 1)
 *                          so step 1 = cfg.min_db and step == steps = 0 dB (1.0)
 * Equal steps in dB sound even to the ear; linear steps would bunch all the
 * audible change at the bottom of the knob.
 *
 * Saving (AUD-05, protecting the flash): the control task asks
 * volume_take_save() regularly. It returns true once the step has rested for
 * cfg.save_delay_ms after the last change, and only if the step differs from
 * the value last saved (or restored at create).
 *
 * Time is injected as a wrapping uint32 millisecond counter.
 */
#ifndef VOLUME_H
#define VOLUME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t steps;          /* full scale                       default 32    */
    uint8_t max_step;       /* cap, 1 … steps                   default 32    */
    uint8_t default_step;   /* used when nothing is saved       default 12    */
    float min_db;           /* gain at step 1, must be < 0      default -48.0 */
    uint32_t save_delay_ms; /* rest time before saving          default 2000  */
} volume_config_t;

typedef struct volume volume_t;

void volume_config_default(volume_config_t *cfg);

/*
 * Returns NULL if cfg is NULL or invalid: steps < 2, max_step == 0,
 * max_step > steps, default_step > max_step, min_db >= 0.
 * If has_saved is true, saved_step is the value read from NVS; it is clamped
 * to max_step (the cap may have been lowered since it was saved).
 */
volume_t *volume_create(const volume_config_t *cfg, bool has_saved, uint8_t saved_step);

/* Accepts NULL. */
void volume_destroy(volume_t *v);

/* Current step. */
uint8_t volume_step(const volume_t *v);

/* Apply encoder detents (positive = louder), clamped to 0 … max_step. */
void volume_turn(volume_t *v, int32_t detents, uint32_t now_ms);

/* Linear gain 0.0 … 1.0 for the current step. */
float volume_gain(const volume_t *v);

/* See "Saving" above. On true, *step_out holds the step to write to NVS. */
bool volume_take_save(volume_t *v, uint32_t now_ms, uint8_t *step_out);

#ifdef __cplusplus
}
#endif

#endif /* VOLUME_H */
