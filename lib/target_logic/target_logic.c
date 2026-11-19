/*
 * target_logic – one target: lit, hit, timed out; and its sleep plan.
 * Contract in target_logic.h, tests in test/test_target_logic/.
 */
#include "target_logic.h"

#include <stdlib.h>
#include <string.h>

#define ARG_MIN_MS 100u
#define ARG_MAX_MS 60000u

struct target_logic {
    target_config_t cfg;
    uint8_t gun_id;
    uint8_t zone;

    target_led_t led;
    uint8_t round;
    uint32_t since;  /* start of the current LED phase */
    uint32_t len_ms; /* its length */

    link_target_evt_t events[TARGET_EVENTS];
    uint8_t ev_head;
    uint8_t ev_count;
};

void target_config_default(target_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->flash_ms = 300;
        cfg->zone_min[0] = 150;
        cfg->zone_min[1] = 400;
        cfg->zone_min[2] = 700;
    }
}

target_logic_t *target_logic_create(const target_config_t *cfg, uint32_t now_ms)
{
    if (cfg == NULL || cfg->flash_ms < 50 || cfg->flash_ms > 2000) {
        return NULL;
    }
    for (int i = 0; i < 3; i++) {
        if (cfg->zone_min[i] < 1 || cfg->zone_min[i] > 1000 || (i > 0 && cfg->zone_min[i] < cfg->zone_min[i - 1])) {
            return NULL;
        }
    }
    target_logic_t *t = calloc(1, sizeof(*t));
    if (t != NULL) {
        t->cfg = *cfg;
        t->gun_id = TARGET_ANY_GUN;
        t->zone = 1;
        t->led = TARGET_LED_OFF;
        t->since = now_ms;
    }
    return t;
}

void target_logic_destroy(target_logic_t *t)
{
    free(t);
}

bool target_logic_setup(target_logic_t *t, uint8_t gun_id, uint8_t zone)
{
    if (t == NULL || (gun_id > 15 && gun_id != TARGET_ANY_GUN) || zone < 1 || zone > 3) {
        return false;
    }
    t->gun_id = gun_id;
    t->zone = zone;
    return true;
}

static void push_event(target_logic_t *t, uint8_t evt, uint8_t round, uint8_t gun_id, uint16_t value)
{
    if (t->ev_count == TARGET_EVENTS) {
        t->ev_head = (uint8_t)((t->ev_head + 1) % TARGET_EVENTS);
        t->ev_count--;
    }
    link_target_evt_t *e = &t->events[(t->ev_head + t->ev_count) % TARGET_EVENTS];
    e->evt = evt;
    e->round = round;
    e->gun_id = gun_id;
    e->value = value;
    t->ev_count++;
}

static void set_led(target_logic_t *t, target_led_t led, uint32_t len_ms, uint32_t now_ms)
{
    t->led = led;
    t->since = now_ms;
    t->len_ms = len_ms;
}

static void update(target_logic_t *t, uint32_t now_ms)
{
    if (t->led == TARGET_LED_OFF || (uint32_t)(now_ms - t->since) < t->len_ms) {
        return;
    }
    if (t->led == TARGET_LED_LIT) {
        push_event(t, LINK_TARGET_TIMEOUT, t->round, 0, 0);
    }
    t->led = TARGET_LED_OFF;
}

bool target_logic_command(target_logic_t *t, const link_target_cmd_t *cmd, uint32_t now_ms)
{
    if (t == NULL || cmd == NULL) {
        return false;
    }
    update(t, now_ms);
    switch (cmd->cmd) {
    case LINK_TARGET_LIGHT:
        if (cmd->arg_ms < ARG_MIN_MS || cmd->arg_ms > ARG_MAX_MS) {
            return false;
        }
        t->round = cmd->round;
        set_led(t, TARGET_LED_LIT, cmd->arg_ms, now_ms);
        return true;
    case LINK_TARGET_OFF:
        set_led(t, TARGET_LED_OFF, 0, now_ms);
        return true;
    case LINK_TARGET_IDENTIFY:
        if (cmd->arg_ms < ARG_MIN_MS || cmd->arg_ms > ARG_MAX_MS) {
            return false;
        }
        set_led(t, TARGET_LED_IDENTIFY, cmd->arg_ms, now_ms);
        return true;
    default:
        return false;
    }
}

bool target_logic_shot(target_logic_t *t, uint8_t gun_id, uint16_t strength, uint32_t now_ms)
{
    if (t == NULL) {
        return false;
    }
    update(t, now_ms);
    if (t->led != TARGET_LED_LIT) {
        return false;
    }
    if (t->gun_id != TARGET_ANY_GUN && gun_id != t->gun_id) {
        return false;
    }
    if (strength < t->cfg.zone_min[t->zone - 1]) {
        return false;
    }
    push_event(t, LINK_TARGET_HIT, t->round, gun_id, strength);
    set_led(t, TARGET_LED_FLASH, t->cfg.flash_ms, now_ms);
    return true;
}

void target_logic_tick(target_logic_t *t, uint32_t now_ms)
{
    if (t != NULL) {
        update(t, now_ms);
    }
}

target_led_t target_logic_led(const target_logic_t *t)
{
    return t ? t->led : TARGET_LED_OFF;
}

bool target_logic_take_event(target_logic_t *t, link_target_evt_t *out)
{
    if (t == NULL || out == NULL || t->ev_count == 0) {
        return false;
    }
    *out = t->events[t->ev_head];
    t->ev_head = (uint8_t)((t->ev_head + 1) % TARGET_EVENTS);
    t->ev_count--;
    return true;
}

uint32_t target_sleep_seconds(bool alarm_known, uint32_t seconds_to_alarm, uint32_t lead_s, uint32_t max_s)
{
    if (!alarm_known) {
        return max_s;
    }
    if (seconds_to_alarm <= lead_s) {
        return 0;
    }
    const uint32_t s = seconds_to_alarm - lead_s;
    return s < max_s ? s : max_s;
}
