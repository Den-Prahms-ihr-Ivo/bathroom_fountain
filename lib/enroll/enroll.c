/*
 * enroll – the device side of pairing.
 * Contract in enroll.h, tests in test/test_enroll/.
 */
#include "enroll.h"

#include <stdlib.h>
#include <string.h>

struct enroll {
    enroll_config_t cfg;
    enroll_outputs_t out;
    enroll_creds_t creds;

    uint32_t next_ms; /* next HELLO / CONFIRM (not used when CLAIMED) */
    uint32_t win_start_ms;
    uint32_t win_ms;
    uint8_t auth_fails;

    enroll_act_t acts[ENROLL_ACTS];
    size_t act_head;
    size_t act_count;
};

void enroll_config_default(enroll_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->hello_ms = 10000;
    cfg->poll_ms = 2000;
    cfg->auth_fail_limit = 3;
}

static bool config_ok(const enroll_config_t *c)
{
    return c->hello_ms >= 1000 && c->hello_ms <= 600000 && c->poll_ms >= 500 && c->poll_ms <= 60000
           && c->auth_fail_limit >= 1 && c->auth_fail_limit <= 20;
}

enroll_t *enroll_create(const enroll_config_t *cfg, bool claimed, uint32_t now_ms)
{
    if (cfg == NULL || !config_ok(cfg)) {
        return NULL;
    }
    enroll_t *e = calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->cfg = *cfg;
    e->out.state = claimed ? ENROLL_CLAIMED : ENROLL_UNCLAIMED;
    e->next_ms = now_ms; /* first HELLO due at once */
    return e;
}

void enroll_destroy(enroll_t *e)
{
    free(e);
}

/* ---- helpers ------------------------------------------------------------- */

static void push(enroll_t *e, enroll_act_t a)
{
    if (e->act_count == ENROLL_ACTS) { /* drop the oldest */
        e->act_head = (e->act_head + 1) % ENROLL_ACTS;
        e->act_count--;
    }
    e->acts[(e->act_head + e->act_count) % ENROLL_ACTS] = a;
    e->act_count++;
}

static bool due(uint32_t now_ms, uint32_t at_ms)
{
    return (int32_t)(now_ms - at_ms) >= 0;
}

static void set_state(enroll_t *e, enroll_state_t s)
{
    if (s != ENROLL_UNCLAIMED) {
        e->out.rejected = false;
    }
    e->out.state = s;
    e->out.blink = (s == ENROLL_CONFIRM || s == ENROLL_CONFIRMING);
}

/* -> UNCLAIMED with the next HELLO hello_ms from now. */
static void to_unclaimed_later(enroll_t *e, uint32_t now_ms)
{
    set_state(e, ENROLL_UNCLAIMED);
    e->next_ms = now_ms + e->cfg.hello_ms;
}

/* -> UNCLAIMED with a HELLO at once. */
static void to_unclaimed_now(enroll_t *e, uint32_t now_ms)
{
    set_state(e, ENROLL_UNCLAIMED);
    push(e, ENROLL_ACT_HELLO);
    e->next_ms = now_ms + e->cfg.hello_ms;
}

static void open_window(enroll_t *e, uint16_t window_s, uint32_t now_ms)
{
    e->win_start_ms = now_ms;
    e->win_ms = (uint32_t)window_s * 1000u;
}

static void catch_up(enroll_t *e, uint32_t now_ms)
{
    switch (e->out.state) {
    case ENROLL_UNCLAIMED:
        if (due(now_ms, e->next_ms)) {
            push(e, ENROLL_ACT_HELLO);
            e->next_ms = now_ms + e->cfg.hello_ms;
        }
        break;
    case ENROLL_CONFIRM:
    case ENROLL_CONFIRMING:
        if (now_ms - e->win_start_ms >= e->win_ms) {
            to_unclaimed_now(e, now_ms);
        } else if (due(now_ms, e->next_ms)) {
            push(e, e->out.state == ENROLL_CONFIRM ? ENROLL_ACT_HELLO : ENROLL_ACT_CONFIRM);
            e->next_ms = now_ms + e->cfg.poll_ms;
        }
        break;
    case ENROLL_CLAIMED:
    default:
        break;
    }
}

/* ---- API ----------------------------------------------------------------- */

bool enroll_reply(enroll_t *e, const enroll_reply_t *reply, uint32_t now_ms)
{
    if (e == NULL || reply == NULL) {
        return false;
    }
    catch_up(e, now_ms);

    switch (e->out.state) {
    case ENROLL_UNCLAIMED:
        switch (reply->kind) {
        case ENROLL_REPLY_CONFIRM:
            set_state(e, ENROLL_CONFIRM);
            open_window(e, reply->window_s, now_ms);
            e->next_ms = now_ms + e->cfg.poll_ms;
            return true;
        case ENROLL_REPLY_REJECTED:
            e->out.rejected = true;
            return true;
        default: /* WAIT; CLAIMED only counts after a confirm */
            return false;
        }

    case ENROLL_CONFIRM:
        switch (reply->kind) {
        case ENROLL_REPLY_WAIT: /* the user cancelled */
            to_unclaimed_later(e, now_ms);
            return true;
        case ENROLL_REPLY_CONFIRM:
            open_window(e, reply->window_s, now_ms);
            return true;
        case ENROLL_REPLY_REJECTED:
            to_unclaimed_later(e, now_ms);
            e->out.rejected = true;
            return true;
        default: /* CLAIMED: the hold is required */
            return false;
        }

    case ENROLL_CONFIRMING:
        switch (reply->kind) {
        case ENROLL_REPLY_CLAIMED:
            e->creds = reply->creds;
            e->auth_fails = 0;
            set_state(e, ENROLL_CLAIMED);
            push(e, ENROLL_ACT_SAVE);
            return true;
        case ENROLL_REPLY_WAIT:
            to_unclaimed_later(e, now_ms);
            return true;
        case ENROLL_REPLY_REJECTED:
            to_unclaimed_later(e, now_ms);
            e->out.rejected = true;
            return true;
        default: /* CONFIRM: keep sending */
            return false;
        }

    case ENROLL_CLAIMED:
    default:
        return false;
    }
}

bool enroll_button_held(enroll_t *e, uint32_t now_ms)
{
    if (e == NULL) {
        return false;
    }
    catch_up(e, now_ms);
    if (e->out.state == ENROLL_CONFIRM) {
        set_state(e, ENROLL_CONFIRMING);
        push(e, ENROLL_ACT_CONFIRM);
        e->next_ms = now_ms + e->cfg.poll_ms;
        return true;
    }
    return e->out.state == ENROLL_CONFIRMING;
}

void enroll_tick(enroll_t *e, uint32_t now_ms)
{
    if (e != NULL) {
        catch_up(e, now_ms);
    }
}

static void clear_creds(enroll_t *e, uint32_t now_ms)
{
    e->auth_fails = 0;
    push(e, ENROLL_ACT_CLEAR);
    to_unclaimed_now(e, now_ms);
}

void enroll_auth_result(enroll_t *e, bool ok, uint32_t now_ms)
{
    if (e == NULL) {
        return;
    }
    catch_up(e, now_ms);
    if (e->out.state != ENROLL_CLAIMED) {
        return;
    }
    if (ok) {
        e->auth_fails = 0;
        return;
    }
    if (++e->auth_fails >= e->cfg.auth_fail_limit) {
        clear_creds(e, now_ms);
    }
}

bool enroll_forget(enroll_t *e, uint32_t now_ms)
{
    if (e == NULL) {
        return false;
    }
    catch_up(e, now_ms);
    if (e->out.state != ENROLL_CLAIMED) {
        return false;
    }
    clear_creds(e, now_ms);
    return true;
}

const enroll_outputs_t *enroll_outputs(const enroll_t *e)
{
    return (e != NULL) ? &e->out : NULL;
}

const enroll_creds_t *enroll_creds(const enroll_t *e)
{
    return (e != NULL) ? &e->creds : NULL;
}

bool enroll_take(enroll_t *e, enroll_act_t *out)
{
    if (e == NULL || out == NULL || e->act_count == 0) {
        return false;
    }
    *out = e->acts[e->act_head];
    e->act_head = (e->act_head + 1) % ENROLL_ACTS;
    e->act_count--;
    return true;
}
