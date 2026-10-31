/*
 * mode_controller – Normal/Ambient state machine (pure C, no hardware).
 *
 * Requirements: BEH-01 … BEH-09, AUD-06, AUD-07 (see the requirements doc).
 * Tests:        test/test_mode_controller/
 *
 * Time is injected as a millisecond counter (e.g. esp_timer_get_time() / 1000
 * truncated to uint32_t). The counter wraps after about 49.7 days and the
 * controller must keep working across the wrap.
 *
 * Behaviour
 * ---------
 * Start (BEH-02)    mode NORMAL, nothing transitioning, all outputs off,
 *                   amplifier disabled, fade_gain 0, no Hue command pending.
 *
 * Trigger           The button toggles both ways (BEH-04). A clap only
 *                   enters Ambient (BEH-03): in Ambient it is ignored
 *                   (returns false, changes nothing, issues no Hue command)
 *                   unless cfg.clap_exits is true, in which case a clap and a
 *                   button press are equivalent.
 *                   A trigger is accepted unless a transition is running or
 *                   less than cfg.lockout_ms has passed since the last
 *                   transition ended (BEH-08). An accepted trigger flips the
 *                   mode immediately and starts a transition of
 *                   cfg.transition_ms. An ignored trigger has no effect at all
 *                   (it does not restart the lockout).
 *
 * Entering Ambient  (BEH-05) at the trigger: Hue command AMBIENT with
 *                   duration cfg.transition_ms; candles, fountain, amplifier
 *                   and music on. fade_gain rises monotonically from 0 to
 *                   exactly 1 at the end of the transition.
 *
 * Leaving Ambient   (BEH-06/07) at the trigger: Hue command NORMAL with
 *                   duration cfg.transition_ms. fade_gain falls monotonically
 *                   from its current value to 0. Candles, fountain and music
 *                   stay on during the fade; at the end of the transition
 *                   candles, fountain and music go off and the amplifier is
 *                   disabled (AUD-07).
 *
 * Hue commands      One command per accepted trigger, taken exactly once via
 *                   mode_controller_take_hue_command(). If a command has not
 *                   been taken when the next one is issued, the newer one
 *                   replaces it. No commands are issued while a mode is
 *                   simply held (BEH-09).
 *
 * Every call that receives now_ms first brings the state up to date for that
 * time, so a trigger works correctly even if tick() was not called for a while.
 */
#ifndef MODE_CONTROLLER_H
#define MODE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AMBIENCE_NORMAL = 0,
    AMBIENCE_AMBIENT = 1,
} ambience_mode_t;

typedef enum {
    TRIGGER_CLAP = 0,
    TRIGGER_BUTTON = 1,
} trigger_source_t;

typedef enum {
    HUE_SCENE_NORMAL = 0,
    HUE_SCENE_AMBIENT = 1,
} hue_scene_t;

typedef struct {
    uint32_t transition_ms; /* BEH-05/06, AUD-06: must be 3000…5000   default 4000 */
    uint32_t lockout_ms;    /* BEH-08: after a transition ends         default 1000 */
    bool clap_exits;        /* BEH-03: a clap also leaves Ambient      default false */
} mode_config_t;

typedef struct {
    ambience_mode_t mode; /* target mode, flips at the accepted trigger   */
    bool transitioning;
    bool candles_on;
    bool fountain_on;
    bool amp_enabled;     /* false = amplifier in power-down (AUD-07)     */
    bool music_playing;
    float fade_gain;      /* 0.0 … 1.0, multiplies the user volume        */
} mode_outputs_t;

typedef struct {
    hue_scene_t scene;
    uint32_t duration_ms;
} hue_command_t;

typedef struct mode_controller mode_controller_t;

/* Fill cfg with the defaults listed above. */
void mode_config_default(mode_config_t *cfg);

/* Returns NULL if cfg is NULL or invalid (transition_ms outside 3000…5000). */
mode_controller_t *mode_controller_create(const mode_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void mode_controller_destroy(mode_controller_t *mc);

/* Returns true if the trigger was accepted. */
bool mode_controller_trigger(mode_controller_t *mc, trigger_source_t source, uint32_t now_ms);

/* Advance fades and finish transitions. Call regularly (e.g. every 20 ms). */
void mode_controller_tick(mode_controller_t *mc, uint32_t now_ms);

/* Current outputs. The pointer stays valid for the controller's lifetime. */
const mode_outputs_t *mode_controller_outputs(const mode_controller_t *mc);

/* If a Hue command is pending, copy it to out, clear it and return true. */
bool mode_controller_take_hue_command(mode_controller_t *mc, hue_command_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MODE_CONTROLLER_H */
