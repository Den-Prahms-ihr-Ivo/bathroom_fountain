/*
 * volume – encoder steps to output gain, with a cap and flash-friendly saving.
 *
 * The contract is in volume.h, the tests in test/test_volume/.
 */
#include "volume.h"

#include <math.h>
#include <stdlib.h>

struct volume {
    volume_config_t cfg;
    uint8_t step;
    uint8_t saved_step;      /* what NVS holds (or would restore to) */
    bool changed;            /* step changed and the rest time has not been handled yet */
    bool save_now;           /* changed at create (clamped to the cap): no rest time */
    uint32_t last_change_ms;
};

void volume_config_default(volume_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->steps = 32;
        cfg->max_step = 32;
        cfg->default_step = 12;
        cfg->min_db = -48.0f;
        cfg->save_delay_ms = 2000;
    }
}

static bool config_valid(const volume_config_t *cfg)
{
    return cfg->steps >= 2
        && cfg->max_step != 0
        && cfg->max_step <= cfg->steps
        && cfg->default_step <= cfg->max_step
        && cfg->min_db < 0.0f;
}

volume_t *volume_create(const volume_config_t *cfg, bool has_saved, uint8_t saved_step)
{
    if (cfg == NULL || !config_valid(cfg)) {
        return NULL;
    }

    volume_t *v = calloc(1, sizeof(*v));
    if (v == NULL) {
        return NULL;
    }

    v->cfg = *cfg;
    if (has_saved && saved_step > cfg->max_step) {
        /* The cap was lowered since the value was saved (AUD-05). Write the
         * clamped value back, so raising the cap later does not bring the old
         * loud value back. */
        v->step = cfg->max_step;
        v->saved_step = saved_step;
        v->changed = true;
        v->save_now = true;
    } else {
        /* Treat the start value as saved: with nothing in NVS, the default is
         * what a restart would come back to anyway. */
        v->step = has_saved ? saved_step : cfg->default_step;
        v->saved_step = v->step;
    }
    return v;
}

void volume_destroy(volume_t *v)
{
    free(v);
}

uint8_t volume_step(const volume_t *v)
{
    return (v != NULL) ? v->step : 0;
}

void volume_turn(volume_t *v, int32_t detents, uint32_t now_ms)
{
    if (v == NULL) {
        return;
    }

    int64_t next = (int64_t)v->step + detents; /* 64 bit: INT32_MAX/MIN cannot overflow */
    if (next < 0) {
        next = 0;
    } else if (next > v->cfg.max_step) {
        next = v->cfg.max_step;
    }

    if ((uint8_t)next != v->step) {
        v->step = (uint8_t)next;
        v->changed = true;
        v->save_now = false;
        v->last_change_ms = now_ms; /* each change restarts the rest time */
    }
}

float volume_gain(const volume_t *v)
{
    if (v == NULL || v->step == 0) {
        return 0.0f; /* mute */
    }
    const float steps = (float)v->cfg.steps;
    const float db = v->cfg.min_db * (steps - (float)v->step) / (steps - 1.0f);
    return powf(10.0f, db / 20.0f);
}

bool volume_take_save(volume_t *v, uint32_t now_ms, uint8_t *step_out)
{
    if (v == NULL || step_out == NULL || !v->changed) {
        return false;
    }
    if (!v->save_now && now_ms - v->last_change_ms < v->cfg.save_delay_ms) {
        return false; /* still resting; unsigned difference survives the wrap */
    }

    v->changed = false;
    v->save_now = false;
    if (v->step == v->saved_step) {
        return false; /* turned back to where it was: no flash write */
    }
    v->saved_step = v->step;
    *step_out = v->step;
    return true;
}
