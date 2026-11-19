/*
 * Tests for lib/game – requirements GAME-01 ... GAME-09, GAME-11 ... GAME-15.
 * The helper plays the targets: it follows the commands and remembers which
 * target is lit with which round. Which target the game picks is random, so
 * the tests check properties, never a particular choice.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "game.h"

static game_t *g;
static game_rules_t rules;
static uint32_t now;

static uint8_t lit_round[GAME_MAX_TARGETS]; /* 0 = dark */
static int n_light;
static int n_off;
static int last_lit; /* target of the newest LIGHT command */
static uint16_t last_lit_ms;
static uint8_t alive;

void setUp(void)
{
    g = NULL;
    now = 44000000u;
    rules.hits = 3;
    rules.lit_ms = 8000;
    rules.targets_at_once = 1;
    rules.zone = 2;
    rules.penalty = false;
    rules.escalate = false;
    memset(lit_round, 0, sizeof(lit_round));
    n_light = n_off = 0;
    last_lit = -1;
    last_lit_ms = 0;
    alive = 0x07; /* targets 0, 1, 2 */
}

void tearDown(void)
{
    game_destroy(g);
    g = NULL;
}

static void create(uint32_t seed)
{
    g = game_create(seed, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(g, "game_create returned NULL");
}

static const game_outputs_t *out(void)
{
    return game_outputs(g);
}

static int lit_count(void)
{
    int n = 0;
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        n += lit_round[i] != 0;
    }
    return n;
}

static uint8_t lit_mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        if (lit_round[i]) {
            m |= (uint8_t)(1u << i);
        }
    }
    return m;
}

/* Take every queued command and apply it to the pretend targets. */
static void pump(void)
{
    game_command_t c;
    while (game_take_command(g, &c)) {
        TEST_ASSERT_TRUE(c.target < GAME_MAX_TARGETS);
        if (c.cmd.cmd == LINK_TARGET_LIGHT) {
            TEST_ASSERT_TRUE_MESSAGE(alive >> c.target & 1, "a dead target was lit");
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, lit_round[c.target], "a target that is already lit was lit again");
            TEST_ASSERT_TRUE_MESSAGE(c.cmd.round != 0, "round numbers start at 1");
            lit_round[c.target] = c.cmd.round;
            last_lit = c.target;
            last_lit_ms = c.cmd.arg_ms;
            n_light++;
        } else if (c.cmd.cmd == LINK_TARGET_OFF) {
            lit_round[c.target] = 0;
            n_off++;
        } else {
            TEST_FAIL_MESSAGE("unexpected command");
        }
    }
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(lit_mask(), out()->lit_mask, "outputs.lit_mask follows the commands");
}

static void start(void)
{
    TEST_ASSERT_TRUE(game_start(g, &rules, alive, true, now));
    pump();
}

static int any_lit(void)
{
    for (int i = 0; i < GAME_MAX_TARGETS; i++) {
        if (lit_round[i]) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE("no target is lit");
    return -1;
}

static void hit(int target)
{
    const uint8_t round = lit_round[target];
    lit_round[target] = 0; /* the target goes dark by itself */
    game_on_hit(g, (uint8_t)target, round, now);
    pump();
}

static void miss(int target)
{
    const uint8_t round = lit_round[target];
    lit_round[target] = 0;
    game_on_timeout(g, (uint8_t)target, round, now);
    pump();
}

/* ------------------------------------------------------------------------ */

static void test_rules_from_presets(void)
{
    game_rules_t r;
    memset(&r, 0xEE, sizeof(r));
    TEST_ASSERT_TRUE(game_rules_from_settings(GAME_PRESET_EASY, 9, 9, 3, 3, true, true, &r));
    TEST_ASSERT_EQUAL_UINT8(2, r.hits);
    TEST_ASSERT_EQUAL_UINT16(15000, r.lit_ms);
    TEST_ASSERT_EQUAL_UINT8(1, r.targets_at_once);
    TEST_ASSERT_EQUAL_UINT8(1, r.zone);
    TEST_ASSERT_FALSE(r.penalty);
    TEST_ASSERT_TRUE_MESSAGE(r.escalate, "escalate always comes from its setting");
    TEST_ASSERT_TRUE(game_rules_from_settings(GAME_PRESET_NORMAL, 9, 9, 3, 3, true, false, &r));
    TEST_ASSERT_EQUAL_UINT8(3, r.hits);
    TEST_ASSERT_EQUAL_UINT16(8000, r.lit_ms);
    TEST_ASSERT_EQUAL_UINT8(1, r.targets_at_once);
    TEST_ASSERT_EQUAL_UINT8(2, r.zone);
    TEST_ASSERT_FALSE(r.penalty);
    TEST_ASSERT_FALSE(r.escalate);
    TEST_ASSERT_TRUE(game_rules_from_settings(GAME_PRESET_HARD, 1, 60, 1, 1, false, true, &r));
    TEST_ASSERT_EQUAL_UINT8(5, r.hits);
    TEST_ASSERT_EQUAL_UINT16(4000, r.lit_ms);
    TEST_ASSERT_EQUAL_UINT8(2, r.targets_at_once);
    TEST_ASSERT_EQUAL_UINT8(3, r.zone);
    TEST_ASSERT_TRUE(r.penalty);
    TEST_ASSERT_TRUE(game_rules_from_settings(GAME_PRESET_CUSTOM, 7, 12, 4, 2, true, false, &r));
    TEST_ASSERT_EQUAL_UINT8(7, r.hits);
    TEST_ASSERT_EQUAL_UINT16(12000, r.lit_ms);
    TEST_ASSERT_EQUAL_UINT8(4, r.targets_at_once);
    TEST_ASSERT_EQUAL_UINT8(2, r.zone);
    TEST_ASSERT_TRUE(r.penalty);
    TEST_ASSERT_TRUE(game_rules_valid(&r));

    const game_rules_t untouched = r;
    TEST_ASSERT_FALSE(game_rules_from_settings(4, 3, 8, 1, 1, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 0, 8, 1, 1, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 21, 8, 1, 1, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 3, 1, 1, 1, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 3, 61, 1, 1, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 3, 8, 0, 1, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 3, 8, 5, 1, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 3, 8, 1, 0, false, false, &r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_CUSTOM, 3, 8, 1, 4, false, false, &r));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &r, sizeof(r));
    TEST_ASSERT_FALSE(game_rules_from_settings(GAME_PRESET_EASY, 3, 8, 1, 1, false, false, NULL));
    TEST_ASSERT_FALSE(game_rules_valid(NULL));
    /* out-of-range individual settings do not matter for a preset */
    TEST_ASSERT_TRUE(game_rules_from_settings(GAME_PRESET_EASY, 0, 0, 0, 0, false, false, &r));
}

static void test_start_lights_one_alive_target(void)
{
    create(1);
    TEST_ASSERT_EQUAL_INT(GAME_IDLE, out()->state);
    alive = 0x24; /* only targets 2 and 5 */
    start();
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
    TEST_ASSERT_EQUAL_UINT8(0, out()->hits);
    TEST_ASSERT_EQUAL_UINT8(3, out()->hits_needed);
    TEST_ASSERT_EQUAL_UINT8(0, out()->misses);
    TEST_ASSERT_EQUAL_INT(1, n_light);
    TEST_ASSERT_TRUE(last_lit == 2 || last_lit == 5);
    TEST_ASSERT_EQUAL_UINT16(8000, last_lit_ms);
    game_destroy(NULL);
}

static void test_unavailable_without_gun_or_targets(void)
{
    create(1);
    TEST_ASSERT_FALSE(game_start(g, &rules, 0x07, false, now));
    TEST_ASSERT_EQUAL_INT(GAME_UNAVAILABLE, out()->state);
    pump();
    TEST_ASSERT_EQUAL_INT(0, n_light);
    TEST_ASSERT_FALSE(game_start(g, &rules, 0x00, true, now));
    TEST_ASSERT_EQUAL_INT(GAME_UNAVAILABLE, out()->state);
    /* once everything is back, a start works */
    start();
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);

    tearDown();
    create(1);
    game_rules_t bad = rules;
    bad.hits = 0;
    TEST_ASSERT_FALSE(game_start(g, &bad, 0x07, true, now));
    TEST_ASSERT_EQUAL_INT_MESSAGE(GAME_IDLE, out()->state, "invalid rules change nothing");
    TEST_ASSERT_FALSE(game_start(g, NULL, 0x07, true, now));
}

static void test_three_hits_win(void)
{
    create(2);
    start();
    hit(any_lit());
    TEST_ASSERT_EQUAL_UINT8(1, out()->hits);
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, lit_count(), "another target lights up");
    now += 1500;
    hit(any_lit());
    TEST_ASSERT_EQUAL_UINT8(2, out()->hits);
    now += 1500;
    hit(any_lit());
    TEST_ASSERT_EQUAL_UINT8(3, out()->hits);
    TEST_ASSERT_EQUAL_INT(GAME_WON, out()->state);
    TEST_ASSERT_EQUAL_INT(0, lit_count());
    TEST_ASSERT_EQUAL_INT(3, n_light);
    now += 60000;
    game_tick(g, now);
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, n_light, "nothing lights up after the win");
    TEST_ASSERT_EQUAL_INT(GAME_WON, out()->state);
}

static void test_the_next_target_is_another_one(void)
{
    for (uint32_t seed = 1; seed <= 40; seed++) {
        setUp();
        create(seed);
        rules.hits = 20;
        start();
        for (int i = 0; i < 19; i++) {
            const int t = any_lit();
            hit(t);
            TEST_ASSERT_EQUAL_INT(1, lit_count());
            TEST_ASSERT_TRUE_MESSAGE(any_lit() != t, "the target just hit must not light up again while others can");
            now += 700;
        }
        tearDown();
    }
    /* with a single target there is no choice */
    setUp();
    create(5);
    alive = 0x10;
    start();
    TEST_ASSERT_EQUAL_INT(4, any_lit());
    hit(4);
    TEST_ASSERT_EQUAL_INT(4, any_lit());
    miss(4);
    TEST_ASSERT_EQUAL_INT(4, any_lit());
}

static void test_choice_is_spread_over_all_targets(void)
{
    create(77);
    alive = 0x1D; /* 0, 2, 3, 4 */
    rules.hits = 20;
    rules.penalty = true; /* misses push the goal up: the game goes on */
    start();
    int seen[GAME_MAX_TARGETS] = {0};
    for (int i = 0; i < 25; i++) {
        const int t = any_lit();
        seen[t]++;
        miss(t);
        now += 500;
    }
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
    TEST_ASSERT_TRUE(seen[0] > 0 && seen[2] > 0 && seen[3] > 0 && seen[4] > 0);
    TEST_ASSERT_EQUAL_INT(0, seen[1] + seen[5] + seen[6] + seen[7]);
}

static void test_timeout_is_a_miss(void)
{
    create(3);
    start();
    miss(any_lit());
    TEST_ASSERT_EQUAL_UINT8(1, out()->misses);
    TEST_ASSERT_EQUAL_UINT8(0, out()->hits);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, out()->hits_needed, "no penalty set");
    TEST_ASSERT_EQUAL_INT(1, lit_count());
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
}

static void test_miss_penalty_adds_a_hit(void)
{
    create(3);
    rules.penalty = true;
    rules.hits = 2;
    start();
    miss(any_lit());
    TEST_ASSERT_EQUAL_UINT8(3, out()->hits_needed);
    hit(any_lit());
    hit(any_lit());
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
    hit(any_lit());
    TEST_ASSERT_EQUAL_INT(GAME_WON, out()->state);

    /* the goal never grows past GAME_MAX_HITS */
    tearDown();
    setUp();
    create(4);
    rules.penalty = true;
    rules.hits = 20;
    start();
    for (int i = 0; i < 40; i++) {
        miss(any_lit());
    }
    TEST_ASSERT_EQUAL_UINT8(GAME_MAX_HITS, out()->hits_needed);
    TEST_ASSERT_EQUAL_UINT8(40, out()->misses);
}

static void test_lost_report_counts_as_timeout(void)
{
    create(6);
    start();
    const int t = any_lit();
    now += 8000 + GAME_GRACE_MS - 20;
    game_tick(g, now);
    pump();
    TEST_ASSERT_EQUAL_UINT8(0, out()->misses);
    lit_round[t] = 0; /* the real target went dark at 8 s; its report was lost */
    now += 20;
    game_tick(g, now);
    pump();
    TEST_ASSERT_EQUAL_UINT8(1, out()->misses);
    TEST_ASSERT_EQUAL_INT(1, lit_count());
    TEST_ASSERT_TRUE(any_lit() != t);
}

static void test_stale_and_foreign_reports_are_ignored(void)
{
    create(8);
    start();
    const int t = any_lit();
    const uint8_t round = lit_round[t];
    const int other = (t + 1) % 3;
    game_on_hit(g, (uint8_t)other, round, now); /* not lit */
    game_on_hit(g, (uint8_t)t, (uint8_t)(round + 1), now); /* wrong round */
    game_on_timeout(g, (uint8_t)t, (uint8_t)(round + 1), now);
    game_on_hit(g, 200, round, now); /* no such target */
    pump();
    TEST_ASSERT_EQUAL_UINT8(0, out()->hits);
    TEST_ASSERT_EQUAL_UINT8(0, out()->misses);
    TEST_ASSERT_EQUAL_INT(1, n_light);
    hit(t);
    TEST_ASSERT_EQUAL_UINT8(1, out()->hits);
    game_on_hit(g, (uint8_t)t, round, now); /* the same report again */
    game_on_timeout(g, (uint8_t)t, round, now); /* a late timeout of the old round */
    pump();
    TEST_ASSERT_EQUAL_UINT8(1, out()->hits);
    TEST_ASSERT_EQUAL_UINT8(0, out()->misses);
}

static void test_two_targets_at_once(void)
{
    create(9);
    alive = 0x0F;
    rules.targets_at_once = 2;
    rules.hits = 6;
    start();
    TEST_ASSERT_EQUAL_INT(2, lit_count());
    for (int i = 0; i < 5; i++) {
        hit(any_lit());
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, lit_count(), "always two lit");
        now += 400;
    }
    hit(any_lit());
    TEST_ASSERT_EQUAL_INT(GAME_WON, out()->state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, lit_count(), "the other lit target is switched off");
    TEST_ASSERT_EQUAL_INT(1, n_off);

    /* more wanted than alive: all alive ones are lit */
    tearDown();
    setUp();
    create(9);
    alive = 0x03;
    rules.targets_at_once = 4;
    start();
    TEST_ASSERT_EQUAL_INT(2, lit_count());
    hit(0);
    TEST_ASSERT_EQUAL_INT(2, lit_count());
}

static void test_target_lost_during_the_game(void)
{
    create(10);
    start();
    int t = any_lit();
    alive &= (uint8_t)~(1u << t);
    lit_round[t] = 0;
    game_on_target_lost(g, (uint8_t)t, now);
    pump();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, out()->misses, "not the player's fault");
    TEST_ASSERT_EQUAL_INT(1, lit_count());
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
    /* a dark target dropping out changes nothing visible */
    int dark = -1;
    for (int i = 0; i < 3; i++) {
        if ((alive >> i & 1) && !lit_round[i]) {
            dark = i;
        }
    }
    TEST_ASSERT_TRUE(dark >= 0);
    alive &= (uint8_t)~(1u << dark);
    game_on_target_lost(g, (uint8_t)dark, now);
    pump();
    TEST_ASSERT_EQUAL_INT(1, lit_count());
    /* the last one goes too: the game gives up */
    t = any_lit();
    alive = 0;
    lit_round[t] = 0;
    game_on_target_lost(g, (uint8_t)t, now);
    pump();
    TEST_ASSERT_EQUAL_INT(GAME_UNAVAILABLE, out()->state);
    TEST_ASSERT_EQUAL_INT(0, lit_count());
}

static void test_gun_lost_during_the_game(void)
{
    create(11);
    rules.targets_at_once = 2;
    start();
    game_on_gun_lost(g, now);
    pump();
    TEST_ASSERT_EQUAL_INT(GAME_UNAVAILABLE, out()->state);
    TEST_ASSERT_EQUAL_INT(0, lit_count());
    TEST_ASSERT_EQUAL_INT(2, n_off);
    game_on_gun_lost(g, now); /* again: nothing */
    pump();
    TEST_ASSERT_EQUAL_INT(2, n_off);
}

static void test_stop_switches_everything_off(void)
{
    create(12);
    rules.targets_at_once = 2;
    start();
    game_stop(g, now); /* emergency button, maximum ring time */
    pump();
    TEST_ASSERT_EQUAL_INT(GAME_IDLE, out()->state);
    TEST_ASSERT_EQUAL_INT(0, lit_count());
    TEST_ASSERT_EQUAL_INT(2, n_off);
    now += 60000;
    game_tick(g, now);
    pump();
    TEST_ASSERT_EQUAL_INT(2, n_light);
    /* stop also clears a finished game */
    start();
    rules.hits = 1;
    game_stop(g, now);
    pump();
    start();
    hit(any_lit());
    TEST_ASSERT_EQUAL_INT(GAME_WON, out()->state);
    game_stop(g, now);
    TEST_ASSERT_EQUAL_INT(GAME_IDLE, out()->state);
}

static void test_start_while_running_is_refused(void)
{
    create(13);
    start();
    hit(any_lit());
    TEST_ASSERT_FALSE(game_start(g, &rules, alive, true, now));
    TEST_ASSERT_FALSE(game_start_selftest(g, alive, true, now));
    TEST_ASSERT_EQUAL_UINT8(1, out()->hits);
    hit(any_lit());
    hit(any_lit());
    TEST_ASSERT_EQUAL_INT(GAME_WON, out()->state);
    start(); /* a new game after a win */
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
    TEST_ASSERT_EQUAL_UINT8(0, out()->hits);
    TEST_ASSERT_EQUAL_UINT8(0, out()->misses);
}

static void test_escalation(void)
{
    create(14);
    rules.escalate = true;
    rules.hits = 20;
    rules.penalty = false;
    start();
    for (int level = 0; level <= 7; level++) {
        game_tick(g, now);
        TEST_ASSERT_EQUAL_UINT8(level > 5 ? 5 : level, out()->escalation);
        now += GAME_ESCALATE_MS - 10;
        game_tick(g, now);
        TEST_ASSERT_EQUAL_UINT8(level > 5 ? 5 : level, out()->escalation);
        now += 10; /* targets time out and relight meanwhile; the commands are not followed here */
    }
    game_stop(g, now);
    TEST_ASSERT_EQUAL_UINT8(0, out()->escalation);

    tearDown();
    setUp();
    create(14);
    rules.escalate = false;
    start();
    now += 200000;
    game_tick(g, now);
    TEST_ASSERT_EQUAL_UINT8(0, out()->escalation);
}

static void test_selftest(void)
{
    create(15);
    alive = 0x16; /* 1, 2, 4 */
    TEST_ASSERT_TRUE(game_start_selftest(g, alive, true, now));
    pump();
    TEST_ASSERT_EQUAL_INT(GAME_SELFTEST, out()->state);
    TEST_ASSERT_EQUAL_INT(1, any_lit());
    TEST_ASSERT_EQUAL_UINT16(GAME_SELFTEST_LIT_MS, last_lit_ms);
    hit(1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, any_lit(), "lowest number first");
    miss(2); /* nobody hit it */
    TEST_ASSERT_EQUAL_INT(4, any_lit());
    TEST_ASSERT_EQUAL_INT(GAME_SELFTEST, out()->state);
    hit(4);
    TEST_ASSERT_EQUAL_INT(GAME_SELFTEST_DONE, out()->state);
    TEST_ASSERT_EQUAL_HEX8(0x12, out()->selftest_ok);
    TEST_ASSERT_EQUAL_INT(0, lit_count());
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, out()->hits, "the self-test is not a game");

    /* a lost report ends a target's turn as well */
    TEST_ASSERT_TRUE(game_start_selftest(g, 0x03, true, now));
    alive = 0x03;
    pump();
    TEST_ASSERT_EQUAL_INT(0, any_lit());
    lit_round[0] = 0;
    now += GAME_SELFTEST_LIT_MS + GAME_GRACE_MS;
    game_tick(g, now);
    pump();
    TEST_ASSERT_EQUAL_INT(1, any_lit());
    hit(1);
    TEST_ASSERT_EQUAL_INT(GAME_SELFTEST_DONE, out()->state);
    TEST_ASSERT_EQUAL_HEX8(0x02, out()->selftest_ok);

    TEST_ASSERT_FALSE(game_start_selftest(g, 0x03, false, now));
    TEST_ASSERT_EQUAL_INT(GAME_UNAVAILABLE, out()->state);
}

static void test_round_numbers_and_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 5000u;
    create(16);
    rules.hits = 20;
    rules.penalty = true;
    start();
    uint8_t prev = 0;
    for (int i = 0; i < 300; i++) { /* more than 255 rounds, across the counter wrap */
        const int t = any_lit();
        TEST_ASSERT_TRUE(lit_round[t] != 0);
        TEST_ASSERT_TRUE_MESSAGE(lit_round[t] != prev, "consecutive rounds differ");
        prev = lit_round[t];
        now += 100;
        miss(t);
    }
    TEST_ASSERT_EQUAL_INT(GAME_RUNNING, out()->state);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, out()->misses, "misses stop counting at 255");
    /* the lost-report timer works across the wrap as well */
    const int t = any_lit();
    lit_round[t] = 0;
    now += 8000 + GAME_GRACE_MS;
    game_tick(g, now);
    pump();
    TEST_ASSERT_EQUAL_INT(1, lit_count());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rules_from_presets);
    RUN_TEST(test_start_lights_one_alive_target);
    RUN_TEST(test_unavailable_without_gun_or_targets);
    RUN_TEST(test_three_hits_win);
    RUN_TEST(test_the_next_target_is_another_one);
    RUN_TEST(test_choice_is_spread_over_all_targets);
    RUN_TEST(test_timeout_is_a_miss);
    RUN_TEST(test_miss_penalty_adds_a_hit);
    RUN_TEST(test_lost_report_counts_as_timeout);
    RUN_TEST(test_stale_and_foreign_reports_are_ignored);
    RUN_TEST(test_two_targets_at_once);
    RUN_TEST(test_target_lost_during_the_game);
    RUN_TEST(test_gun_lost_during_the_game);
    RUN_TEST(test_stop_switches_everything_off);
    RUN_TEST(test_start_while_running_is_refused);
    RUN_TEST(test_escalation);
    RUN_TEST(test_selftest);
    RUN_TEST(test_round_numbers_and_clock_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
