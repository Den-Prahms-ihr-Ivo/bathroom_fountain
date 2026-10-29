/*
 * mode_controller – Normal/Ambient state machine.
 *
 * The contract is in mode_controller.h, the tests in test/test_mode_controller/.
 *
 * All time arithmetic uses unsigned differences (now_ms - start_ms), which
 * stay correct across the 32-bit millisecond counter wrap.
 */
#include "mode_controller.h"

#include <stdlib.h>
#include <string.h>

#define TRANSITION_MIN_MS 3000u
#define TRANSITION_MAX_MS 5000u

struct mode_controller {
    mode_config_t cfg;
    mode_outputs_t out;

    uint32_t transition_start_ms;
    float transition_start_gain;

    bool lockout_active;          /* a transition has ended and lockout may still run */
    uint32_t transition_end_ms;

    bool hue_pending;
    hue_command_t hue;
};

void mode_config_default(mode_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->transition_ms = 4000;
        cfg->lockout_ms = 1000;
    }
}

static void set_all_off(mode_outputs_t *o)
{
    o->candles_on = false;
    o->fountain_on = false;
    o->music_playing = false;
    o->amp_enabled = false;
    o->fade_gain = 0.0f;
}

/* Bring fades, transition end and lockout up to date for now_ms. */
static void update(mode_controller_t *mc, uint32_t now_ms)
{
    if (mc->out.transitioning) {
        const uint32_t elapsed = now_ms - mc->transition_start_ms;
        if ((int32_t)elapsed < 0) {
            return; /* stale timestamp from before the trigger */
        }

        const float target = (mc->out.mode == AMBIENCE_AMBIENT) ? 1.0f : 0.0f;
        if (elapsed >= mc->cfg.transition_ms) {
            mc->out.transitioning = false;
            mc->out.fade_gain = target;
            /* Lockout counts from the scheduled end, not from when we noticed it. */
            mc->transition_end_ms = mc->transition_start_ms + mc->cfg.transition_ms;
            mc->lockout_active = true;
            if (mc->out.mode == AMBIENCE_NORMAL) {
                set_all_off(&mc->out); /* BEH-07, AUD-07 */
            }
        } else {
            const float frac = (float)elapsed / (float)mc->cfg.transition_ms;
            const float start = mc->transition_start_gain;
            mc->out.fade_gain = start + (target - start) * frac;
        }
    }

    if (mc->lockout_active && !mc->out.transitioning) {
        const uint32_t since_end = now_ms - mc->transition_end_ms;
        if ((int32_t)since_end >= 0 && since_end >= mc->cfg.lockout_ms) {
            mc->lockout_active = false;
        }
    }
}

mode_controller_t *mode_controller_create(const mode_config_t *cfg, uint32_t now_ms)
{
    (void)now_ms;

    if (cfg == NULL || cfg->transition_ms < TRANSITION_MIN_MS || cfg->transition_ms > TRANSITION_MAX_MS) {
        return NULL;
    }

    mode_controller_t *mc = calloc(1, sizeof(*mc));
    if (mc == NULL) {
        return NULL;
    }

    mc->cfg = *cfg;
    mc->out.mode = AMBIENCE_NORMAL;
    mc->out.transitioning = false;
    set_all_off(&mc->out); /* BEH-02 */
    return mc;
}

void mode_controller_destroy(mode_controller_t *mc)
{
    free(mc);
}

bool mode_controller_trigger(mode_controller_t *mc, trigger_source_t source, uint32_t now_ms)
{
    (void)source; /* clap and button are equivalent (BEH-03/04) */

    if (mc == NULL) {
        return false;
    }

    update(mc, now_ms);
    if (mc->out.transitioning || mc->lockout_active) {
        return false; /* BEH-08 */
    }

    const bool entering = (mc->out.mode == AMBIENCE_NORMAL);

    mc->out.mode = entering ? AMBIENCE_AMBIENT : AMBIENCE_NORMAL;
    mc->out.transitioning = true;
    mc->transition_start_ms = now_ms;
    mc->transition_start_gain = mc->out.fade_gain;

    if (entering) {
        /* BEH-05: everything on at the trigger, music fades in from 0. */
        mc->out.candles_on = true;
        mc->out.fountain_on = true;
        mc->out.amp_enabled = true;
        mc->out.music_playing = true;
    }

    /* A newer command replaces one that has not been taken yet. */
    mc->hue.scene = entering ? HUE_SCENE_AMBIENT : HUE_SCENE_NORMAL;
    mc->hue.duration_ms = mc->cfg.transition_ms;
    mc->hue_pending = true;
    return true;
}

void mode_controller_tick(mode_controller_t *mc, uint32_t now_ms)
{
    if (mc != NULL) {
        update(mc, now_ms);
    }
}

const mode_outputs_t *mode_controller_outputs(const mode_controller_t *mc)
{
    return (mc != NULL) ? &mc->out : NULL;
}

bool mode_controller_take_hue_command(mode_controller_t *mc, hue_command_t *out)
{
    if (mc == NULL || out == NULL || !mc->hue_pending) {
        return false;
    }
    *out = mc->hue;
    mc->hue_pending = false;
    return true;
}
