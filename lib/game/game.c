/*
 * game – the alarm game master.
 * Contract in game.h, tests in test/test_game/.
 */
#include "game.h"

#include <stdlib.h>
#include <string.h>

#define ESCALATION_MAX 5

typedef struct {
    bool lit;
    uint8_t round;
    uint32_t lit_at;
} target_t;

struct game {
    uint32_t rng;
    game_rules_t rules;
    game_outputs_t out;
    uint8_t alive;
    target_t t[GAME_MAX_TARGETS];
    uint8_t next_round;
    uint32_t start_ms;
    uint16_t lit_ms;        /* rules.lit_ms, or GAME_SELFTEST_LIT_MS */
    int8_t selftest_target; /* target whose turn it is */

    game_command_t cmds[GAME_COMMANDS];
    uint8_t cmd_head;
    uint8_t cmd_count;
};

/* ---- rules -------------------------------------------------------------- */

bool game_rules_valid(const game_rules_t *r)
{
    return r != NULL && r->hits >= 1 && r->hits <= 20 && r->lit_ms >= 2000 && r->lit_ms <= 60000 &&
           r->targets_at_once >= 1 && r->targets_at_once <= 4 && r->zone >= 1 && r->zone <= 3;
}

bool game_rules_from_settings(uint8_t preset, uint8_t hits, uint8_t lit_s, uint8_t targets, uint8_t zone,
                              bool penalty, bool escalate, game_rules_t *out)
{
    if (out == NULL) {
        return false;
    }
    game_rules_t r;
    r.escalate = escalate;
    switch (preset) {
    case GAME_PRESET_EASY:
        r.hits = 2;
        r.lit_ms = 15000;
        r.targets_at_once = 1;
        r.zone = 1;
        r.penalty = false;
        break;
    case GAME_PRESET_NORMAL:
        r.hits = 3;
        r.lit_ms = 8000;
        r.targets_at_once = 1;
        r.zone = 2;
        r.penalty = false;
        break;
    case GAME_PRESET_HARD:
        r.hits = 5;
        r.lit_ms = 4000;
        r.targets_at_once = 2;
        r.zone = 3;
        r.penalty = true;
        break;
    case GAME_PRESET_CUSTOM:
        if (lit_s < 2 || lit_s > 60) {
            return false;
        }
        r.hits = hits;
        r.lit_ms = (uint16_t)(lit_s * 1000u);
        r.targets_at_once = targets;
        r.zone = zone;
        r.penalty = penalty;
        if (!game_rules_valid(&r)) {
            return false;
        }
        break;
    default:
        return false;
    }
    *out = r;
    return true;
}

/* ---- helpers ------------------------------------------------------------ */

game_t *game_create(uint32_t seed, uint32_t now_ms)
{
    (void)now_ms;
    game_t *g = calloc(1, sizeof(*g));
    if (g != NULL) {
        g->rng = seed ? seed : 0x9E3779B9u;
        g->next_round = 1;
        g->out.state = GAME_IDLE;
    }
    return g;
}

void game_destroy(game_t *g)
{
    free(g);
}

static uint32_t rnd(game_t *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

static void push_cmd(game_t *g, uint8_t target, uint8_t cmd, uint8_t round, uint16_t arg_ms)
{
    if (g->cmd_count == GAME_COMMANDS) {
        g->cmd_head = (uint8_t)((g->cmd_head + 1) % GAME_COMMANDS);
        g->cmd_count--;
    }
    game_command_t *c = &g->cmds[(g->cmd_head + g->cmd_count) % GAME_COMMANDS];
    c->target = target;
    c->cmd.cmd = cmd;
    c->cmd.round = round;
    c->cmd.arg_ms = arg_ms;
    g->cmd_count++;
}

static void update_mask(game_t *g)
{
    uint8_t m = 0;
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        if (g->t[i].lit) {
            m |= (uint8_t)(1u << i);
        }
    }
    g->out.lit_mask = m;
}

static void light(game_t *g, int target, uint32_t now_ms)
{
    const uint8_t round = g->next_round;
    g->next_round = (uint8_t)(round == 255 ? 1 : round + 1);
    g->t[target].lit = true;
    g->t[target].round = round;
    g->t[target].lit_at = now_ms;
    push_cmd(g, (uint8_t)target, LINK_TARGET_LIGHT, round, g->lit_ms);
    update_mask(g);
}

static void all_off(game_t *g)
{
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        if (g->t[i].lit) {
            g->t[i].lit = false;
            push_cmd(g, (uint8_t)i, LINK_TARGET_OFF, g->t[i].round, 0);
        }
    }
    update_mask(g);
}

/* Light one more target, avoiding `avoid` (-1: none) unless it is the only
 * choice. Returns false if no target can be lit. */
static bool light_another(game_t *g, int avoid, uint32_t now_ms)
{
    uint8_t candidates = (uint8_t)(g->alive & (uint8_t)~g->out.lit_mask);
    if (avoid >= 0 && (candidates & (uint8_t)~(1u << avoid)) != 0) {
        candidates &= (uint8_t)~(1u << avoid);
    }
    int n = 0;
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        n += (candidates >> i) & 1;
    }
    if (n == 0) {
        return false;
    }
    int pick = (int)(rnd(g) % (uint32_t)n);
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        if ((candidates >> i) & 1) {
            if (pick == 0) {
                light(g, i, now_ms);
                return true;
            }
            pick--;
        }
    }
    return false;
}

static int lit_count(const game_t *g)
{
    int n = 0;
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        n += g->t[i].lit;
    }
    return n;
}

/* Self-test: light the next alive target above `after`, or finish. */
static void selftest_next(game_t *g, int after, uint32_t now_ms)
{
    for (int i = after + 1; i < GAME_MAX_TARGETS; i++) {
        if ((g->alive >> i) & 1) {
            g->selftest_target = (int8_t)i;
            light(g, i, now_ms);
            return;
        }
    }
    g->selftest_target = -1;
    all_off(g);
    g->out.state = GAME_SELFTEST_DONE;
}

static bool can_start(game_t *g, uint8_t alive_mask, bool gun_alive)
{
    if (!gun_alive || alive_mask == 0) {
        all_off(g);
        g->out.state = GAME_UNAVAILABLE;
        g->out.escalation = 0;
        return false;
    }
    return true;
}

static void begin(game_t *g, uint8_t alive_mask, uint32_t now_ms)
{
    all_off(g);
    g->alive = alive_mask;
    g->start_ms = now_ms;
    g->out.hits = 0;
    g->out.misses = 0;
    g->out.escalation = 0;
}

/* ---- API ---------------------------------------------------------------- */

bool game_start(game_t *g, const game_rules_t *rules, uint8_t alive_mask, bool gun_alive, uint32_t now_ms)
{
    if (g == NULL || g->out.state == GAME_RUNNING || g->out.state == GAME_SELFTEST || !game_rules_valid(rules)) {
        return false;
    }
    if (!can_start(g, alive_mask, gun_alive)) {
        return false;
    }
    begin(g, alive_mask, now_ms);
    g->rules = *rules;
    g->lit_ms = rules->lit_ms;
    g->out.hits_needed = rules->hits;
    g->out.state = GAME_RUNNING;
    for (int i = 0; i < rules->targets_at_once; i++) {
        if (!light_another(g, -1, now_ms)) {
            break;
        }
    }
    return true;
}

bool game_start_selftest(game_t *g, uint8_t alive_mask, bool gun_alive, uint32_t now_ms)
{
    if (g == NULL || g->out.state == GAME_RUNNING || g->out.state == GAME_SELFTEST) {
        return false;
    }
    if (!can_start(g, alive_mask, gun_alive)) {
        return false;
    }
    begin(g, alive_mask, now_ms);
    memset(&g->rules, 0, sizeof(g->rules));
    g->lit_ms = GAME_SELFTEST_LIT_MS;
    g->out.hits_needed = 0;
    g->out.selftest_ok = 0;
    g->out.state = GAME_SELFTEST;
    selftest_next(g, -1, now_ms);
    return true;
}

/* A report from a lit target with the right round ends its turn. */
static bool report_ok(const game_t *g, uint8_t target, uint8_t round)
{
    return target < GAME_MAX_TARGETS && g->t[target].lit && g->t[target].round == round;
}

static void target_done(game_t *g, uint8_t target, bool hit, uint32_t now_ms)
{
    g->t[target].lit = false;
    update_mask(g);
    if (g->out.state == GAME_SELFTEST) {
        if (hit) {
            g->out.selftest_ok |= (uint8_t)(1u << target);
        }
        selftest_next(g, target, now_ms);
        return;
    }
    if (hit) {
        g->out.hits++;
        if (g->out.hits >= g->out.hits_needed) {
            all_off(g);
            g->out.state = GAME_WON;
            return;
        }
    } else {
        if (g->out.misses < 255) {
            g->out.misses++;
        }
        if (g->rules.penalty && g->out.hits_needed < GAME_MAX_HITS) {
            g->out.hits_needed++;
        }
    }
    light_another(g, target, now_ms);
}

static bool active(const game_t *g)
{
    return g->out.state == GAME_RUNNING || g->out.state == GAME_SELFTEST;
}

void game_on_hit(game_t *g, uint8_t target, uint8_t round, uint32_t now_ms)
{
    if (g != NULL && active(g) && report_ok(g, target, round)) {
        target_done(g, target, true, now_ms);
    }
}

void game_on_timeout(game_t *g, uint8_t target, uint8_t round, uint32_t now_ms)
{
    if (g != NULL && active(g) && report_ok(g, target, round)) {
        target_done(g, target, false, now_ms);
    }
}

void game_on_target_lost(game_t *g, uint8_t target, uint32_t now_ms)
{
    if (g == NULL || target >= GAME_MAX_TARGETS) {
        return;
    }
    g->alive &= (uint8_t)~(1u << target);
    if (!active(g)) {
        return;
    }
    const bool was_lit = g->t[target].lit;
    g->t[target].lit = false;
    update_mask(g);
    if (g->alive == 0) {
        all_off(g);
        g->out.state = GAME_UNAVAILABLE;
        return;
    }
    if (!was_lit) {
        return;
    }
    if (g->out.state == GAME_SELFTEST) {
        selftest_next(g, target, now_ms);
    } else {
        light_another(g, target, now_ms);
    }
}

void game_on_gun_lost(game_t *g, uint32_t now_ms)
{
    (void)now_ms;
    if (g == NULL || !active(g)) {
        return;
    }
    all_off(g);
    g->out.state = GAME_UNAVAILABLE;
}

void game_stop(game_t *g, uint32_t now_ms)
{
    (void)now_ms;
    if (g == NULL) {
        return;
    }
    all_off(g);
    g->out.state = GAME_IDLE;
    g->out.escalation = 0;
}

void game_tick(game_t *g, uint32_t now_ms)
{
    if (g == NULL || !active(g)) {
        return;
    }
    /* lost reports: oldest first, so a chain of timeouts keeps its order */
    for (;;) {
        int oldest = -1;
        uint32_t oldest_age = 0;
        for (int i = 0; i < GAME_MAX_TARGETS; i++) {
            if (!g->t[i].lit) {
                continue;
            }
            const uint32_t age = now_ms - g->t[i].lit_at;
            if (age >= (uint32_t)g->lit_ms + GAME_GRACE_MS && (oldest < 0 || age > oldest_age)) {
                oldest = i;
                oldest_age = age;
            }
        }
        if (oldest < 0 || !active(g)) {
            break;
        }
        target_done(g, (uint8_t)oldest, false, now_ms);
        if (!active(g) || lit_count(g) == 0) {
            break;
        }
    }
    if (g->out.state == GAME_RUNNING && g->rules.escalate) {
        const uint32_t level = (now_ms - g->start_ms) / GAME_ESCALATE_MS;
        g->out.escalation = (uint8_t)(level > ESCALATION_MAX ? ESCALATION_MAX : level);
    } else if (g->out.state == GAME_RUNNING) {
        g->out.escalation = 0;
    }
}

const game_outputs_t *game_outputs(const game_t *g)
{
    return g ? &g->out : NULL;
}

bool game_take_command(game_t *g, game_command_t *out)
{
    if (g == NULL || out == NULL || g->cmd_count == 0) {
        return false;
    }
    *out = g->cmds[g->cmd_head];
    g->cmd_head = (uint8_t)((g->cmd_head + 1) % GAME_COMMANDS);
    g->cmd_count--;
    return true;
}
