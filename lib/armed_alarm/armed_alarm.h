/*
 * armed_alarm – the speaker node's own copy of the next alarm, so it rings
 * even if the bedside node or the link is gone (pure C).
 *
 * Requirements: BED-10 ... BED-16 (see the requirements doc).
 * Tests:        test/test_armed_alarm/
 *
 * The bedside node owns the clock and the schedule. With every change and
 * every heartbeat it sends LINK_TIME_ALARM: its current time, the next alarm
 * and a version. The speaker keeps that copy here, counts the time forward on
 * its own millisecond clock and starts ringing by itself when the alarm time
 * comes. Time is a wrapping uint32 millisecond counter; UTC values are
 * seconds since 1970.
 *
 * Copy
 *   armed_alarm_update() takes a copy if its version is not older than the
 *   one held (an older one returns false and changes nothing). The speaker's
 *   idea of the time is then copy.utc_now_s at this now_ms, counted on from
 *   there. An alarm_utc_s of 0, or one that is not later than utc_now_s,
 *   means "nothing armed".
 *   While the alarm rings or snoozes, an update still corrects the clock and
 *   is kept, but only takes effect as the armed alarm once the ring is over.
 *
 * States
 *   ARMED_IDLE      nothing armed
 *   ARMED_WAITING   an alarm is armed and lies in the future
 *   ARMED_RINGING   the alarm sound plays
 *   ARMED_SNOOZED   silent, rings again when the snooze is over
 *
 * Ringing starts
 *   - by itself when the counted time reaches alarm_utc_s (WAITING), or
 *   - with armed_alarm_ring(LINK_RING_START) from the bedside (IDLE or
 *     WAITING; ignored while already ringing or snoozing).
 *   Event ARMED_EV_RING with the copy's sound, volume, fade and game flag
 *   (without any copy: sound 0, volume 16, fade 0, 15 minutes, no game).
 *
 * Ringing ends
 *   - armed_alarm_ring(LINK_RING_STOP): always, whatever the version says
 *     (ARMED_STOP_REMOTE);
 *   - armed_alarm_local_stop(): the emergency button on the speaker
 *     (ARMED_STOP_LOCAL);
 *   - after ring_max_min minutes of uninterrupted ringing
 *     (ARMED_STOP_TIMEOUT); a ring_max_min of 0 in the copy counts as 15.
 *   Each gives event ARMED_EV_STOP with the reason. Afterwards the newest
 *   copy decides between WAITING and IDLE; the alarm that just rang is in
 *   the past and never rings again.
 *
 * Snooze
 *   armed_alarm_ring(LINK_RING_SNOOZE, minutes) while RINGING: event
 *   ARMED_EV_SNOOZE, silent for that many minutes (1 ... 60, others are
 *   refused), then RINGING again with a new ARMED_EV_RING and a fresh
 *   ring_max_min. A stop works during the snooze too.
 *
 * armed_alarm_ring() returns true if the command changed something.
 * LINK_RING_ESCALATE is not handled here (it only changes the volume).
 * Events are taken once each, in order (at most ARMED_EVENTS wait; the
 * oldest is dropped).
 */
#ifndef ARMED_ALARM_H
#define ARMED_ALARM_H

#include <stdbool.h>
#include <stdint.h>

#include "link_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARMED_EVENTS 4

typedef enum {
    ARMED_IDLE = 0,
    ARMED_WAITING,
    ARMED_RINGING,
    ARMED_SNOOZED,
} armed_state_t;

typedef enum {
    ARMED_STOP_NONE = 0,
    ARMED_STOP_REMOTE,
    ARMED_STOP_LOCAL,
    ARMED_STOP_TIMEOUT,
} armed_stop_t;

typedef enum {
    ARMED_EV_RING = 1,
    ARMED_EV_SNOOZE,
    ARMED_EV_STOP,
} armed_event_id_t;

typedef struct {
    armed_event_id_t id;
    armed_stop_t reason; /* ARMED_EV_STOP                 */
    uint8_t sound;       /* ARMED_EV_RING, from the copy  */
    uint8_t volume;
    uint16_t fade_s;
    bool game;
} armed_event_t;

typedef struct armed_alarm armed_alarm_t;

/* Returns NULL only when memory runs out. */
armed_alarm_t *armed_alarm_create(uint32_t now_ms);

/* Accepts NULL. */
void armed_alarm_destroy(armed_alarm_t *aa);

bool armed_alarm_update(armed_alarm_t *aa, const link_time_alarm_t *copy, uint32_t now_ms);

bool armed_alarm_ring(armed_alarm_t *aa, link_ring_action_t action, uint16_t arg, uint32_t now_ms);

/* Returns true if an alarm was ringing or snoozing. */
bool armed_alarm_local_stop(armed_alarm_t *aa, uint32_t now_ms);

/* Call regularly (e.g. every 100 ms). */
void armed_alarm_tick(armed_alarm_t *aa, uint32_t now_ms);

armed_state_t armed_alarm_state(const armed_alarm_t *aa);

/* Version of the copy held; 0 if none was received yet. */
uint32_t armed_alarm_version(const armed_alarm_t *aa);

/* The speaker's idea of the UTC time, or 0 before the first copy. */
uint32_t armed_alarm_utc(const armed_alarm_t *aa, uint32_t now_ms);

/* Seconds the current uninterrupted ring has lasted (0 unless RINGING). */
uint16_t armed_alarm_ring_seconds(const armed_alarm_t *aa, uint32_t now_ms);

bool armed_alarm_take_event(armed_alarm_t *aa, armed_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ARMED_ALARM_H */
