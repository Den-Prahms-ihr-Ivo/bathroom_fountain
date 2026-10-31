/*
 * Tests for lib/mode_controller – requirements BEH-01 … BEH-09, AUD-06, AUD-07.
 *
 * BEH-03 (2026-10-31): a double clap only enters Ambient; leaving is by button
 * unless cfg.clap_exits is set. Tests that leave Ambient therefore use the button.
 */
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "mode_controller.h"

#define T0          10000u /* arbitrary start time, ms */
#define TICK_MS     20u
#define GAIN_EPS    1e-6f

static mode_config_t cfg;
static mode_controller_t *mc;

void setUp(void)
{
    mode_config_default(&cfg);
    cfg.transition_ms = 4000;
    cfg.lockout_ms = 1000;
    mc = NULL;
}

void tearDown(void)
{
    mode_controller_destroy(mc);
    mc = NULL;
}

static void create_at(uint32_t now_ms)
{
    mc = mode_controller_create(&cfg, now_ms);
    TEST_ASSERT_NOT_NULL_MESSAGE(mc, "mode_controller_create returned NULL for a valid config");
    TEST_ASSERT_NOT_NULL_MESSAGE(mode_controller_outputs(mc), "outputs() returned NULL");
}

static const mode_outputs_t *out(void)
{
    return mode_controller_outputs(mc);
}

static void tick_until(uint32_t from_ms, uint32_t to_ms)
{
    for (uint32_t t = from_ms; (int32_t)(to_ms - t) > 0; t += TICK_MS) {
        mode_controller_tick(mc, t);
    }
    mode_controller_tick(mc, to_ms);
}

static void assert_all_off(void)
{
    TEST_ASSERT_FALSE(out()->candles_on);
    TEST_ASSERT_FALSE(out()->fountain_on);
    TEST_ASSERT_FALSE(out()->music_playing);
    TEST_ASSERT_FALSE_MESSAGE(out()->amp_enabled, "amplifier must be in power-down (AUD-07)");
    TEST_ASSERT_FLOAT_WITHIN(GAIN_EPS, 0.0f, out()->fade_gain);
}

static void assert_take_hue(hue_scene_t scene, uint32_t duration_ms)
{
    hue_command_t cmd;
    memset(&cmd, 0xA5, sizeof(cmd));
    TEST_ASSERT_TRUE_MESSAGE(mode_controller_take_hue_command(mc, &cmd), "expected a Hue command");
    TEST_ASSERT_EQUAL_INT(scene, cmd.scene);
    TEST_ASSERT_EQUAL_UINT32(duration_ms, cmd.duration_ms);
    TEST_ASSERT_FALSE_MESSAGE(mode_controller_take_hue_command(mc, &cmd), "Hue command must be taken only once");
}

static void assert_no_hue(void)
{
    hue_command_t cmd;
    TEST_ASSERT_FALSE(mode_controller_take_hue_command(mc, &cmd));
}

/* Enter Ambient at t and run the fade to completion; returns the end time. */
static uint32_t enter_ambient(uint32_t t, trigger_source_t src)
{
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, src, t));
    tick_until(t, t + cfg.transition_ms);
    return t + cfg.transition_ms;
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

static void test_defaults_match_requirements(void)
{
    mode_config_t d;
    memset(&d, 0xA5, sizeof(d));
    mode_config_default(&d);
    TEST_ASSERT_EQUAL_UINT32(4000, d.transition_ms); /* BEH-05/06: 3–5 s */
    TEST_ASSERT_EQUAL_UINT32(1000, d.lockout_ms);    /* BEH-08 */
    TEST_ASSERT_FALSE_MESSAGE(d.clap_exits, "BEH-03: a clap does not leave Ambient by default");
}

static void test_create_enforces_transition_range(void)
{
    create_at(T0);

    TEST_ASSERT_NULL(mode_controller_create(NULL, T0));

    mode_config_t c = cfg;
    mode_controller_t *m;

    c.transition_ms = 2999;
    TEST_ASSERT_NULL_MESSAGE(mode_controller_create(&c, T0), "2999 ms is below 3 s");
    c.transition_ms = 5001;
    TEST_ASSERT_NULL_MESSAGE(mode_controller_create(&c, T0), "5001 ms is above 5 s");

    c.transition_ms = 3000;
    m = mode_controller_create(&c, T0);
    TEST_ASSERT_NOT_NULL_MESSAGE(m, "3000 ms is allowed");
    mode_controller_destroy(m);

    c.transition_ms = 5000;
    m = mode_controller_create(&c, T0);
    TEST_ASSERT_NOT_NULL_MESSAGE(m, "5000 ms is allowed");
    mode_controller_destroy(m);
}

/* ------------------------------------------------------------------------ */
/* BEH-01/02: start-up                                                      */
/* ------------------------------------------------------------------------ */

static void test_starts_in_normal_with_everything_off(void)
{
    create_at(T0);
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
    TEST_ASSERT_FALSE(out()->transitioning);
    assert_all_off();
    assert_no_hue();
}

static void test_startup_ticks_change_nothing(void)
{
    create_at(T0);
    tick_until(T0, T0 + 60000);
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
    assert_all_off();
    assert_no_hue();
}

/* ------------------------------------------------------------------------ */
/* BEH-03/05: entering Ambient                                              */
/* ------------------------------------------------------------------------ */

static void check_trigger_enters_ambient(trigger_source_t src)
{
    create_at(T0);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, src, T0 + 100));

    TEST_ASSERT_EQUAL_INT(AMBIENCE_AMBIENT, out()->mode);
    TEST_ASSERT_TRUE(out()->transitioning);
    TEST_ASSERT_TRUE(out()->candles_on);
    TEST_ASSERT_TRUE(out()->fountain_on);
    TEST_ASSERT_TRUE(out()->amp_enabled);
    TEST_ASSERT_TRUE(out()->music_playing);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, out()->fade_gain);
    assert_take_hue(HUE_SCENE_AMBIENT, cfg.transition_ms);
}

static void test_clap_enters_ambient(void)
{
    check_trigger_enters_ambient(TRIGGER_CLAP);
}

static void test_button_enters_ambient(void)
{
    check_trigger_enters_ambient(TRIGGER_BUTTON);
}

static void test_fade_in_rises_monotonically_to_exactly_one(void) /* AUD-06 */
{
    create_at(T0);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_CLAP, T0));

    float prev = out()->fade_gain;
    for (uint32_t t = T0 + TICK_MS; t < T0 + cfg.transition_ms; t += TICK_MS) {
        mode_controller_tick(mc, t);
        TEST_ASSERT_TRUE_MESSAGE(out()->fade_gain >= prev, "fade-in must not decrease");
        TEST_ASSERT_TRUE(out()->transitioning);
        prev = out()->fade_gain;
    }

    mode_controller_tick(mc, T0 + cfg.transition_ms);
    TEST_ASSERT_FALSE(out()->transitioning);
    TEST_ASSERT_FLOAT_WITHIN(GAIN_EPS, 1.0f, out()->fade_gain);
}

static void test_fade_in_is_partway_at_midpoint(void)
{
    create_at(T0);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_CLAP, T0));
    mode_controller_tick(mc, T0 + cfg.transition_ms / 2);
    TEST_ASSERT_TRUE(out()->fade_gain > 0.0f);
    TEST_ASSERT_TRUE(out()->fade_gain < 1.0f);
}

/* ------------------------------------------------------------------------ */
/* BEH-08: lockout                                                          */
/* ------------------------------------------------------------------------ */

static void test_trigger_ignored_during_transition(void)
{
    create_at(T0);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_CLAP, T0));
    assert_take_hue(HUE_SCENE_AMBIENT, cfg.transition_ms);

    tick_until(T0, T0 + 1000);
    TEST_ASSERT_FALSE(mode_controller_trigger(mc, TRIGGER_BUTTON, T0 + 1000));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_AMBIENT, out()->mode);
    TEST_ASSERT_TRUE(out()->transitioning);
    assert_no_hue();
}

static void test_trigger_ignored_until_lockout_has_passed(void)
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_CLAP);

    TEST_ASSERT_FALSE(mode_controller_trigger(mc, TRIGGER_BUTTON, end + cfg.lockout_ms - 1));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_AMBIENT, out()->mode);
    TEST_ASSERT_FALSE(out()->transitioning);

    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_BUTTON, end + cfg.lockout_ms));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
}

/* ------------------------------------------------------------------------ */
/* BEH-04/06/07, AUD-06/07: leaving Ambient                                 */
/* ------------------------------------------------------------------------ */

static void test_leaving_fades_out_then_switches_everything_off(void)
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_CLAP);
    assert_take_hue(HUE_SCENE_AMBIENT, cfg.transition_ms);

    const uint32_t t1 = end + 5000;
    tick_until(end, t1);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_BUTTON, t1));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
    TEST_ASSERT_TRUE(out()->transitioning);
    assert_take_hue(HUE_SCENE_NORMAL, cfg.transition_ms);

    float prev = out()->fade_gain;
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, prev);
    for (uint32_t t = t1 + TICK_MS; t < t1 + cfg.transition_ms; t += TICK_MS) {
        mode_controller_tick(mc, t);
        TEST_ASSERT_TRUE_MESSAGE(out()->fade_gain <= prev, "fade-out must not increase");
        TEST_ASSERT_TRUE_MESSAGE(out()->candles_on, "candles stay on during the fade");
        TEST_ASSERT_TRUE_MESSAGE(out()->fountain_on, "fountain stays on during the fade");
        TEST_ASSERT_TRUE_MESSAGE(out()->music_playing, "music plays during the fade");
        TEST_ASSERT_TRUE(out()->amp_enabled);
        prev = out()->fade_gain;
    }

    mode_controller_tick(mc, t1 + cfg.transition_ms);
    TEST_ASSERT_FALSE(out()->transitioning);
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
    assert_all_off();
}

static void test_clap_in_ambient_is_ignored(void) /* BEH-03 */
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_CLAP);
    assert_take_hue(HUE_SCENE_AMBIENT, cfg.transition_ms);

    const uint32_t times[] = {end + cfg.lockout_ms, end + 60000u, end + 3600000u};
    uint32_t last = end;
    for (unsigned i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
        tick_until(last, times[i]);
        last = times[i];
        TEST_ASSERT_FALSE_MESSAGE(mode_controller_trigger(mc, TRIGGER_CLAP, times[i]),
                                  "a clap must not leave Ambient");
        TEST_ASSERT_EQUAL_INT(AMBIENCE_AMBIENT, out()->mode);
        TEST_ASSERT_FALSE(out()->transitioning);
        TEST_ASSERT_TRUE(out()->candles_on);
        TEST_ASSERT_TRUE(out()->fountain_on);
        TEST_ASSERT_TRUE(out()->music_playing);
        TEST_ASSERT_TRUE(out()->amp_enabled);
        TEST_ASSERT_FLOAT_WITHIN(GAIN_EPS, 1.0f, out()->fade_gain);
        assert_no_hue();
    }
}

static void test_ignored_clap_does_not_delay_the_button(void) /* BEH-03, BEH-08 */
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_BUTTON);
    const uint32_t t = end + cfg.lockout_ms;
    TEST_ASSERT_FALSE(mode_controller_trigger(mc, TRIGGER_CLAP, t));
    TEST_ASSERT_TRUE_MESSAGE(mode_controller_trigger(mc, TRIGGER_BUTTON, t),
                             "an ignored clap must not restart the lockout");
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
}

static void test_button_leaves_ambient_entered_by_clap(void) /* BEH-04 */
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_CLAP);
    assert_take_hue(HUE_SCENE_AMBIENT, cfg.transition_ms);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_BUTTON, end + cfg.lockout_ms));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
    assert_take_hue(HUE_SCENE_NORMAL, cfg.transition_ms);
}

static void test_clap_enters_again_after_leaving(void) /* BEH-03 */
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_CLAP);
    const uint32_t t_exit = end + cfg.lockout_ms;
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_BUTTON, t_exit));
    tick_until(t_exit, t_exit + cfg.transition_ms);
    const uint32_t t_again = t_exit + cfg.transition_ms + cfg.lockout_ms;
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_CLAP, t_again));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_AMBIENT, out()->mode);
}

static void test_clap_exits_when_enabled(void) /* BEH-03 option */
{
    cfg.clap_exits = true;
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_BUTTON);
    assert_take_hue(HUE_SCENE_AMBIENT, cfg.transition_ms);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_CLAP, end + cfg.lockout_ms));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
    assert_take_hue(HUE_SCENE_NORMAL, cfg.transition_ms);
}

static void test_trigger_without_ticks_finishes_previous_transition_first(void)
{
    create_at(T0);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_CLAP, T0));
    /* no ticks at all */
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_BUTTON, T0 + cfg.transition_ms + cfg.lockout_ms));
    TEST_ASSERT_EQUAL_INT(AMBIENCE_NORMAL, out()->mode);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, out()->fade_gain);
}

/* ------------------------------------------------------------------------ */
/* Hue command hand-off, BEH-09                                             */
/* ------------------------------------------------------------------------ */

static void test_newer_hue_command_replaces_untaken_one(void)
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_CLAP);
    /* Ambient command deliberately not taken */
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_BUTTON, end + cfg.lockout_ms));
    assert_take_hue(HUE_SCENE_NORMAL, cfg.transition_ms);
}

static void test_holding_ambient_issues_no_hue_commands(void)
{
    create_at(T0);
    const uint32_t end = enter_ambient(T0, TRIGGER_CLAP);
    assert_take_hue(HUE_SCENE_AMBIENT, cfg.transition_ms);

    for (uint32_t t = end; t < end + 3600000u; t += 100) {
        mode_controller_tick(mc, t);
    }
    assert_no_hue();
    TEST_ASSERT_EQUAL_INT(AMBIENCE_AMBIENT, out()->mode);
    TEST_ASSERT_TRUE(out()->candles_on);
    TEST_ASSERT_TRUE(out()->fountain_on);
    TEST_ASSERT_TRUE(out()->music_playing);
    TEST_ASSERT_FLOAT_WITHIN(GAIN_EPS, 1.0f, out()->fade_gain);
}

/* ------------------------------------------------------------------------ */
/* Robustness                                                               */
/* ------------------------------------------------------------------------ */

static void test_works_across_millisecond_counter_wrap(void)
{
    const uint32_t start = UINT32_MAX - 1500u; /* wraps 1.5 s into the fade */
    create_at(start);
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_CLAP, start));

    tick_until(start, start + cfg.transition_ms / 2);
    TEST_ASSERT_TRUE(out()->transitioning);
    TEST_ASSERT_TRUE(out()->fade_gain > 0.0f && out()->fade_gain < 1.0f);

    const uint32_t end = start + cfg.transition_ms; /* wrapped */
    tick_until(start + cfg.transition_ms / 2, end);
    TEST_ASSERT_FALSE(out()->transitioning);
    TEST_ASSERT_FLOAT_WITHIN(GAIN_EPS, 1.0f, out()->fade_gain);

    TEST_ASSERT_FALSE(mode_controller_trigger(mc, TRIGGER_BUTTON, end + cfg.lockout_ms - 1));
    TEST_ASSERT_TRUE(mode_controller_trigger(mc, TRIGGER_BUTTON, end + cfg.lockout_ms));
}

static void test_outputs_pointer_is_stable(void)
{
    create_at(T0);
    const mode_outputs_t *p = mode_controller_outputs(mc);
    enter_ambient(T0, TRIGGER_CLAP);
    TEST_ASSERT_EQUAL_PTR(p, mode_controller_outputs(mc));
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_requirements);
    RUN_TEST(test_create_enforces_transition_range);
    RUN_TEST(test_starts_in_normal_with_everything_off);
    RUN_TEST(test_startup_ticks_change_nothing);
    RUN_TEST(test_clap_enters_ambient);
    RUN_TEST(test_button_enters_ambient);
    RUN_TEST(test_fade_in_rises_monotonically_to_exactly_one);
    RUN_TEST(test_fade_in_is_partway_at_midpoint);
    RUN_TEST(test_trigger_ignored_during_transition);
    RUN_TEST(test_trigger_ignored_until_lockout_has_passed);
    RUN_TEST(test_leaving_fades_out_then_switches_everything_off);
    RUN_TEST(test_clap_in_ambient_is_ignored);
    RUN_TEST(test_ignored_clap_does_not_delay_the_button);
    RUN_TEST(test_button_leaves_ambient_entered_by_clap);
    RUN_TEST(test_clap_enters_again_after_leaving);
    RUN_TEST(test_clap_exits_when_enabled);
    RUN_TEST(test_trigger_without_ticks_finishes_previous_transition_first);
    RUN_TEST(test_newer_hue_command_replaces_untaken_one);
    RUN_TEST(test_holding_ambient_issues_no_hue_commands);
    RUN_TEST(test_works_across_millisecond_counter_wrap);
    RUN_TEST(test_outputs_pointer_is_stable);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
