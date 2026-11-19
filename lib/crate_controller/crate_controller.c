/*
 * crate_controller – lid open = light and choir, lid closed = fade and sleep.
 * Contract in crate_controller.h, tests in test/test_crate_controller/.
 *
 * Timed things (debounced lid change, end of a fade, the max-open timer)
 * each have a planned time. update() handles them in time order at their
 * planned times, so a late call ends up where regular polling would have.
 */
#include "crate_controller.h"

#include <stdlib.h>
#include <string.h>

#define HYSTERESIS_MV 300u

typedef struct {
    bool active;
    uint32_t since;
} run_t;

struct crate_controller {
    crate_config_t cfg;
    crate_outputs_t out;

    /* lid */
    bool raw;
    uint32_t raw_since;

    /* timers */
    bool fading;          /* CLOSING, or DIM before the music stopped */
    uint32_t fade_start;
    uint32_t open_since;  /* for max_open_ms */
    uint32_t closed_since;

    /* battery */
    run_t run_empty;   /* mv < cutoff            */
    run_t run_low;     /* mv < warn              */
    run_t run_up;      /* mv >= cutoff + 300     */
    run_t run_ok;      /* mv >= warn + 300       */

    crate_event_t events[CRATE_EVENTS];
    uint8_t ev_head;
    uint8_t ev_count;
};

void crate_config_default(crate_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->debounce_ms = 50;
    cfg->open_level = 600;
    cfg->dim_level = 150;
    cfg->open_fade_ms = 1500;
    cfg->close_fade_ms = 3000;
    cfg->volume = 500;
    cfg->max_open_ms = 900000;
    cfg->cutoff_mv = 12400;
    cfg->warn_mv = 12800;
    cfg->battery_hold_ms = 10000;
    cfg->sleep_delay_ms = 2000;
}

static bool config_ok(const crate_config_t *c)
{
    return c->debounce_ms >= 10 && c->debounce_ms <= 500 && c->open_level >= 1 && c->open_level <= 1000 &&
           c->dim_level <= c->open_level && c->open_fade_ms <= 10000 && c->close_fade_ms <= 20000 &&
           c->volume >= 1 && c->volume <= 1000 &&
           (c->max_open_ms == 0 || (c->max_open_ms >= 60000 && c->max_open_ms <= 3600000u)) &&
           c->cutoff_mv >= 10000 && c->cutoff_mv <= 13500 && c->warn_mv >= c->cutoff_mv && c->warn_mv <= 14000 &&
           c->battery_hold_ms >= 1000 && c->battery_hold_ms <= 60000 && c->sleep_delay_ms <= 60000;
}

static void push_event(crate_controller_t *cc, crate_event_t e)
{
    if (cc->ev_count == CRATE_EVENTS) {
        cc->ev_head = (uint8_t)((cc->ev_head + 1) % CRATE_EVENTS);
        cc->ev_count--;
    }
    cc->events[(cc->ev_head + cc->ev_count) % CRATE_EVENTS] = e;
    cc->ev_count++;
}

static void set_targets(crate_controller_t *cc, uint16_t light, uint16_t volume, uint16_t fade_ms)
{
    cc->out.light_permille = light;
    cc->out.light_fade_ms = fade_ms;
    cc->out.volume_permille = volume;
    cc->out.volume_fade_ms = fade_ms;
}

static void stop_music(crate_controller_t *cc)
{
    if (cc->out.music_playing) {
        cc->out.music_playing = false;
        push_event(cc, CRATE_EV_MUSIC_STOP);
    }
    cc->out.amp_on = false;
}

static void fade_over(crate_controller_t *cc, uint32_t t)
{
    cc->fading = false;
    stop_music(cc);
    if (cc->out.state == CRATE_CLOSING) {
        cc->out.state = CRATE_CLOSED;
        cc->closed_since = t;
    }
}

static void start_close(crate_controller_t *cc, uint32_t t)
{
    cc->out.state = CRATE_CLOSING;
    set_targets(cc, 0, 0, cc->cfg.close_fade_ms);
    cc->fading = true;
    cc->fade_start = t;
    if (cc->cfg.close_fade_ms == 0) {
        fade_over(cc, t);
    }
}

static void lid_opened(crate_controller_t *cc, uint32_t t)
{
    if (cc->out.battery == CRATE_BATT_EMPTY) {
        push_event(cc, CRATE_EV_BLINK);
        return;
    }
    if (cc->out.state != CRATE_CLOSED && cc->out.state != CRATE_CLOSING) {
        return;
    }
    cc->out.state = CRATE_OPEN;
    set_targets(cc, cc->cfg.open_level, cc->cfg.volume, cc->cfg.open_fade_ms);
    cc->out.amp_on = true;
    cc->fading = false;
    cc->open_since = t;
    if (!cc->out.music_playing) {
        cc->out.music_playing = true;
        push_event(cc, CRATE_EV_MUSIC_START);
    }
}

static void lid_closed(crate_controller_t *cc, uint32_t t)
{
    if (cc->out.state == CRATE_OPEN || cc->out.state == CRATE_DIM) {
        start_close(cc, t);
    }
}

static void dim(crate_controller_t *cc, uint32_t t)
{
    cc->out.state = CRATE_DIM;
    set_targets(cc, cc->cfg.dim_level, 0, cc->cfg.close_fade_ms);
    cc->fading = true;
    cc->fade_start = t;
    if (cc->cfg.close_fade_ms == 0) {
        fade_over(cc, t);
    }
}

/* How long ago t was (negative: still in the future). */
static int32_t ago(uint32_t now, uint32_t t)
{
    return (int32_t)(now - t);
}

static void update(crate_controller_t *cc, uint32_t now)
{
    for (;;) {
        /* collect the timed things that are due, pick the earliest */
        int which = -1;
        uint32_t when = 0;
        int32_t best = -1;

        if (cc->raw != cc->out.lid_open) {
            const uint32_t t = cc->raw_since + cc->cfg.debounce_ms;
            if (ago(now, t) >= 0 && ago(now, t) > best) {
                which = 0;
                when = t;
                best = ago(now, t);
            }
        }
        if (cc->fading) {
            const uint32_t t = cc->fade_start + cc->cfg.close_fade_ms;
            if (ago(now, t) >= 0 && ago(now, t) > best) {
                which = 1;
                when = t;
                best = ago(now, t);
            }
        }
        if (cc->out.state == CRATE_OPEN && cc->cfg.max_open_ms > 0) {
            const uint32_t t = cc->open_since + cc->cfg.max_open_ms;
            if (ago(now, t) >= 0 && ago(now, t) > best) {
                which = 2;
                when = t;
                best = ago(now, t);
            }
        }
        if (which < 0) {
            break;
        }
        switch (which) {
        case 0:
            cc->out.lid_open = cc->raw;
            if (cc->raw) {
                lid_opened(cc, when);
            } else {
                lid_closed(cc, when);
            }
            break;
        case 1:
            fade_over(cc, when);
            break;
        default:
            dim(cc, when);
            break;
        }
    }
    cc->out.sleep_ok = cc->out.state == CRATE_CLOSED &&
                       (uint32_t)(now - cc->closed_since) >= cc->cfg.sleep_delay_ms &&
                       (!cc->out.lid_open || cc->out.battery == CRATE_BATT_EMPTY);
}

crate_controller_t *crate_controller_create(const crate_config_t *cfg, bool lid_open, uint32_t now_ms)
{
    if (cfg == NULL || !config_ok(cfg)) {
        return NULL;
    }
    crate_controller_t *cc = calloc(1, sizeof(*cc));
    if (cc == NULL) {
        return NULL;
    }
    cc->cfg = *cfg;
    cc->out.state = CRATE_CLOSED;
    cc->out.battery = CRATE_BATT_OK;
    cc->closed_since = now_ms;
    cc->raw = lid_open;
    cc->raw_since = now_ms;
    cc->out.lid_open = lid_open;
    if (lid_open) {
        lid_opened(cc, now_ms);
    }
    update(cc, now_ms);
    return cc;
}

void crate_controller_destroy(crate_controller_t *cc)
{
    free(cc);
}

void crate_controller_lid(crate_controller_t *cc, bool open_raw, uint32_t now_ms)
{
    if (cc == NULL) {
        return;
    }
    update(cc, now_ms);
    if (open_raw != cc->raw) {
        cc->raw = open_raw;
        cc->raw_since = now_ms;
    }
    update(cc, now_ms);
}

static void track(run_t *r, bool condition, uint32_t now)
{
    if (!condition) {
        r->active = false;
    } else if (!r->active) {
        r->active = true;
        r->since = now;
    }
}

static bool held(const crate_controller_t *cc, const run_t *r, uint32_t now)
{
    return r->active && (uint32_t)(now - r->since) >= cc->cfg.battery_hold_ms;
}

void crate_controller_battery(crate_controller_t *cc, uint16_t battery_mv, uint32_t now_ms)
{
    if (cc == NULL) {
        return;
    }
    update(cc, now_ms);
    const crate_config_t *c = &cc->cfg;
    track(&cc->run_empty, battery_mv < c->cutoff_mv, now_ms);
    track(&cc->run_low, battery_mv < c->warn_mv, now_ms);
    track(&cc->run_up, battery_mv >= (uint32_t)c->cutoff_mv + HYSTERESIS_MV, now_ms);
    track(&cc->run_ok, battery_mv >= (uint32_t)c->warn_mv + HYSTERESIS_MV, now_ms);

    const crate_battery_t before = cc->out.battery;
    switch (before) {
    case CRATE_BATT_OK:
        if (held(cc, &cc->run_empty, now_ms)) {
            cc->out.battery = CRATE_BATT_EMPTY;
        } else if (held(cc, &cc->run_low, now_ms)) {
            cc->out.battery = CRATE_BATT_LOW;
        }
        break;
    case CRATE_BATT_LOW:
        if (held(cc, &cc->run_empty, now_ms)) {
            cc->out.battery = CRATE_BATT_EMPTY;
        } else if (held(cc, &cc->run_ok, now_ms)) {
            cc->out.battery = CRATE_BATT_OK;
        }
        break;
    default: /* EMPTY */
        if (held(cc, &cc->run_up, now_ms)) {
            cc->out.battery = held(cc, &cc->run_ok, now_ms) ? CRATE_BATT_OK : CRATE_BATT_LOW;
        }
        break;
    }
    if (cc->out.battery == CRATE_BATT_EMPTY && before != CRATE_BATT_EMPTY &&
        (cc->out.state == CRATE_OPEN || cc->out.state == CRATE_DIM)) {
        push_event(cc, CRATE_EV_BLINK);
        start_close(cc, now_ms);
    }
    update(cc, now_ms);
}

void crate_controller_tick(crate_controller_t *cc, uint32_t now_ms)
{
    if (cc != NULL) {
        update(cc, now_ms);
    }
}

const crate_outputs_t *crate_controller_outputs(const crate_controller_t *cc)
{
    return cc ? &cc->out : NULL;
}

bool crate_controller_take_event(crate_controller_t *cc, crate_event_t *out)
{
    if (cc == NULL || out == NULL || cc->ev_count == 0) {
        return false;
    }
    *out = cc->events[cc->ev_head];
    cc->ev_head = (uint8_t)((cc->ev_head + 1) % CRATE_EVENTS);
    cc->ev_count--;
    return true;
}
