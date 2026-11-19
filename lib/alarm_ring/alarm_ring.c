/*
 * alarm_ring – one alarm from sunrise to "off" on the bedside node.
 * Contract in alarm_ring.h, tests in test/test_alarm_ring/.
 *
 * Planned times (sunrise start, ring, snooze end, ring timeout) are kept as
 * absolute wrapping milliseconds and compared with a signed difference, so a
 * late tick lands every change at its planned time.
 */
#include "alarm_ring.h"

#include <stdlib.h>

#define ARM_MAX_MS 2000000000u

struct alarm_ring {
    alarm_ring_config_t cfg;  /* applies from the next arm() */
    alarm_ring_config_t plan; /* the current alarm's copy */
    alarm_ring_outputs_t out;

    uint32_t sunrise_start;
    uint32_t sunrise_len;
    uint32_t ring_at;    /* planned ring, or end of the snooze */
    uint32_t ring_start; /* for ring_max_ms */

    alarm_event_t events[ALARM_RING_EVENTS];
    uint8_t ev_head;
    uint8_t ev_count;
};

static bool config_ok(const alarm_ring_config_t *c)
{
    return c != NULL && c->sunrise_ms <= 3600000u && c->snooze_ms >= 60000u && c->snooze_ms <= 1800000u &&
           c->ring_max_ms >= 60000u && c->ring_max_ms <= 3600000u;
}

void alarm_ring_config_default(alarm_ring_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->sunrise_ms = 1200000;
        cfg->snooze_ms = 540000;
        cfg->ring_max_ms = 900000;
        cfg->game = false;
    }
}

alarm_ring_t *alarm_ring_create(const alarm_ring_config_t *cfg, uint32_t now_ms)
{
    (void)now_ms;
    if (!config_ok(cfg)) {
        return NULL;
    }
    alarm_ring_t *ar = calloc(1, sizeof(*ar));
    if (ar != NULL) {
        ar->cfg = *cfg;
        ar->plan = *cfg;
        ar->out.state = ALARM_IDLE;
    }
    return ar;
}

void alarm_ring_destroy(alarm_ring_t *ar)
{
    free(ar);
}

bool alarm_ring_set_config(alarm_ring_t *ar, const alarm_ring_config_t *cfg)
{
    if (ar == NULL || !config_ok(cfg)) {
        return false;
    }
    ar->cfg = *cfg;
    return true;
}

static void push_event(alarm_ring_t *ar, alarm_event_id_t id, uint32_t value)
{
    if (ar->ev_count == ALARM_RING_EVENTS) {
        ar->ev_head = (uint8_t)((ar->ev_head + 1) % ALARM_RING_EVENTS);
        ar->ev_count--;
    }
    alarm_event_t *e = &ar->events[(ar->ev_head + ar->ev_count) % ALARM_RING_EVENTS];
    e->id = id;
    e->value = value;
    ar->ev_count++;
}

static bool reached(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t) >= 0;
}

static void go_idle(alarm_ring_t *ar)
{
    ar->out.state = ALARM_IDLE;
    ar->out.game_required = false;
    ar->out.light_permille = 0;
}

static void stop(alarm_ring_t *ar, alarm_stop_t why)
{
    go_idle(ar);
    push_event(ar, ALARM_EV_STOP, (uint32_t)why);
}

static void start_ringing(alarm_ring_t *ar, uint32_t at)
{
    ar->out.state = ALARM_RINGING;
    ar->ring_start = at;
    push_event(ar, ALARM_EV_RING, ar->out.game_required ? 1u : 0u);
}

static void update(alarm_ring_t *ar, uint32_t now)
{
    for (;;) {
        switch (ar->out.state) {
        case ALARM_ARMED:
            if (ar->sunrise_len > 0 && reached(now, ar->sunrise_start) && !reached(ar->sunrise_start, ar->ring_at)) {
                ar->out.state = ALARM_SUNRISE;
                push_event(ar, ALARM_EV_SUNRISE, ar->sunrise_len);
                continue;
            }
            if (reached(now, ar->ring_at)) {
                start_ringing(ar, ar->ring_at);
                continue;
            }
            break;
        case ALARM_SUNRISE:
            if (reached(now, ar->ring_at)) {
                start_ringing(ar, ar->ring_at);
                continue;
            }
            break;
        case ALARM_RINGING:
            if ((uint32_t)(now - ar->ring_start) >= ar->plan.ring_max_ms) {
                stop(ar, ALARM_STOP_TIMEOUT);
                continue;
            }
            break;
        case ALARM_SNOOZED:
            if (reached(now, ar->ring_at)) {
                start_ringing(ar, ar->ring_at);
                continue;
            }
            break;
        default:
            break;
        }
        break;
    }

    switch (ar->out.state) {
    case ALARM_SUNRISE: {
        const uint32_t elapsed = now - ar->sunrise_start;
        const uint64_t level = (uint64_t)elapsed * 1000u / ar->sunrise_len;
        ar->out.light_permille = (uint16_t)(level > 1000u ? 1000u : level);
        break;
    }
    case ALARM_RINGING:
    case ALARM_SNOOZED:
        ar->out.light_permille = 1000;
        break;
    default:
        ar->out.light_permille = 0;
        break;
    }
}

bool alarm_ring_arm(alarm_ring_t *ar, uint32_t in_ms, uint32_t now_ms)
{
    if (ar == NULL || in_ms > ARM_MAX_MS) {
        return false;
    }
    update(ar, now_ms);
    if (ar->out.state == ALARM_RINGING || ar->out.state == ALARM_SNOOZED) {
        return false;
    }
    ar->plan = ar->cfg;
    ar->out.game_required = ar->plan.game;
    ar->ring_at = now_ms + in_ms;
    ar->sunrise_len = in_ms < ar->plan.sunrise_ms ? in_ms : ar->plan.sunrise_ms;
    ar->sunrise_start = ar->ring_at - ar->sunrise_len;
    ar->out.state = ALARM_ARMED;
    update(ar, now_ms);
    return true;
}

void alarm_ring_disarm(alarm_ring_t *ar, uint32_t now_ms)
{
    if (ar == NULL) {
        return;
    }
    update(ar, now_ms);
    if (ar->out.state == ALARM_ARMED) {
        go_idle(ar);
    } else if (ar->out.state == ALARM_SUNRISE) {
        stop(ar, ALARM_STOP_DISARMED);
    }
}

bool alarm_ring_button_short(alarm_ring_t *ar, uint32_t now_ms)
{
    if (ar == NULL) {
        return false;
    }
    update(ar, now_ms);
    if (ar->out.state != ALARM_RINGING || ar->out.game_required) {
        return false;
    }
    ar->out.state = ALARM_SNOOZED;
    ar->ring_at = now_ms + ar->plan.snooze_ms;
    push_event(ar, ALARM_EV_SNOOZE, ar->plan.snooze_ms);
    update(ar, now_ms);
    return true;
}

bool alarm_ring_button_long(alarm_ring_t *ar, uint32_t now_ms)
{
    if (ar == NULL) {
        return false;
    }
    update(ar, now_ms);
    switch (ar->out.state) {
    case ALARM_SUNRISE:
        stop(ar, ALARM_STOP_BUTTON);
        return true;
    case ALARM_RINGING:
    case ALARM_SNOOZED:
        if (ar->out.game_required) {
            return false;
        }
        stop(ar, ALARM_STOP_BUTTON);
        return true;
    default:
        return false;
    }
}

void alarm_ring_game_won(alarm_ring_t *ar, uint32_t now_ms)
{
    if (ar == NULL) {
        return;
    }
    update(ar, now_ms);
    if (ar->out.state == ALARM_RINGING && ar->out.game_required) {
        stop(ar, ALARM_STOP_GAME);
    }
}

void alarm_ring_game_unavailable(alarm_ring_t *ar, uint32_t now_ms)
{
    if (ar == NULL) {
        return;
    }
    update(ar, now_ms);
    ar->out.game_required = false;
}

void alarm_ring_remote_stop(alarm_ring_t *ar, uint32_t now_ms)
{
    if (ar == NULL) {
        return;
    }
    update(ar, now_ms);
    if (ar->out.state == ALARM_RINGING || ar->out.state == ALARM_SNOOZED) {
        stop(ar, ALARM_STOP_REMOTE);
    }
}

void alarm_ring_tick(alarm_ring_t *ar, uint32_t now_ms)
{
    if (ar != NULL) {
        update(ar, now_ms);
    }
}

const alarm_ring_outputs_t *alarm_ring_outputs(const alarm_ring_t *ar)
{
    return ar ? &ar->out : NULL;
}

bool alarm_ring_take_event(alarm_ring_t *ar, alarm_event_t *out)
{
    if (ar == NULL || out == NULL || ar->ev_count == 0) {
        return false;
    }
    *out = ar->events[ar->ev_head];
    ar->ev_head = (uint8_t)((ar->ev_head + 1) % ALARM_RING_EVENTS);
    ar->ev_count--;
    return true;
}
