/*
 * outputs – which output changes to carry out, in which order.
 *
 * The contract is in outputs.h, the tests in test/test_outputs/.
 */
#include "outputs.h"

#include <stdlib.h>

#include "amp_volume.h"
#include "fade_curve.h"

/* One remembered output: unknown, or the value last confirmed. */
typedef struct {
    bool known;
    uint8_t value;
} applied_t;

struct outputs {
    applied_t candles;
    applied_t fountain;
    applied_t music;
    applied_t amp;
    applied_t vol;
};

outputs_t *outputs_create(void)
{
    return calloc(1, sizeof(outputs_t)); /* everything unknown */
}

void outputs_destroy(outputs_t *o)
{
    free(o);
}

static bool needs(applied_t a, uint8_t want)
{
    return !a.known || a.value != want;
}

static void add(output_action_t *actions, size_t *n, output_kind_t kind, uint8_t value)
{
    actions[*n].kind = kind;
    actions[*n].value = value;
    (*n)++;
}

size_t outputs_plan(const outputs_t *o, const outputs_state_t *want,
                    output_action_t actions[OUTPUTS_MAX_ACTIONS])
{
    if (o == NULL || want == NULL || actions == NULL) {
        return 0;
    }

    size_t n = 0;
    const bool music_change = needs(o->music, want->music);
    const bool power_change = needs(o->amp, want->amp_on);

    if (needs(o->candles, want->candles)) {
        add(actions, &n, OUTPUT_CANDLES, want->candles);
    }
    if (needs(o->fountain, want->fountain)) {
        add(actions, &n, OUTPUT_FOUNTAIN, want->fountain);
    }
    if (music_change && !want->music) {
        add(actions, &n, OUTPUT_MUSIC, 0); /* stop before the amplifier goes off */
    }
    if (power_change) {
        add(actions, &n, OUTPUT_AMP_POWER, want->amp_on);
    }
    /* A fresh power-up comes up muted, so the volume always follows it. */
    if (want->amp_on && (power_change || needs(o->vol, want->vol_code))) {
        add(actions, &n, OUTPUT_VOLUME, want->vol_code);
    }
    if (music_change && want->music) {
        add(actions, &n, OUTPUT_MUSIC, 1); /* start once the amplifier is up */
    }
    return n;
}

static applied_t *item(outputs_t *o, output_kind_t kind)
{
    switch (kind) {
    case OUTPUT_CANDLES: return &o->candles;
    case OUTPUT_FOUNTAIN: return &o->fountain;
    case OUTPUT_MUSIC: return &o->music;
    case OUTPUT_AMP_POWER: return &o->amp;
    case OUTPUT_VOLUME: return &o->vol;
    }
    return NULL;
}

void outputs_done(outputs_t *o, const output_action_t *action)
{
    if (o == NULL || action == NULL) {
        return;
    }
    applied_t *a = item(o, action->kind);
    if (a == NULL) {
        return;
    }
    a->known = true;
    a->value = (action->kind == OUTPUT_VOLUME) ? action->value : (action->value != 0);
    if (action->kind == OUTPUT_AMP_POWER) {
        o->vol.known = false; /* powered up muted, or off */
    }
}

void outputs_invalidate(outputs_t *o)
{
    if (o != NULL) {
        *o = (outputs_t){0};
    }
}

void outputs_want_from_mode(const mode_outputs_t *mode, float volume_gain,
                            outputs_state_t *want)
{
    if (mode == NULL || want == NULL) {
        return;
    }
    want->candles = mode->candles_on;
    want->fountain = mode->fountain_on;
    want->amp_on = mode->amp_enabled;
    want->music = mode->music_playing;
    want->vol_code = amp_volume_code_for(volume_gain,
                                         fade_curve_gain(mode->fade_gain, FADE_CURVE_FLOOR_DB_DEFAULT));
}
