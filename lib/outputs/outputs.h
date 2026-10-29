/*
 * outputs – decides which output changes the control task must carry out,
 * in which order, and retries the ones that failed (pure C, no hardware).
 *
 * Requirements: BEH-05, BEH-06, BEH-07, AUD-05, AUD-06, AUD-07
 * Tests:        test/test_outputs/
 * Depends on:   mode_controller.h (mode_outputs_t), amp_volume.h, fade_curve.h
 *
 * The control task, every tick:
 *   outputs_want_from_mode(mode_controller_outputs(mc), volume_gain(v), &want);
 *   n = outputs_plan(o, &want, actions);
 *   for each action in order: carry it out (GPIO, tas5825m, player command);
 *     on success call outputs_done(o, &action); on failure stop for this tick.
 *
 * The module remembers what was actually applied (confirmed through
 * outputs_done). Each item is either unknown or has a known applied value.
 * At create, and after outputs_invalidate(), everything is unknown.
 *
 * outputs_plan(want) lists, in exactly this order, the items that are unknown
 * or differ from want:
 *   1. OUTPUT_CANDLES     value 0/1
 *   2. OUTPUT_FOUNTAIN    value 0/1
 *   3. OUTPUT_MUSIC       value 0   (stop music before the amplifier goes off)
 *   4. OUTPUT_AMP_POWER   value 0/1
 *   5. OUTPUT_VOLUME      value = DIG_VOL code
 *   6. OUTPUT_MUSIC       value 1   (start music after the amplifier is up)
 * OUTPUT_MUSIC appears at most once (position 3 or 6, by its value).
 * OUTPUT_VOLUME appears only if want.amp_on, and then:
 *   - always when OUTPUT_AMP_POWER 1 is in the same plan (a fresh power-up
 *     comes up muted, so the volume must be sent again), otherwise
 *   - when the applied volume is unknown or differs from want.vol_code.
 * want.vol_code is ignored while want.amp_on is false.
 * Planning changes nothing; the same want gives the same plan until
 * outputs_done() or outputs_invalidate() is called.
 *
 * outputs_done(action) records the action's value as applied. In addition,
 * OUTPUT_AMP_POWER (either value) makes the applied volume unknown, so
 * confirm actions in plan order.
 *
 * At most OUTPUTS_MAX_ACTIONS actions are planned.
 */
#ifndef OUTPUTS_H
#define OUTPUTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mode_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OUTPUTS_MAX_ACTIONS 5

typedef struct {
    bool candles;
    bool fountain;
    bool amp_on;
    bool music;
    uint8_t vol_code; /* TAS5825M DIG_VOL code; only used while amp_on */
} outputs_state_t;

typedef enum {
    OUTPUT_CANDLES = 0,
    OUTPUT_FOUNTAIN,
    OUTPUT_MUSIC,
    OUTPUT_AMP_POWER,
    OUTPUT_VOLUME,
} output_kind_t;

typedef struct {
    output_kind_t kind;
    uint8_t value; /* 0/1 for switches, DIG_VOL code for OUTPUT_VOLUME */
} output_action_t;

typedef struct outputs outputs_t;

outputs_t *outputs_create(void);

/* Accepts NULL. */
void outputs_destroy(outputs_t *o);

/* Returns the number of actions written to actions[] (0 … OUTPUTS_MAX_ACTIONS).
 * Returns 0 if any argument is NULL. */
size_t outputs_plan(const outputs_t *o, const outputs_state_t *want,
                    output_action_t actions[OUTPUTS_MAX_ACTIONS]);

/* Ignores NULL arguments and unknown kinds. */
void outputs_done(outputs_t *o, const output_action_t *action);

/* Everything unknown again (e.g. after an amplifier fault or reset). Accepts NULL. */
void outputs_invalidate(outputs_t *o);

/*
 * The wanted state for the current mode outputs and user volume:
 *   candles, fountain, music   copied from mode
 *   amp_on                     mode->amp_enabled
 *   vol_code                   amp_volume_code_for(volume_gain,
 *                                fade_curve_gain(mode->fade_gain,
 *                                                FADE_CURVE_FLOOR_DB_DEFAULT))
 * Leaves want untouched if mode or want is NULL.
 */
void outputs_want_from_mode(const mode_outputs_t *mode, float volume_gain,
                            outputs_state_t *want);

#ifdef __cplusplus
}
#endif

#endif /* OUTPUTS_H */
