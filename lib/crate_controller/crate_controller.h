/*
 * crate_controller – the holy crate: lid open = light and choir, lid closed =
 * fade out and sleep (pure C, no hardware).
 *
 * Requirements: CRATE-01 ... CRATE-10, CRATE-13 (listed in docs/CRATE.md); CRATE-11
 *               (volume encoder) is the app's, with lib/volume.
 * Tests:        test/test_crate_controller/
 *
 * The lid is a level, not an event: the crate follows it. The module decides
 * what the light, the volume, the amplifier and the music should do; the
 * crate app turns that into PWM (light_ramp), the TAS5825M volume
 * (fade_curve), the player and deep sleep. Time is a wrapping uint32
 * millisecond counter. Every call that receives now_ms first brings the state
 * up to date.
 *
 * Lid
 *   crate_controller_lid() takes the raw level of the door contact on every
 *   poll. A new level counts once it has been unchanged for cfg.debounce_ms,
 *   timed from the first call that reported it. The level given to create()
 *   counts at once: after a wake-up from deep sleep the lid is already open.
 *
 * States (outputs.state)
 *   CRATE_CLOSED   all off. Light 0, volume 0, amplifier off, no music.
 *   CRATE_OPEN     light fades to cfg.open_level and the volume to cfg.volume,
 *                  both over cfg.open_fade_ms. Music plays.
 *   CRATE_DIM      the lid has been open for cfg.max_open_ms (0 = never):
 *                  light fades to cfg.dim_level and the volume to 0 over
 *                  cfg.close_fade_ms; when that fade is over the music stops
 *                  and the amplifier goes off. The light stays dim while the
 *                  lid stays open.
 *   CRATE_CLOSING  the lid closed: light and volume fade to 0 over
 *                  cfg.close_fade_ms; when that fade is over the music stops
 *                  (if it still played), the amplifier goes off and the state
 *                  is CRATE_CLOSED.
 *
 * Transitions
 *   lid opens   CLOSED or CLOSING -> OPEN. If no music is playing at that
 *               moment, event CRATE_EV_MUSIC_START (start from the first
 *               track: the "aaah" belongs at the beginning). Opening during
 *               CLOSING turns the fades around without restarting the music.
 *               The max-open timer starts.
 *   lid closes  OPEN or DIM -> CLOSING.
 *   timer       OPEN -> DIM after cfg.max_open_ms of open lid.
 *   fade over   CLOSING -> CLOSED; DIM stays DIM (music stopped).
 *   Every change of a light or volume target restarts that fade from the
 *   level the output has at that moment; the app's light_ramp does exactly
 *   that, so the module only gives target and duration.
 *
 * Battery (outputs.battery)
 *   crate_controller_battery() takes the battery voltage in mV whenever the
 *   app measures it. Until the first sample the battery counts as OK.
 *     CRATE_BATT_OK -> LOW    below cfg.warn_mv for cfg.battery_hold_ms
 *     any           -> EMPTY  below cfg.cutoff_mv for cfg.battery_hold_ms
 *     EMPTY -> LOW or OK      at or above cfg.cutoff_mv + 300 for
 *                             cfg.battery_hold_ms (the charger is on);
 *                             OK if also at or above cfg.warn_mv + 300
 *     LOW -> OK               at or above cfg.warn_mv + 300 for
 *                             cfg.battery_hold_ms
 *   "For battery_hold_ms" means every sample during that time; one sample on
 *   the other side restarts the count. The hold is timed from the first
 *   sample of the run.
 *   When the battery becomes EMPTY while the crate is OPEN or DIM, it behaves
 *   as if the lid had closed (CLOSING), and gives event CRATE_EV_BLINK.
 *   Opening the lid while EMPTY gives CRATE_EV_BLINK only: the state stays
 *   CLOSED, no music, no light (the app blinks the strip briefly).
 *
 * Sleep (outputs.sleep_ok)
 *   true in CRATE_CLOSED once cfg.sleep_delay_ms have passed since the state
 *   became CLOSED (or since create, if created closed), provided the lid is
 *   closed or the battery is EMPTY. With an empty battery and the lid open
 *   the app must sleep with "lid closes" as its wake source, or the lid wakes
 *   it at once again.
 *
 * Busy (crate_controller_busy(), CRATE-13)
 *   The crate is online whenever it is awake. The app reports busy = true
 *   while a music download (cmd/music/sync) or a firmware update runs. Busy
 *   changes no light, volume, music or state: it only delays sleep_ok in
 *   CRATE_CLOSED, for at most cfg.busy_max_ms after the sleep delay ended
 *   (counted from the moment sleep_ok would have become true without it).
 *   Busy turning false lets sleep_ok become true at once (if the delay is
 *   over). With the battery EMPTY, busy is ignored: the battery comes first.
 *   Starts not busy.
 *
 * Events are taken once each, in order (at most CRATE_EVENTS wait; the
 * oldest is dropped).
 */
#ifndef CRATE_CONTROLLER_H
#define CRATE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CRATE_EVENTS 4

typedef struct {
    uint16_t debounce_ms;       /* 10 ... 500              default 50     */
    uint16_t open_level;        /* 1 ... 1000 per mille    default 600    */
    uint16_t dim_level;         /* 0 ... open_level        default 150    */
    uint16_t open_fade_ms;      /* 0 ... 10000             default 1500   */
    uint16_t close_fade_ms;     /* 0 ... 20000             default 3000   */
    uint16_t volume;            /* 1 ... 1000 per mille    default 500    */
    uint32_t max_open_ms;       /* 0 (off) or 60000 ... 3600000
                                                           default 900000 */
    uint16_t cutoff_mv;         /* 10000 ... 13500         default 12400  */
    uint16_t warn_mv;           /* cutoff_mv ... 14000     default 12800  */
    uint16_t battery_hold_ms;   /* 1000 ... 60000          default 10000  */
    uint16_t sleep_delay_ms;    /* 0 ... 60000             default 2000   */
    uint32_t busy_max_ms;       /* 0 ... 3600000           default 600000 (10 min) */
} crate_config_t;

typedef enum {
    CRATE_CLOSED = 0,
    CRATE_OPEN,
    CRATE_DIM,
    CRATE_CLOSING,
} crate_state_t;

typedef enum {
    CRATE_BATT_OK = 0,
    CRATE_BATT_LOW,
    CRATE_BATT_EMPTY,
} crate_battery_t;

typedef enum {
    CRATE_EV_MUSIC_START = 1,
    CRATE_EV_MUSIC_STOP,
    CRATE_EV_BLINK,
} crate_event_t;

typedef struct {
    crate_state_t state;
    uint16_t light_permille;  /* target of the current light fade  */
    uint16_t light_fade_ms;   /* its duration                      */
    uint16_t volume_permille; /* target of the current volume fade */
    uint16_t volume_fade_ms;
    bool amp_on;
    bool music_playing;       /* between MUSIC_START and MUSIC_STOP */
    bool lid_open;            /* debounced                          */
    crate_battery_t battery;
    bool sleep_ok;
} crate_outputs_t;

typedef struct crate_controller crate_controller_t;

void crate_config_default(crate_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is out of range. If lid_open is
 * true the crate opens at once (events as for a lid that opens). */
crate_controller_t *crate_controller_create(const crate_config_t *cfg, bool lid_open, uint32_t now_ms);

/* Accepts NULL. */
void crate_controller_destroy(crate_controller_t *cc);

void crate_controller_lid(crate_controller_t *cc, bool open_raw, uint32_t now_ms);
void crate_controller_battery(crate_controller_t *cc, uint16_t battery_mv, uint32_t now_ms);

void crate_controller_busy(crate_controller_t *cc, bool busy, uint32_t now_ms);

/* Call regularly (e.g. every 20 ms). */
void crate_controller_tick(crate_controller_t *cc, uint32_t now_ms);

/* Current outputs. The pointer stays valid for the object's lifetime. */
const crate_outputs_t *crate_controller_outputs(const crate_controller_t *cc);

bool crate_controller_take_event(crate_controller_t *cc, crate_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* CRATE_CONTROLLER_H */
