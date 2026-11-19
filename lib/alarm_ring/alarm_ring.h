/*
 * alarm_ring – one alarm from sunrise to "off" on the bedside node (pure C).
 *
 * Requirements: BED-20 ... BED-28, GAME-01, GAME-08 (see the requirements doc).
 * Tests:        test/test_alarm_ring/
 *
 * alarm_schedule says when the next alarm is; the bedside app arms this
 * module with the time left until then. From there it runs the sequence:
 * sunrise light, ringing, snooze, and the ways an alarm ends. It only
 * decides; the app drives the light (light_ramp), tells the speaker
 * (LINK_RING) and starts the game (lib/game). Time is a wrapping uint32
 * millisecond counter.
 *
 * States
 *   ALARM_IDLE      nothing armed
 *   ALARM_ARMED     waiting for the sunrise to begin
 *   ALARM_SUNRISE   the light ramps up; it ends at the ring time
 *   ALARM_RINGING   the alarm sounds
 *   ALARM_SNOOZED   silent, the light stays on, rings again afterwards
 *
 * Arming
 *   alarm_ring_arm(in_ms) plans a ring in_ms from now and replaces a ring
 *   that was planned before (ARMED or SUNRISE). The sunrise begins
 *   cfg.sunrise_ms before the ring, or at once (shortened) if less time is
 *   left; with sunrise_ms 0 there is no SUNRISE state. Returns false while
 *   RINGING or SNOOZED: that alarm has to end first.
 *   alarm_ring_disarm() cancels a planned ring (ARMED: silently; SUNRISE:
 *   event STOP with ALARM_STOP_DISARMED). It does nothing while RINGING or
 *   SNOOZED, so flipping the "alarms armed" switch cannot silence an alarm.
 *
 * Events (taken once each, in order; at most ALARM_RING_EVENTS wait)
 *   ALARM_EV_SUNRISE  value = duration of the light ramp in ms
 *   ALARM_EV_RING     the alarm starts or comes back from a snooze;
 *                     value = 1 if the game is required, else 0
 *   ALARM_EV_SNOOZE   value = snooze time in ms
 *   ALARM_EV_STOP     value = alarm_stop_t
 *
 * Game
 *   If cfg.game is true, the alarm rings with the game required: the buttons
 *   do not stop or snooze it, only alarm_ring_game_won() does
 *   (ALARM_STOP_GAME). alarm_ring_game_unavailable() (gun or targets missing
 *   or flat) drops that requirement for this alarm and gives the buttons
 *   back; it may be called before or during the ring. The next alarm starts
 *   with cfg.game again.
 *   alarm_ring_game_won() has no effect unless the alarm is RINGING with the
 *   game required (a demo game never stops a normal alarm).
 *
 * Buttons (the big button on the bedside unit)
 *   short press   RINGING without game: SNOOZED for cfg.snooze_ms.
 *                 Otherwise ignored.
 *   long press    RINGING or SNOOZED without game: off (ALARM_STOP_BUTTON).
 *                 SUNRISE: this alarm is cancelled (ALARM_STOP_BUTTON), game
 *                 or not: you are awake already.
 *                 Otherwise ignored.
 *   Both return true if the press did something.
 *
 * Always
 *   alarm_ring_remote_stop()  the speaker reports its emergency button:
 *                             RINGING or SNOOZED end (ALARM_STOP_REMOTE),
 *                             game or not.
 *   Maximum ring time         cfg.ring_max_ms of uninterrupted ringing end
 *                             the alarm (ALARM_STOP_TIMEOUT), game or not.
 *
 * outputs.light_permille
 *   0 when IDLE or ARMED; rises linearly from 0 to 1000 during SUNRISE (a
 *   shortened sunrise also ends at 1000); 1000 while RINGING and SNOOZED.
 *   The app multiplies it with the sunrise_level setting.
 */
#ifndef ALARM_RING_H
#define ALARM_RING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_RING_EVENTS 6

typedef enum {
    ALARM_IDLE = 0,
    ALARM_ARMED,
    ALARM_SUNRISE,
    ALARM_RINGING,
    ALARM_SNOOZED,
} alarm_state_t;

typedef enum {
    ALARM_STOP_BUTTON = 1,
    ALARM_STOP_GAME,
    ALARM_STOP_REMOTE,
    ALARM_STOP_TIMEOUT,
    ALARM_STOP_DISARMED,
} alarm_stop_t;

typedef enum {
    ALARM_EV_SUNRISE = 1,
    ALARM_EV_RING,
    ALARM_EV_SNOOZE,
    ALARM_EV_STOP,
} alarm_event_id_t;

typedef struct {
    alarm_event_id_t id;
    uint32_t value;
} alarm_event_t;

typedef struct {
    uint32_t sunrise_ms;  /* 0 ... 3600000          default 1200000 (20 min) */
    uint32_t snooze_ms;   /* 60000 ... 1800000      default 540000 (9 min)   */
    uint32_t ring_max_ms; /* 60000 ... 3600000      default 900000 (15 min)  */
    bool game;            /* the game is required   default false            */
} alarm_ring_config_t;

typedef struct {
    alarm_state_t state;
    bool game_required;      /* for the current alarm */
    uint16_t light_permille; /* 0 ... 1000 */
} alarm_ring_outputs_t;

typedef struct alarm_ring alarm_ring_t;

void alarm_ring_config_default(alarm_ring_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is out of range. */
alarm_ring_t *alarm_ring_create(const alarm_ring_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void alarm_ring_destroy(alarm_ring_t *ar);

/* New settings; they apply from the next arm(). Returns false and changes
 * nothing if cfg is NULL or invalid. */
bool alarm_ring_set_config(alarm_ring_t *ar, const alarm_ring_config_t *cfg);

/* in_ms up to 2000000000 (about 23 days). */
bool alarm_ring_arm(alarm_ring_t *ar, uint32_t in_ms, uint32_t now_ms);
void alarm_ring_disarm(alarm_ring_t *ar, uint32_t now_ms);

bool alarm_ring_button_short(alarm_ring_t *ar, uint32_t now_ms);
bool alarm_ring_button_long(alarm_ring_t *ar, uint32_t now_ms);

void alarm_ring_game_won(alarm_ring_t *ar, uint32_t now_ms);
void alarm_ring_game_unavailable(alarm_ring_t *ar, uint32_t now_ms);
void alarm_ring_remote_stop(alarm_ring_t *ar, uint32_t now_ms);

/* Call regularly (e.g. every 20 ms). */
void alarm_ring_tick(alarm_ring_t *ar, uint32_t now_ms);

/* Current outputs. The pointer stays valid for the object's lifetime. */
const alarm_ring_outputs_t *alarm_ring_outputs(const alarm_ring_t *ar);

bool alarm_ring_take_event(alarm_ring_t *ar, alarm_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_RING_H */
