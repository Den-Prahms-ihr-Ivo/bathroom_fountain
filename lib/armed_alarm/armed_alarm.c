/*
 * armed_alarm – the speaker's own copy of the next alarm.
 * Contract in armed_alarm.h, tests in test/test_armed_alarm/.
 *
 * The clock is "copy.utc_now_s at base_ms", counted on with the local
 * millisecond counter. The alarm rings when the counted time reaches
 * alarm_utc_s, to the millisecond.
 */
#include "armed_alarm.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_VOLUME 16
#define DEFAULT_RING_MAX_MIN 15

typedef struct {
    uint8_t sound;
    uint8_t volume;
    uint16_t fade_s;
    bool game;
    uint32_t ring_max_ms;
} ring_params_t;

struct armed_alarm {
    bool have_copy;
    link_time_alarm_t copy;
    uint32_t base_ms; /* local time at which copy.utc_now_s was true */

    armed_state_t state;
    uint32_t rung_alarm; /* alarm_utc_s of the alarm that rang last */
    ring_params_t ring;
    uint32_t ring_start;
    uint32_t snooze_end;

    armed_event_t events[ARMED_EVENTS];
    uint8_t ev_head;
    uint8_t ev_count;
};

armed_alarm_t *armed_alarm_create(uint32_t now_ms)
{
    (void)now_ms;
    armed_alarm_t *aa = calloc(1, sizeof(*aa));
    if (aa != NULL) {
        aa->state = ARMED_IDLE;
    }
    return aa;
}

void armed_alarm_destroy(armed_alarm_t *aa)
{
    free(aa);
}

static void push_event(armed_alarm_t *aa, const armed_event_t *e)
{
    if (aa->ev_count == ARMED_EVENTS) {
        aa->ev_head = (uint8_t)((aa->ev_head + 1) % ARMED_EVENTS);
        aa->ev_count--;
    }
    aa->events[(aa->ev_head + aa->ev_count) % ARMED_EVENTS] = *e;
    aa->ev_count++;
}

/* Milliseconds from now until the armed alarm; negative once it is due. */
static int64_t ms_until_alarm(const armed_alarm_t *aa, uint32_t now_ms)
{
    const int64_t elapsed = (int64_t)(uint32_t)(now_ms - aa->base_ms);
    const int64_t until = ((int64_t)aa->copy.alarm_utc_s - (int64_t)aa->copy.utc_now_s) * 1000;
    return until - elapsed;
}

/* IDLE or WAITING, from the newest copy. */
static void settle(armed_alarm_t *aa, uint32_t now_ms)
{
    if (aa->state != ARMED_IDLE && aa->state != ARMED_WAITING) {
        return;
    }
    const bool armed = aa->have_copy && aa->copy.alarm_utc_s != 0 && aa->copy.alarm_utc_s > aa->copy.utc_now_s &&
                       aa->copy.alarm_utc_s != aa->rung_alarm && ms_until_alarm(aa, now_ms) > 0;
    aa->state = armed ? ARMED_WAITING : ARMED_IDLE;
}

static void start_ring(armed_alarm_t *aa, uint32_t at)
{
    ring_params_t p = {0, DEFAULT_VOLUME, 0, false, DEFAULT_RING_MAX_MIN * 60000u};
    if (aa->have_copy) {
        p.sound = aa->copy.sound;
        p.volume = aa->copy.volume;
        p.fade_s = aa->copy.fade_s;
        p.game = aa->copy.game;
        const uint32_t max_min = aa->copy.ring_max_min ? aa->copy.ring_max_min : DEFAULT_RING_MAX_MIN;
        p.ring_max_ms = max_min * 60000u;
        aa->rung_alarm = aa->copy.alarm_utc_s;
    }
    aa->ring = p;
    aa->state = ARMED_RINGING;
    aa->ring_start = at;
    armed_event_t e;
    memset(&e, 0, sizeof(e));
    e.id = ARMED_EV_RING;
    e.sound = p.sound;
    e.volume = p.volume;
    e.fade_s = p.fade_s;
    e.game = p.game;
    push_event(aa, &e);
}

static void ring_again(armed_alarm_t *aa, uint32_t at)
{
    aa->state = ARMED_RINGING;
    aa->ring_start = at;
    armed_event_t e;
    memset(&e, 0, sizeof(e));
    e.id = ARMED_EV_RING;
    e.sound = aa->ring.sound;
    e.volume = aa->ring.volume;
    e.fade_s = aa->ring.fade_s;
    e.game = aa->ring.game;
    push_event(aa, &e);
}

static void stop(armed_alarm_t *aa, armed_stop_t reason, uint32_t now_ms)
{
    aa->state = ARMED_IDLE;
    armed_event_t e;
    memset(&e, 0, sizeof(e));
    e.id = ARMED_EV_STOP;
    e.reason = reason;
    push_event(aa, &e);
    settle(aa, now_ms);
}

static void update(armed_alarm_t *aa, uint32_t now_ms)
{
    switch (aa->state) {
    case ARMED_WAITING:
        if (ms_until_alarm(aa, now_ms) <= 0) {
            start_ring(aa, now_ms);
        }
        break;
    case ARMED_RINGING:
        if ((uint32_t)(now_ms - aa->ring_start) >= aa->ring.ring_max_ms) {
            stop(aa, ARMED_STOP_TIMEOUT, now_ms);
        }
        break;
    case ARMED_SNOOZED:
        if ((int32_t)(now_ms - aa->snooze_end) >= 0) {
            ring_again(aa, aa->snooze_end);
        }
        break;
    default:
        break;
    }
}

bool armed_alarm_update(armed_alarm_t *aa, const link_time_alarm_t *copy, uint32_t now_ms)
{
    if (aa == NULL || copy == NULL) {
        return false;
    }
    if (aa->have_copy && copy->version < aa->copy.version) {
        return false;
    }
    update(aa, now_ms);
    aa->copy = *copy;
    aa->have_copy = true;
    aa->base_ms = now_ms;
    settle(aa, now_ms);
    update(aa, now_ms);
    return true;
}

bool armed_alarm_ring(armed_alarm_t *aa, link_ring_action_t action, uint16_t arg, uint32_t now_ms)
{
    if (aa == NULL) {
        return false;
    }
    update(aa, now_ms);
    switch (action) {
    case LINK_RING_START:
        if (aa->state == ARMED_RINGING || aa->state == ARMED_SNOOZED) {
            return false;
        }
        start_ring(aa, now_ms);
        return true;
    case LINK_RING_STOP:
        if (aa->state != ARMED_RINGING && aa->state != ARMED_SNOOZED) {
            return false;
        }
        stop(aa, ARMED_STOP_REMOTE, now_ms);
        return true;
    case LINK_RING_SNOOZE: {
        if (aa->state != ARMED_RINGING || arg < 1 || arg > 60) {
            return false;
        }
        aa->state = ARMED_SNOOZED;
        aa->snooze_end = now_ms + (uint32_t)arg * 60000u;
        armed_event_t e;
        memset(&e, 0, sizeof(e));
        e.id = ARMED_EV_SNOOZE;
        push_event(aa, &e);
        return true;
    }
    default:
        return false;
    }
}

bool armed_alarm_local_stop(armed_alarm_t *aa, uint32_t now_ms)
{
    if (aa == NULL) {
        return false;
    }
    update(aa, now_ms);
    if (aa->state != ARMED_RINGING && aa->state != ARMED_SNOOZED) {
        return false;
    }
    stop(aa, ARMED_STOP_LOCAL, now_ms);
    return true;
}

void armed_alarm_tick(armed_alarm_t *aa, uint32_t now_ms)
{
    if (aa != NULL) {
        update(aa, now_ms);
    }
}

armed_state_t armed_alarm_state(const armed_alarm_t *aa)
{
    return aa ? aa->state : ARMED_IDLE;
}

uint32_t armed_alarm_version(const armed_alarm_t *aa)
{
    return (aa != NULL && aa->have_copy) ? aa->copy.version : 0;
}

uint32_t armed_alarm_utc(const armed_alarm_t *aa, uint32_t now_ms)
{
    if (aa == NULL || !aa->have_copy) {
        return 0;
    }
    return aa->copy.utc_now_s + (uint32_t)(now_ms - aa->base_ms) / 1000u;
}

uint16_t armed_alarm_ring_seconds(const armed_alarm_t *aa, uint32_t now_ms)
{
    if (aa == NULL || aa->state != ARMED_RINGING) {
        return 0;
    }
    const uint32_t s = (uint32_t)(now_ms - aa->ring_start) / 1000u;
    return (uint16_t)(s > 0xFFFFu ? 0xFFFFu : s);
}

bool armed_alarm_take_event(armed_alarm_t *aa, armed_event_t *out)
{
    if (aa == NULL || out == NULL || aa->ev_count == 0) {
        return false;
    }
    *out = aa->events[aa->ev_head];
    aa->ev_head = (uint8_t)((aa->ev_head + 1) % ARMED_EVENTS);
    aa->ev_count--;
    return true;
}
