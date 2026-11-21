/*
 * Tests for lib/bed_modes – requirements BED-40 ... BED-52.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "bed_modes.h"

#define T0 100000u
#define MIN 60000u
#define NIGHT 20400u /* some night number */

static bed_modes_config_t cfg;
static bed_modes_t *bm;

void setUp(void)
{
    bed_modes_config_default(&cfg);
    bm = NULL;
}

void tearDown(void)
{
    bed_modes_destroy(bm);
    bm = NULL;
}

static void create_at(uint32_t saved_night, uint32_t now)
{
    bm = bed_modes_create(&cfg, saved_night, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(bm, "bed_modes_create returned NULL for a valid config");
}

static void create(void)
{
    create_at(BED_NIGHT_UNKNOWN, T0);
}

static const bed_outputs_t *o(void)
{
    return bed_modes_outputs(bm);
}

static bool press(bed_button_t b, uint32_t now)
{
    return bed_modes_button(bm, b, NIGHT, now);
}

static void expect_no_cmd(void)
{
    bed_cmd_t c;
    TEST_ASSERT_FALSE_MESSAGE(bed_modes_take_cmd(bm, &c), "no (further) command expected");
}

static bed_cmd_t take(bed_cmd_type_t type)
{
    bed_cmd_t c;
    memset(&c, 0xEE, sizeof(c));
    TEST_ASSERT_TRUE_MESSAGE(bed_modes_take_cmd(bm, &c), "a command was expected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(type, c.type, "command type");
    return c;
}

static void expect_scene(bed_scene_t scene, uint32_t fade_ms)
{
    const bed_cmd_t c = take(BED_CMD_SCENE);
    TEST_ASSERT_EQUAL_INT(scene, c.scene);
    TEST_ASSERT_EQUAL_UINT32(fade_ms, c.fade_ms);
}

static void expect_play(bed_mode_t folder, bool resume, bool keep, uint32_t fade_ms)
{
    const bed_cmd_t c = take(BED_CMD_PLAY);
    TEST_ASSERT_EQUAL_INT_MESSAGE(folder, c.folder, "PLAY folder");
    TEST_ASSERT_EQUAL_MESSAGE(resume, c.resume, "PLAY resume");
    TEST_ASSERT_EQUAL_MESSAGE(keep, c.keep, "PLAY keep");
    TEST_ASSERT_EQUAL_UINT32(fade_ms, c.fade_ms);
}

static void expect_stop(uint32_t fade_ms)
{
    const bed_cmd_t c = take(BED_CMD_STOP);
    TEST_ASSERT_EQUAL_UINT32(fade_ms, c.fade_ms);
}

static void drain(void)
{
    bed_cmd_t c;
    while (bed_modes_take_cmd(bm, &c)) {
    }
}

static void expect_state(bed_mode_t mode, bool fading, bool candles)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(mode, o()->mode, "mode");
    TEST_ASSERT_EQUAL_MESSAGE(fading, o()->fading, "fading");
    TEST_ASSERT_EQUAL_MESSAGE(candles, o()->candles, "candles");
}

/* ------------------------------------------------------------------------ */
/* Configuration and start                                                  */
/* ------------------------------------------------------------------------ */

static void test_defaults(void)
{
    bed_modes_config_t d;
    memset(&d, 0, sizeof(d));
    bed_modes_config_default(&d);
    TEST_ASSERT_EQUAL_UINT32(20 * MIN, d.med_ms);
    TEST_ASSERT_EQUAL_UINT32(30000, d.med_fade_ms);
    TEST_ASSERT_EQUAL_UINT32(30 * MIN, d.sleep_ms);
    TEST_ASSERT_EQUAL_UINT32(120000, d.sleep_fade_ms);
    TEST_ASSERT_EQUAL_UINT32(5 * MIN, d.sleep_candle_ms);
    TEST_ASSERT_EQUAL_UINT32(3000, d.start_fade_ms);
    TEST_ASSERT_EQUAL_UINT32(2000, d.stop_fade_ms);
}

static void test_create_rejects_invalid_config(void)
{
    TEST_ASSERT_NULL(bed_modes_create(NULL, BED_NIGHT_UNKNOWN, T0));
    bed_modes_config_t bad;
#define REJECT(field, value)                                                        \
    do {                                                                            \
        bad = cfg;                                                                  \
        bad.field = (value);                                                        \
        TEST_ASSERT_NULL_MESSAGE(bed_modes_create(&bad, BED_NIGHT_UNKNOWN, T0), #field); \
    } while (0)
    REJECT(med_ms, 59999);
    REJECT(med_ms, 10800001);
    REJECT(med_fade_ms, 600001);
    REJECT(sleep_ms, 14400001);
    REJECT(sleep_fade_ms, 600001);
    REJECT(sleep_candle_ms, 7200001);
    REJECT(start_fade_ms, 10001);
    REJECT(stop_fade_ms, 10001);
#undef REJECT
    /* the limits themselves are fine */
    bad = cfg;
    bad.med_ms = 60000;
    bad.sleep_ms = 0;
    bad.sleep_candle_ms = 0;
    bad.med_fade_ms = 0;
    bad.sleep_fade_ms = 0;
    bad.start_fade_ms = 0;
    bad.stop_fade_ms = 0;
    bed_modes_t *ok = bed_modes_create(&bad, BED_NIGHT_UNKNOWN, T0);
    TEST_ASSERT_NOT_NULL(ok);
    bed_modes_destroy(ok);
    bad = cfg;
    bad.med_ms = 10800000;
    bad.sleep_ms = 14400000;
    bad.sleep_candle_ms = 7200000;
    bad.med_fade_ms = 600000;
    bad.sleep_fade_ms = 600000;
    bad.start_fade_ms = 10000;
    bad.stop_fade_ms = 10000;
    ok = bed_modes_create(&bad, BED_NIGHT_UNKNOWN, T0);
    TEST_ASSERT_NOT_NULL(ok);
    bed_modes_destroy(ok);
    bed_modes_destroy(NULL);
}

static void test_starts_idle(void)
{
    create_at(1234, T0);
    expect_state(BED_MODE_NONE, false, false);
    expect_no_cmd();
    TEST_ASSERT_EQUAL_UINT32(1234, bed_modes_sleep_night(bm));
    bed_modes_tick(bm, T0 + 10 * 60 * MIN);
    expect_state(BED_MODE_NONE, false, false);
    expect_no_cmd();
}

/* ------------------------------------------------------------------------ */
/* Meditation                                                               */
/* ------------------------------------------------------------------------ */

static void test_meditation_starts_scene_music_candles(void)
{
    create();
    TEST_ASSERT_TRUE(press(BED_BTN_MEDITATION, T0));
    expect_scene(BED_SCENE_MEDITATION, 3000);
    expect_play(BED_MODE_MEDITATION, false, false, 3000);
    expect_no_cmd();
    expect_state(BED_MODE_MEDITATION, false, true);
}

static void test_meditation_timer_fades_to_normal_scene(void)
{
    create();
    press(BED_BTN_MEDITATION, T0);
    drain();
    bed_modes_tick(bm, T0 + 20 * MIN - 1);
    expect_no_cmd();
    expect_state(BED_MODE_MEDITATION, false, true);

    bed_modes_tick(bm, T0 + 20 * MIN);
    expect_stop(30000);
    expect_scene(BED_SCENE_NORMAL, 30000);
    expect_no_cmd();
    expect_state(BED_MODE_MEDITATION, true, true); /* candles stay on during the fade */

    bed_modes_tick(bm, T0 + 20 * MIN + 30000 - 1);
    expect_state(BED_MODE_MEDITATION, true, true);
    bed_modes_tick(bm, T0 + 20 * MIN + 30000);
    expect_state(BED_MODE_NONE, false, false);
    expect_no_cmd();
}

static void test_meditation_without_fade_ends_at_once(void)
{
    cfg.med_fade_ms = 0;
    create();
    press(BED_BTN_MEDITATION, T0);
    drain();
    bed_modes_tick(bm, T0 + 20 * MIN);
    expect_stop(0);
    expect_scene(BED_SCENE_NORMAL, 0);
    expect_state(BED_MODE_NONE, false, false);
}

static void test_meditation_press_again_restarts_timer(void)
{
    create();
    press(BED_BTN_MEDITATION, T0);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_MEDITATION, T0 + 15 * MIN));
    expect_no_cmd();
    bed_modes_tick(bm, T0 + 20 * MIN);
    expect_no_cmd();
    expect_state(BED_MODE_MEDITATION, false, true);
    bed_modes_tick(bm, T0 + 35 * MIN);
    expect_stop(30000);
}

static void test_meditation_press_during_fade_starts_over(void)
{
    create();
    press(BED_BTN_MEDITATION, T0);
    bed_modes_tick(bm, T0 + 20 * MIN + 10000);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_MEDITATION, T0 + 20 * MIN + 10000));
    expect_scene(BED_SCENE_MEDITATION, 3000);
    expect_play(BED_MODE_MEDITATION, false, false, 3000);
    expect_state(BED_MODE_MEDITATION, false, true);
    /* the old fade's end does nothing */
    bed_modes_tick(bm, T0 + 20 * MIN + 30000);
    expect_state(BED_MODE_MEDITATION, false, true);
    expect_no_cmd();
}

static void test_late_tick_runs_timer_and_fade(void)
{
    create();
    press(BED_BTN_MEDITATION, T0);
    drain();
    bed_modes_tick(bm, T0 + 2 * 60 * MIN); /* long after timer and fade */
    expect_stop(30000);
    expect_scene(BED_SCENE_NORMAL, 30000);
    expect_no_cmd();
    expect_state(BED_MODE_NONE, false, false);
}

/* ------------------------------------------------------------------------ */
/* Mood                                                                     */
/* ------------------------------------------------------------------------ */

static void test_mood_starts_and_runs_until_stop(void)
{
    create();
    TEST_ASSERT_TRUE(press(BED_BTN_MOOD, T0));
    expect_scene(BED_SCENE_MOOD, 3000);
    expect_play(BED_MODE_MOOD, false, false, 3000);
    expect_no_cmd();
    expect_state(BED_MODE_MOOD, false, true);
    bed_modes_tick(bm, T0 + 10 * 60 * MIN);
    expect_no_cmd();
    expect_state(BED_MODE_MOOD, false, true);

    TEST_ASSERT_TRUE(press(BED_BTN_STOP, T0 + 10 * 60 * MIN));
    expect_stop(2000);
    expect_no_cmd();
    expect_state(BED_MODE_NONE, false, false);
}

static void test_mood_press_again_cycles_folder(void)
{
    create();
    press(BED_BTN_MOOD, T0);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_MOOD, T0 + MIN));
    take(BED_CMD_MOOD_FOLDER_NEXT);
    expect_play(BED_MODE_MOOD, false, false, 3000);
    expect_no_cmd(); /* no scene again */
    expect_state(BED_MODE_MOOD, false, true);
}

/* ------------------------------------------------------------------------ */
/* Sleep                                                                    */
/* ------------------------------------------------------------------------ */

static void test_first_sleep_press_of_night(void)
{
    create();
    TEST_ASSERT_TRUE(press(BED_BTN_SLEEP, T0));
    expect_scene(BED_SCENE_SLEEP, 3000);
    expect_play(BED_MODE_SLEEP, false, true, 3000);
    expect_no_cmd();
    expect_state(BED_MODE_SLEEP, false, true);
    TEST_ASSERT_EQUAL_UINT32(NIGHT, bed_modes_sleep_night(bm));
}

static void test_sleep_candles_go_off_after_candle_time(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    bed_modes_tick(bm, T0 + 5 * MIN - 1);
    expect_state(BED_MODE_SLEEP, false, true);
    bed_modes_tick(bm, T0 + 5 * MIN);
    expect_state(BED_MODE_SLEEP, false, false);
    expect_no_cmd();
}

static void test_sleep_timer_fades_out_scene_stays(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    bed_modes_tick(bm, T0 + 30 * MIN - 1);
    expect_no_cmd();
    bed_modes_tick(bm, T0 + 30 * MIN);
    expect_stop(120000);
    expect_no_cmd(); /* no scene change */
    expect_state(BED_MODE_SLEEP, true, false);
    bed_modes_tick(bm, T0 + 32 * MIN);
    expect_state(BED_MODE_NONE, false, false);
    expect_no_cmd();
}

static void test_sleep_without_fade_ends_at_timer(void)
{
    cfg.sleep_fade_ms = 0;
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    bed_modes_tick(bm, T0 + 30 * MIN);
    expect_stop(0);
    expect_state(BED_MODE_NONE, false, false);
}

static void test_sleep_candles_outlast_short_sleep_timer(void)
{
    cfg.sleep_ms = 2 * MIN;
    cfg.sleep_fade_ms = 0;
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    bed_modes_tick(bm, T0 + 2 * MIN);
    expect_stop(0);
    expect_state(BED_MODE_NONE, false, true); /* candle timer runs on its own */
    TEST_ASSERT_FALSE(press(BED_BTN_NEXT, T0 + 3 * MIN));
    bed_modes_tick(bm, T0 + 5 * MIN);
    expect_state(BED_MODE_NONE, false, false);
    expect_no_cmd();
}

static void test_sleep_timer_zero_plays_until_stop(void)
{
    cfg.sleep_ms = 0;
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    bed_modes_tick(bm, T0 + 4 * 60 * MIN);
    expect_no_cmd();
    expect_state(BED_MODE_SLEEP, false, false);
}

static void test_sleep_press_while_playing_restarts_timer_only(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_SLEEP, T0 + 3 * MIN));
    expect_no_cmd();
    expect_state(BED_MODE_SLEEP, false, true);
    /* candle timer keeps its first start */
    bed_modes_tick(bm, T0 + 5 * MIN);
    expect_state(BED_MODE_SLEEP, false, false);
    /* music timer runs from the second press */
    bed_modes_tick(bm, T0 + 33 * MIN - 1);
    expect_no_cmd();
    bed_modes_tick(bm, T0 + 33 * MIN);
    expect_stop(120000);
}

static void test_sleep_press_during_fade_resumes(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    bed_modes_tick(bm, T0 + 31 * MIN);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_SLEEP, T0 + 31 * MIN));
    expect_play(BED_MODE_SLEEP, true, true, 3000);
    expect_no_cmd();
    expect_state(BED_MODE_SLEEP, false, false);
    /* the old fade end does nothing; the new timer runs 30 min */
    bed_modes_tick(bm, T0 + 32 * MIN);
    expect_state(BED_MODE_SLEEP, false, false);
    bed_modes_tick(bm, T0 + 61 * MIN);
    expect_stop(120000);
}

static void test_second_sleep_of_night_continues(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    bed_modes_tick(bm, T0 + 40 * MIN); /* timer, fade and candles over */
    drain();
    expect_state(BED_MODE_NONE, false, false);

    TEST_ASSERT_TRUE(press(BED_BTN_SLEEP, T0 + 3 * 60 * MIN));
    expect_play(BED_MODE_SLEEP, true, true, 3000);
    expect_no_cmd(); /* no scene */
    expect_state(BED_MODE_SLEEP, false, false); /* no candles */
    bed_modes_tick(bm, T0 + 3 * 60 * MIN + 30 * MIN);
    expect_stop(120000);
}

static void test_next_night_starts_fresh(void)
{
    create();
    bed_modes_button(bm, BED_BTN_SLEEP, NIGHT, T0);
    bed_modes_tick(bm, T0 + 40 * MIN);
    drain();
    TEST_ASSERT_TRUE(bed_modes_button(bm, BED_BTN_SLEEP, NIGHT + 1, T0 + 60 * MIN));
    expect_scene(BED_SCENE_SLEEP, 3000);
    expect_play(BED_MODE_SLEEP, false, true, 3000);
    expect_state(BED_MODE_SLEEP, false, true);
    TEST_ASSERT_EQUAL_UINT32(NIGHT + 1, bed_modes_sleep_night(bm));
}

static void test_saved_night_survives_reboot(void)
{
    create_at(NIGHT, T0);
    TEST_ASSERT_TRUE(press(BED_BTN_SLEEP, T0));
    expect_play(BED_MODE_SLEEP, true, true, 3000);
    expect_no_cmd();
    expect_state(BED_MODE_SLEEP, false, false);
}

static void test_continuation_turns_off_candles_of_other_mode(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    bed_modes_tick(bm, T0 + 40 * MIN);
    press(BED_BTN_MOOD, T0 + 50 * MIN);
    drain();
    expect_state(BED_MODE_MOOD, false, true);
    TEST_ASSERT_TRUE(press(BED_BTN_SLEEP, T0 + 60 * MIN));
    expect_play(BED_MODE_SLEEP, true, true, 3000);
    expect_no_cmd();
    expect_state(BED_MODE_SLEEP, false, false);
}

static void test_stop_keeps_the_night(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    press(BED_BTN_STOP, T0 + MIN);
    expect_stop(2000);
    expect_state(BED_MODE_NONE, false, false);
    TEST_ASSERT_TRUE(press(BED_BTN_SLEEP, T0 + 2 * MIN));
    expect_play(BED_MODE_SLEEP, true, true, 3000);
    expect_no_cmd();
}

static void test_unknown_night_uses_twelve_hours(void)
{
    create();
    TEST_ASSERT_TRUE(bed_modes_button(bm, BED_BTN_SLEEP, BED_NIGHT_UNKNOWN, T0));
    expect_scene(BED_SCENE_SLEEP, 3000);
    expect_play(BED_MODE_SLEEP, false, true, 3000);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(BED_NIGHT_UNKNOWN, bed_modes_sleep_night(bm), "unknown night is not stored");
    bed_modes_button(bm, BED_BTN_STOP, BED_NIGHT_UNKNOWN, T0 + MIN);
    drain();

    TEST_ASSERT_TRUE(bed_modes_button(bm, BED_BTN_SLEEP, BED_NIGHT_UNKNOWN, T0 + 12 * 60 * MIN - 1));
    expect_play(BED_MODE_SLEEP, true, true, 3000);
    expect_no_cmd();
    bed_modes_button(bm, BED_BTN_STOP, BED_NIGHT_UNKNOWN, T0 + 12 * 60 * MIN - 1);
    drain();

    TEST_ASSERT_TRUE(bed_modes_button(bm, BED_BTN_SLEEP, BED_NIGHT_UNKNOWN, T0 + 12 * 60 * MIN));
    expect_scene(BED_SCENE_SLEEP, 3000);
    expect_play(BED_MODE_SLEEP, false, true, 3000);
}

static void test_unknown_night_without_first_press_is_first(void)
{
    create_at(NIGHT, T0);
    TEST_ASSERT_TRUE(bed_modes_button(bm, BED_BTN_SLEEP, BED_NIGHT_UNKNOWN, T0));
    expect_scene(BED_SCENE_SLEEP, 3000);
    expect_play(BED_MODE_SLEEP, false, true, 3000);
    TEST_ASSERT_EQUAL_UINT32(NIGHT, bed_modes_sleep_night(bm));
}

static void test_known_night_after_unknown_first_press(void)
{
    create();
    bed_modes_button(bm, BED_BTN_SLEEP, BED_NIGHT_UNKNOWN, T0);
    bed_modes_button(bm, BED_BTN_STOP, BED_NIGHT_UNKNOWN, T0 + MIN);
    drain();
    /* the clock got set: no night stored yet, so this is a first press */
    TEST_ASSERT_TRUE(bed_modes_button(bm, BED_BTN_SLEEP, NIGHT, T0 + 2 * MIN));
    expect_scene(BED_SCENE_SLEEP, 3000);
    expect_play(BED_MODE_SLEEP, false, true, 3000);
    TEST_ASSERT_EQUAL_UINT32(NIGHT, bed_modes_sleep_night(bm));
}

static void test_sleep_candle_time_zero(void)
{
    cfg.sleep_candle_ms = 0;
    create();
    press(BED_BTN_MOOD, T0);
    drain();
    press(BED_BTN_SLEEP, T0 + MIN);
    expect_scene(BED_SCENE_SLEEP, 3000);
    expect_play(BED_MODE_SLEEP, false, true, 3000);
    expect_state(BED_MODE_SLEEP, false, false);
}

/* ------------------------------------------------------------------------ */
/* Newest button wins                                                       */
/* ------------------------------------------------------------------------ */

static void test_mood_replaces_meditation(void)
{
    create();
    press(BED_BTN_MEDITATION, T0);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_MOOD, T0 + MIN));
    expect_scene(BED_SCENE_MOOD, 3000);
    expect_play(BED_MODE_MOOD, false, false, 3000);
    expect_no_cmd();
    expect_state(BED_MODE_MOOD, false, true);
    /* the meditation timer is gone */
    bed_modes_tick(bm, T0 + 2 * 60 * MIN);
    expect_no_cmd();
    expect_state(BED_MODE_MOOD, false, true);
}

static void test_meditation_replaces_sleep_and_keeps_candles_on(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_MEDITATION, T0 + MIN));
    expect_scene(BED_SCENE_MEDITATION, 3000);
    expect_play(BED_MODE_MEDITATION, false, false, 3000);
    expect_state(BED_MODE_MEDITATION, false, true);
    /* the sleep candle timer is dropped: candles stay on for meditation */
    bed_modes_tick(bm, T0 + 10 * MIN);
    expect_state(BED_MODE_MEDITATION, false, true);
    expect_no_cmd();
}

static void test_mode_during_meditation_fade_cancels_it(void)
{
    create();
    press(BED_BTN_MEDITATION, T0);
    bed_modes_tick(bm, T0 + 20 * MIN + 5000);
    drain();
    press(BED_BTN_MOOD, T0 + 20 * MIN + 5000);
    drain();
    bed_modes_tick(bm, T0 + 25 * MIN);
    expect_state(BED_MODE_MOOD, false, true);
}

/* ------------------------------------------------------------------------ */
/* Next and stop                                                            */
/* ------------------------------------------------------------------------ */

static void test_next_in_every_mode(void)
{
    create();
    TEST_ASSERT_FALSE_MESSAGE(press(BED_BTN_NEXT, T0), "nothing plays");
    expect_no_cmd();
    const bed_button_t modes[] = {BED_BTN_MEDITATION, BED_BTN_MOOD, BED_BTN_SLEEP};
    for (int i = 0; i < 3; i++) {
        press(modes[i], T0 + (uint32_t)i * MIN);
        drain();
        TEST_ASSERT_TRUE(press(BED_BTN_NEXT, T0 + (uint32_t)i * MIN + 1));
        take(BED_CMD_NEXT);
        expect_no_cmd();
    }
    /* also during a closing fade */
    bed_modes_tick(bm, T0 + 2 * MIN + 30 * MIN + 1000);
    drain();
    TEST_ASSERT_TRUE(o()->fading);
    TEST_ASSERT_TRUE(press(BED_BTN_NEXT, T0 + 2 * MIN + 30 * MIN + 1000));
    take(BED_CMD_NEXT);
}

static void test_stop_leaves_hue_alone(void)
{
    create();
    press(BED_BTN_MEDITATION, T0);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_STOP, T0 + MIN));
    expect_stop(2000);
    expect_no_cmd(); /* no scene command */
    expect_state(BED_MODE_NONE, false, false);
    bed_modes_tick(bm, T0 + 2 * 60 * MIN);
    expect_no_cmd();
}

static void test_stop_during_fade(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    bed_modes_tick(bm, T0 + 31 * MIN);
    drain();
    TEST_ASSERT_TRUE(press(BED_BTN_STOP, T0 + 31 * MIN));
    expect_stop(2000);
    expect_state(BED_MODE_NONE, false, false);
}

static void test_stop_with_only_candles_on(void)
{
    cfg.sleep_ms = MIN;
    cfg.sleep_fade_ms = 0;
    create();
    press(BED_BTN_SLEEP, T0);
    bed_modes_tick(bm, T0 + 2 * MIN);
    drain();
    expect_state(BED_MODE_NONE, false, true);
    TEST_ASSERT_TRUE(press(BED_BTN_STOP, T0 + 2 * MIN));
    expect_no_cmd(); /* nothing plays: no STOP to the speaker */
    expect_state(BED_MODE_NONE, false, false);
    TEST_ASSERT_FALSE(press(BED_BTN_STOP, T0 + 3 * MIN));
}

static void test_unknown_button_is_ignored(void)
{
    create();
    TEST_ASSERT_FALSE(press((bed_button_t)17, T0));
    expect_no_cmd();
    expect_state(BED_MODE_NONE, false, false);
}

/* ------------------------------------------------------------------------ */
/* Alarm                                                                    */
/* ------------------------------------------------------------------------ */

static void test_alarm_ends_mode_and_blocks_buttons(void)
{
    create();
    press(BED_BTN_MOOD, T0);
    drain();
    bed_modes_alarm(bm, true, T0 + MIN);
    expect_no_cmd();
    expect_state(BED_MODE_NONE, false, false);

    const bed_button_t all[] = {BED_BTN_MEDITATION, BED_BTN_MOOD, BED_BTN_SLEEP, BED_BTN_NEXT, BED_BTN_STOP};
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_FALSE(press(all[i], T0 + 2 * MIN));
    }
    expect_no_cmd();
    expect_state(BED_MODE_NONE, false, false);

    bed_modes_alarm(bm, false, T0 + 3 * MIN);
    expect_no_cmd();
    TEST_ASSERT_TRUE(press(BED_BTN_MEDITATION, T0 + 4 * MIN));
    expect_scene(BED_SCENE_MEDITATION, 3000);
}

static void test_alarm_drops_sleep_timers(void)
{
    create();
    press(BED_BTN_SLEEP, T0);
    drain();
    bed_modes_alarm(bm, true, T0 + MIN);
    bed_modes_alarm(bm, false, T0 + 2 * MIN);
    bed_modes_tick(bm, T0 + 60 * MIN);
    expect_no_cmd(); /* no STOP from the old sleep timer */
    expect_state(BED_MODE_NONE, false, false);
}

static void test_repeated_alarm_value_changes_nothing(void)
{
    create();
    bed_modes_alarm(bm, false, T0);
    press(BED_BTN_MOOD, T0);
    drain();
    bed_modes_alarm(bm, false, T0 + MIN);
    expect_state(BED_MODE_MOOD, false, true);
    expect_no_cmd();
}

/* ------------------------------------------------------------------------ */
/* Queue and clock                                                          */
/* ------------------------------------------------------------------------ */

static void test_queue_drops_oldest_when_full(void)
{
    create();
    press(BED_BTN_MOOD, T0); /* SCENE, PLAY */
    for (int i = 0; i < BED_CMDS; i++) {
        press(BED_BTN_NEXT, T0 + 1); /* NEXT x8 */
    }
    for (int i = 0; i < BED_CMDS; i++) {
        take(BED_CMD_NEXT);
    }
    expect_no_cmd();
}

static void test_timers_work_across_counter_wrap(void)
{
    const uint32_t t = 0xFFFFFFFFu - 10 * MIN;
    create_at(BED_NIGHT_UNKNOWN, t);
    press(BED_BTN_SLEEP, t);
    drain();
    bed_modes_tick(bm, t + 5 * MIN);
    expect_state(BED_MODE_SLEEP, false, false);
    bed_modes_tick(bm, t + 30 * MIN - 1); /* wrapped */
    expect_no_cmd();
    bed_modes_tick(bm, t + 30 * MIN);
    expect_stop(120000);
}

static void test_null_arguments(void)
{
    bed_modes_t *none = NULL;
    TEST_ASSERT_FALSE(bed_modes_button(none, BED_BTN_MOOD, NIGHT, T0));
    bed_modes_alarm(none, true, T0);
    bed_modes_tick(none, T0);
    create();
    TEST_ASSERT_FALSE(bed_modes_take_cmd(bm, NULL));
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static void test_night_id(void)
{
    const int64_t day = 86400;
    const int64_t d = 20000 * day; /* 00:00 local of some day */
    TEST_ASSERT_EQUAL_UINT32(19999, bed_night_id(d));             /* midnight belongs to yesterday's night */
    TEST_ASSERT_EQUAL_UINT32(19999, bed_night_id(d + 43199));     /* 11:59:59 */
    TEST_ASSERT_EQUAL_UINT32(20000, bed_night_id(d + 43200));     /* 12:00 starts a new night */
    TEST_ASSERT_EQUAL_UINT32(20000, bed_night_id(d + day + 43199));
    TEST_ASSERT_EQUAL_UINT32(20001, bed_night_id(d + day + 43200));
    TEST_ASSERT_EQUAL_UINT32(0, bed_night_id(0));
    TEST_ASSERT_EQUAL_UINT32(0, bed_night_id(-5));
    TEST_ASSERT_EQUAL_UINT32(0, bed_night_id(43200));
}

static void test_next_folder(void)
{
    const char *names[] = {"sleep", "mood_rain", "Mood_Chill", "meditation", NULL, "mood_jazz", "moodx"};
    const size_t n = sizeof(names) / sizeof(names[0]);
    char out[24];
    TEST_ASSERT_TRUE(bed_next_folder(names, n, "", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Mood_Chill", out);
    TEST_ASSERT_TRUE(bed_next_folder(names, n, "Mood_Chill", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mood_jazz", out);
    TEST_ASSERT_TRUE(bed_next_folder(names, n, "mood_jazz", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mood_rain", out);
    TEST_ASSERT_TRUE(bed_next_folder(names, n, "mood_rain", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Mood_Chill", out, "wraps round");
    /* current no longer on the card: the one after it */
    TEST_ASSERT_TRUE(bed_next_folder(names, n, "mood_disco", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mood_jazz", out);
    TEST_ASSERT_TRUE(bed_next_folder(names, n, "zzz", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Mood_Chill", out);
    /* case-insensitive order, ties by strcmp */
    const char *tie[] = {"mood_a", "MOOD_A"};
    TEST_ASSERT_TRUE(bed_next_folder(tie, 2, "", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("MOOD_A", out);
    TEST_ASSERT_TRUE(bed_next_folder(tie, 2, "MOOD_A", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mood_a", out);
    /* only one: it comes back */
    const char *one[] = {"mood_only"};
    TEST_ASSERT_TRUE(bed_next_folder(one, 1, "mood_only", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mood_only", out);
}

static void test_next_folder_failures(void)
{
    const char *names[] = {"sleep", "mood_rain"};
    char out[24] = "x";
    TEST_ASSERT_FALSE(bed_next_folder(names, 1, "", "mood_", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    strcpy(out, "x");
    TEST_ASSERT_FALSE_MESSAGE(bed_next_folder(names, 2, "", "mood_", out, 9), "mood_rain needs 10 bytes");
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_TRUE(bed_next_folder(names, 2, "", "mood_", out, 10));
    TEST_ASSERT_FALSE(bed_next_folder(NULL, 2, "", "mood_", out, sizeof(out)));
    TEST_ASSERT_FALSE(bed_next_folder(names, 2, NULL, "mood_", out, sizeof(out)));
    TEST_ASSERT_FALSE(bed_next_folder(names, 2, "", NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(bed_next_folder(names, 2, "", "mood_", NULL, sizeof(out)));
    TEST_ASSERT_FALSE(bed_next_folder(names, 0, "", "mood_", out, sizeof(out)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_create_rejects_invalid_config);
    RUN_TEST(test_starts_idle);
    RUN_TEST(test_meditation_starts_scene_music_candles);
    RUN_TEST(test_meditation_timer_fades_to_normal_scene);
    RUN_TEST(test_meditation_without_fade_ends_at_once);
    RUN_TEST(test_meditation_press_again_restarts_timer);
    RUN_TEST(test_meditation_press_during_fade_starts_over);
    RUN_TEST(test_late_tick_runs_timer_and_fade);
    RUN_TEST(test_mood_starts_and_runs_until_stop);
    RUN_TEST(test_mood_press_again_cycles_folder);
    RUN_TEST(test_first_sleep_press_of_night);
    RUN_TEST(test_sleep_candles_go_off_after_candle_time);
    RUN_TEST(test_sleep_timer_fades_out_scene_stays);
    RUN_TEST(test_sleep_without_fade_ends_at_timer);
    RUN_TEST(test_sleep_candles_outlast_short_sleep_timer);
    RUN_TEST(test_sleep_timer_zero_plays_until_stop);
    RUN_TEST(test_sleep_press_while_playing_restarts_timer_only);
    RUN_TEST(test_sleep_press_during_fade_resumes);
    RUN_TEST(test_second_sleep_of_night_continues);
    RUN_TEST(test_next_night_starts_fresh);
    RUN_TEST(test_saved_night_survives_reboot);
    RUN_TEST(test_continuation_turns_off_candles_of_other_mode);
    RUN_TEST(test_stop_keeps_the_night);
    RUN_TEST(test_unknown_night_uses_twelve_hours);
    RUN_TEST(test_unknown_night_without_first_press_is_first);
    RUN_TEST(test_known_night_after_unknown_first_press);
    RUN_TEST(test_sleep_candle_time_zero);
    RUN_TEST(test_mood_replaces_meditation);
    RUN_TEST(test_meditation_replaces_sleep_and_keeps_candles_on);
    RUN_TEST(test_mode_during_meditation_fade_cancels_it);
    RUN_TEST(test_next_in_every_mode);
    RUN_TEST(test_stop_leaves_hue_alone);
    RUN_TEST(test_stop_during_fade);
    RUN_TEST(test_stop_with_only_candles_on);
    RUN_TEST(test_unknown_button_is_ignored);
    RUN_TEST(test_alarm_ends_mode_and_blocks_buttons);
    RUN_TEST(test_alarm_drops_sleep_timers);
    RUN_TEST(test_repeated_alarm_value_changes_nothing);
    RUN_TEST(test_queue_drops_oldest_when_full);
    RUN_TEST(test_timers_work_across_counter_wrap);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_night_id);
    RUN_TEST(test_next_folder);
    RUN_TEST(test_next_folder_failures);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
