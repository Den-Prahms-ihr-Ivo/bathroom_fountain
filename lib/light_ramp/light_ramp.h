/*
 * light_ramp – brightness over time for one LED channel: linear fades in
 * perceived brightness, gamma-corrected PWM duty (pure C).
 *
 * Requirements: BED-30 ... BED-33 (see the requirements doc).
 * Tests:        test/test_light_ramp/
 *
 * The bedside node drives its LED strips with 14-bit LEDC PWM. A sunrise
 * that looks even to the eye needs a gamma curve and fine steps at the dark
 * end; both are here. One object per channel. Time is a wrapping uint32
 * millisecond counter.
 *
 * Level
 *   0 ... 1000 (per mille of full brightness, as the eye sees it).
 *
 * Duty (light_ramp_duty)
 *   duty = round(max_duty * (level / 1000) ^ gamma), with two fixed points:
 *   level 0 gives 0, and any level above 0 gives at least 1 (a channel that
 *   is "on" is never rounded down to dark). Level 1000 gives max_duty. The
 *   result never decreases when the level rises. Levels above 1000 count as
 *   1000.
 *
 * Fades
 *   light_ramp_set(target, duration_ms) starts a fade from the level the
 *   channel has at that moment to target; the level moves linearly in time
 *   and reaches target exactly after duration_ms. Duration 0 jumps. A new
 *   call during a fade starts from the current level, without a step.
 *   light_ramp_level() brings the fade up to date for now_ms and returns the
 *   level; light_ramp_busy() says whether a fade is still running.
 *   At create the level is 0 and nothing is running.
 */
#ifndef LIGHT_RAMP_H
#define LIGHT_RAMP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t max_duty; /* 255 ... 16383 (2^bits - 1)   default 16383 */
    float gamma;       /* 1.0 ... 3.0                  default 2.2   */
} light_ramp_config_t;

typedef struct light_ramp light_ramp_t;

void light_ramp_config_default(light_ramp_config_t *cfg);

/* PWM duty for a level. cfg NULL or invalid gives 0. */
uint16_t light_ramp_duty(const light_ramp_config_t *cfg, uint16_t level_permille);

/* Returns NULL if cfg is NULL or a value is out of range. */
light_ramp_t *light_ramp_create(const light_ramp_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void light_ramp_destroy(light_ramp_t *lr);

/* target above 1000 counts as 1000; duration_ms up to 4000000 (about 66 min),
 * longer counts as that. */
void light_ramp_set(light_ramp_t *lr, uint16_t target_permille, uint32_t duration_ms, uint32_t now_ms);

uint16_t light_ramp_level(light_ramp_t *lr, uint32_t now_ms);

/* Duty of the current level: light_ramp_duty(cfg, light_ramp_level(now)). */
uint16_t light_ramp_now(light_ramp_t *lr, uint32_t now_ms);

bool light_ramp_busy(light_ramp_t *lr, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* LIGHT_RAMP_H */
