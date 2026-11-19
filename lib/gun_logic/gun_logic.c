/*
 * gun_logic – trigger, magazine, sights and sleep of the laser gun.
 * Contract in gun_logic.h, tests in test/test_gun_logic/.
 */
#include "gun_logic.h"

#include <stdlib.h>

#define DEFAULT_SIGHTS 30

struct gun_logic {
    gun_config_t cfg;
    gun_setup_t setup;
    gun_outputs_t out;

    bool pressed;
    uint32_t press_ms;
    bool demo_sent;
    bool fired_once;    /* a press has fired or clicked dry */
    uint32_t last_fire; /* time of that press */
    uint8_t shot;       /* next shot number */

    bool magazine_in;
    uint8_t rounds;

    uint32_t last_activity;

    gun_action_t actions[GUN_ACTIONS];
    uint8_t act_head;
    uint8_t act_count;
};

void gun_config_default(gun_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->cooldown_ms = 250;
        cfg->idle_sleep_ms = 60000;
        cfg->demo_hold_ms = 3000;
    }
}

static bool has_magazine(const gun_logic_t *g)
{
    return g->setup.ammo > 0;
}

static uint8_t ammo_left(const gun_logic_t *g)
{
    return has_magazine(g) ? g->rounds : GUN_NO_MAGAZINE;
}

static void refresh(gun_logic_t *g)
{
    g->out.gun_id = g->setup.gun_id;
    g->out.ammo_left = ammo_left(g);
    g->out.sights = g->out.sleep ? 0 : g->setup.sights;
}

static void push_action(gun_logic_t *g, gun_action_id_t id, uint8_t shot)
{
    if (g->act_count == GUN_ACTIONS) {
        g->act_head = (uint8_t)((g->act_head + 1) % GUN_ACTIONS);
        g->act_count--;
    }
    gun_action_t *a = &g->actions[(g->act_head + g->act_count) % GUN_ACTIONS];
    a->id = id;
    a->shot = shot;
    a->ammo_left = ammo_left(g);
    g->act_count++;
}

static void activity(gun_logic_t *g, uint32_t now_ms)
{
    g->last_activity = now_ms;
    g->out.sleep = false;
    refresh(g);
}

gun_logic_t *gun_logic_create(const gun_config_t *cfg, uint32_t now_ms)
{
    if (cfg == NULL || cfg->cooldown_ms < 50 || cfg->cooldown_ms > 2000 || cfg->idle_sleep_ms < 5000 ||
        cfg->idle_sleep_ms > 600000u || cfg->demo_hold_ms < 1000 || cfg->demo_hold_ms > 10000) {
        return NULL;
    }
    gun_logic_t *g = calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    g->cfg = *cfg;
    g->setup.gun_id = 0;
    g->setup.sights = DEFAULT_SIGHTS;
    g->setup.ammo = 0;
    g->magazine_in = true;
    g->last_activity = now_ms;
    refresh(g);
    return g;
}

void gun_logic_destroy(gun_logic_t *g)
{
    free(g);
}

bool gun_logic_setup(gun_logic_t *g, const gun_setup_t *setup, uint32_t now_ms)
{
    if (g == NULL || setup == NULL || setup->gun_id > 15 || setup->sights > 100 || setup->ammo > 30) {
        return false;
    }
    g->setup = *setup;
    g->rounds = setup->ammo;
    activity(g, now_ms);
    return true;
}

static void update(gun_logic_t *g, uint32_t now_ms)
{
    if (g->pressed && !g->demo_sent && (uint32_t)(now_ms - g->press_ms) >= g->cfg.demo_hold_ms) {
        g->demo_sent = true;
        push_action(g, GUN_ACT_DEMO_REQUEST, 0);
    }
    if (!g->pressed && !g->out.sleep && (uint32_t)(now_ms - g->last_activity) >= g->cfg.idle_sleep_ms) {
        g->out.sleep = true;
        refresh(g);
    }
}

static void press(gun_logic_t *g, uint32_t now_ms)
{
    g->press_ms = now_ms;
    g->demo_sent = false;
    if (g->fired_once && (uint32_t)(now_ms - g->last_fire) < g->cfg.cooldown_ms) {
        return;
    }
    g->fired_once = true;
    g->last_fire = now_ms;
    if (has_magazine(g) && (!g->magazine_in || g->rounds == 0)) {
        push_action(g, GUN_ACT_DRY, 0);
        return;
    }
    const uint8_t shot = g->shot;
    g->shot = (uint8_t)((shot + 1) & 0x0F);
    if (has_magazine(g)) {
        g->rounds--;
        refresh(g);
    }
    push_action(g, GUN_ACT_FIRE, shot);
}

void gun_logic_trigger(gun_logic_t *g, bool pressed, uint32_t now_ms)
{
    if (g == NULL) {
        return;
    }
    update(g, now_ms);
    if (pressed == g->pressed) {
        return;
    }
    g->pressed = pressed;
    activity(g, now_ms);
    if (pressed) {
        press(g, now_ms);
    }
}

void gun_logic_motion(gun_logic_t *g, uint32_t now_ms)
{
    if (g != NULL) {
        activity(g, now_ms);
    }
}

void gun_logic_magazine(gun_logic_t *g, bool inserted, uint32_t now_ms)
{
    if (g == NULL) {
        return;
    }
    update(g, now_ms);
    if (inserted == g->magazine_in) {
        return;
    }
    g->magazine_in = inserted;
    if (inserted && has_magazine(g)) {
        g->rounds = g->setup.ammo;
        refresh(g);
        push_action(g, GUN_ACT_RELOADED, 0);
    }
}

void gun_logic_tick(gun_logic_t *g, uint32_t now_ms)
{
    if (g != NULL) {
        update(g, now_ms);
    }
}

const gun_outputs_t *gun_logic_outputs(const gun_logic_t *g)
{
    return g ? &g->out : NULL;
}

bool gun_logic_take_action(gun_logic_t *g, gun_action_t *out)
{
    if (g == NULL || out == NULL || g->act_count == 0) {
        return false;
    }
    *out = g->actions[g->act_head];
    g->act_head = (uint8_t)((g->act_head + 1) % GUN_ACTIONS);
    g->act_count--;
    return true;
}
