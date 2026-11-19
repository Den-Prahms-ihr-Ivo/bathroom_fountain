/*
 * light_ramp – linear fades in perceived brightness, gamma-corrected duty.
 * Contract in light_ramp.h, tests in test/test_light_ramp/.
 */
#include "light_ramp.h"

#include <math.h>
#include <stdlib.h>

#define LEVEL_MAX 1000u
#define DURATION_MAX 4000000u

struct light_ramp {
    light_ramp_config_t cfg;
    uint16_t from;
    uint16_t target;
    uint32_t start_ms;
    uint32_t duration_ms;
    bool busy;
};

void light_ramp_config_default(light_ramp_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->max_duty = 16383;
        cfg->gamma = 2.2f;
    }
}

static bool config_ok(const light_ramp_config_t *cfg)
{
    return cfg != NULL && cfg->max_duty >= 255 && cfg->max_duty <= 16383 && cfg->gamma >= 1.0f &&
           cfg->gamma <= 3.0f;
}

uint16_t light_ramp_duty(const light_ramp_config_t *cfg, uint16_t level_permille)
{
    if (!config_ok(cfg) || level_permille == 0) {
        return 0;
    }
    if (level_permille >= LEVEL_MAX) {
        return cfg->max_duty;
    }
    const float x = (float)level_permille / (float)LEVEL_MAX;
    const long d = lroundf((float)cfg->max_duty * powf(x, cfg->gamma));
    if (d < 1) {
        return 1;
    }
    return d > cfg->max_duty ? cfg->max_duty : (uint16_t)d;
}

light_ramp_t *light_ramp_create(const light_ramp_config_t *cfg, uint32_t now_ms)
{
    if (!config_ok(cfg)) {
        return NULL;
    }
    light_ramp_t *lr = calloc(1, sizeof(*lr));
    if (lr != NULL) {
        lr->cfg = *cfg;
        lr->start_ms = now_ms;
    }
    return lr;
}

void light_ramp_destroy(light_ramp_t *lr)
{
    free(lr);
}

uint16_t light_ramp_level(light_ramp_t *lr, uint32_t now_ms)
{
    if (lr == NULL) {
        return 0;
    }
    if (lr->busy) {
        const uint32_t elapsed = now_ms - lr->start_ms;
        if (elapsed >= lr->duration_ms) {
            lr->busy = false;
            lr->from = lr->target;
        } else {
            const int64_t delta = (int64_t)lr->target - (int64_t)lr->from;
            const int64_t level = (int64_t)lr->from + delta * (int64_t)elapsed / (int64_t)lr->duration_ms;
            return (uint16_t)level;
        }
    }
    return lr->target;
}

void light_ramp_set(light_ramp_t *lr, uint16_t target_permille, uint32_t duration_ms, uint32_t now_ms)
{
    if (lr == NULL) {
        return;
    }
    const uint16_t current = light_ramp_level(lr, now_ms);
    const uint16_t target = target_permille > LEVEL_MAX ? (uint16_t)LEVEL_MAX : target_permille;
    lr->from = current;
    lr->target = target;
    lr->start_ms = now_ms;
    lr->duration_ms = duration_ms > DURATION_MAX ? DURATION_MAX : duration_ms;
    lr->busy = lr->duration_ms > 0 && current != target;
    if (!lr->busy) {
        lr->from = target;
    }
}

uint16_t light_ramp_now(light_ramp_t *lr, uint32_t now_ms)
{
    if (lr == NULL) {
        return 0;
    }
    return light_ramp_duty(&lr->cfg, light_ramp_level(lr, now_ms));
}

bool light_ramp_busy(light_ramp_t *lr, uint32_t now_ms)
{
    if (lr == NULL) {
        return false;
    }
    (void)light_ramp_level(lr, now_ms);
    return lr->busy;
}
