/*
 * Tests for lib/cold_protocol – requirements COLD-01 ... COLD-08, COLD-13.
 * The helper ticks every 10 ms and logs every cue, scene and end with the
 * time it appeared, relative to the start of the run.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "cold_protocol.h"

#define T0 1000000u
#define STEP 10u
#define MAX_LOG 32

typedef struct {
    uint32_t t;
    cold_cue_t cue;
} cue_log_t;

typedef struct {
    uint32_t t;
    cold_scene_t scene;
} scene_log_t;

static cold_config_t cfg;
static cold_profile_t prof;
static cold_protocol_t *cp;
static uint32_t now;
static uint32_t t0;
static uint32_t clips[COLD_CUE_COUNT];

static cue_log_t cues[MAX_LOG];
static int n_cues;
static scene_log_t scenes[MAX_LOG];
static int n_scenes;
static cold_end_t end_reason;
static uint32_t end_time;
static int n_ends;

void setUp(void)
{
    cold_config_default(&cfg);
    cfg.warning_lead_ms = 20000;
    cold_profile_default(&prof);
    prof.warm_s = 120;
    prof.cold_s = 90;
    prof.after_s = 300;
    prof.voice = true;
    prof.music = true;
    prof.calm = true;
    clips[COLD_CUE_GO] = 3200;
    clips[COLD_CUE_WARNING] = 1800;
    clips[COLD_CUE_TO_COLD] = 3500;
    clips[COLD_CUE_DONE] = 2900;
    cp = NULL;
    now = T0;
    t0 = T0;
    n_cues = n_scenes = n_ends = 0;
    end_time = 0;
}

void tearDown(void)
{
    cold_protocol_destroy(cp);
    cp = NULL;
}

static void create(void)
{
    cp = cold_protocol_create(&cfg, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(cp, "cold_protocol_create returned NULL for a valid config");
}

static void collect(void)
{
    cold_cue_t c;
    cold_scene_t s;
    cold_end_t e;
    if (cold_protocol_take_cue(cp, &c)) {
        TEST_ASSERT_TRUE(n_cues < MAX_LOG);
        cues[n_cues].t = now - t0;
        cues[n_cues].cue = c;
        n_cues++;
        TEST_ASSERT_FALSE_MESSAGE(cold_protocol_take_cue(cp, &c), "a cue is taken once");
    }
    if (cold_protocol_take_scene(cp, &s)) {
        TEST_ASSERT_TRUE(n_scenes < MAX_LOG);
        scenes[n_scenes].t = now - t0;
        scenes[n_scenes].scene = s;
        n_scenes++;
        TEST_ASSERT_FALSE_MESSAGE(cold_protocol_take_scene(cp, &s), "a scene is taken once");
    }
    if (cold_protocol_take_end(cp, &e)) {
        end_reason = e;
        end_time = now - t0;
        n_ends++;
        TEST_ASSERT_FALSE_MESSAGE(cold_protocol_take_end(cp, &e), "the end is taken once");
    }
}

static void start(void)
{
    t0 = now;
    TEST_ASSERT_TRUE(cold_protocol_start(cp, &prof, clips, now));
    collect();
}

/* Tick in 10 ms steps until `rel` ms after the start of the run. */
static void run_to(uint32_t rel)
{
    while (now - t0 < rel) {
        now += STEP;
        cold_protocol_tick(cp, now);
        collect();
    }
}

static cold_phase_t phase(void)
{
    return cold_protocol_outputs(cp)->phase;
}

static void expect_cue(int i, uint32_t t, cold_cue_id_t id, bool beep)
{
    TEST_ASSERT_TRUE_MESSAGE(i < n_cues, "cue missing");
    TEST_ASSERT_EQUAL_UINT32(t, cues[i].t);
    TEST_ASSERT_EQUAL_INT(id, cues[i].cue.id);
    TEST_ASSERT_EQUAL_INT(beep, cues[i].cue.beep);
}

static void expect_scene(int i, uint32_t t, cold_scene_t scene)
{
    TEST_ASSERT_TRUE_MESSAGE(i < n_scenes, "scene missing");
    TEST_ASSERT_EQUAL_UINT32(t, scenes[i].t);
    TEST_ASSERT_EQUAL_INT(scene, scenes[i].scene);
}

/* ------------------------------------------------------------------------ */

static void test_defaults_and_validation(void)
{
    cold_config_t c;
    memset(&c, 0, sizeof(c));
    cold_config_default(&c);
    TEST_ASSERT_EQUAL_UINT32(20000, c.warning_lead_ms);
    cold_profile_t p;
    memset(&p, 0, sizeof(p));
    cold_profile_default(&p);
    TEST_ASSERT_EQUAL_UINT16(120, p.warm_s);
    TEST_ASSERT_EQUAL_UINT16(90, p.cold_s);
    TEST_ASSERT_EQUAL_UINT16(300, p.after_s);
    TEST_ASSERT_TRUE(p.voice);
    TEST_ASSERT_TRUE(p.music);
    TEST_ASSERT_TRUE(p.calm);
    TEST_ASSERT_TRUE(cold_profile_valid(&p));
    TEST_ASSERT_FALSE(cold_profile_valid(NULL));

    p.warm_s = 0;
    TEST_ASSERT_TRUE(cold_profile_valid(&p));
    p.warm_s = 1800;
    TEST_ASSERT_TRUE(cold_profile_valid(&p));
    p.warm_s = 1801;
    TEST_ASSERT_FALSE(cold_profile_valid(&p));
    p.warm_s = 120;
    p.cold_s = 14;
    TEST_ASSERT_FALSE(cold_profile_valid(&p));
    p.cold_s = 15;
    TEST_ASSERT_TRUE(cold_profile_valid(&p));
    p.cold_s = 601;
    TEST_ASSERT_FALSE(cold_profile_valid(&p));
    p.cold_s = 600;
    p.after_s = 29;
    TEST_ASSERT_FALSE(cold_profile_valid(&p));
    p.after_s = 1801;
    TEST_ASSERT_FALSE(cold_profile_valid(&p));
    p.after_s = 1800;
    TEST_ASSERT_TRUE(cold_profile_valid(&p));

    create();
    TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase());
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_NONE, cold_protocol_outputs(cp)->music);
    TEST_ASSERT_EQUAL_UINT32(0, cold_protocol_outputs(cp)->remaining_ms);
    c.warning_lead_ms = 4999;
    TEST_ASSERT_NULL(cold_protocol_create(&c, now));
    c.warning_lead_ms = 60001;
    TEST_ASSERT_NULL(cold_protocol_create(&c, now));
    TEST_ASSERT_NULL(cold_protocol_create(NULL, now));
    cold_protocol_destroy(NULL);
}

static void test_countdown_then_warm_begins_when_the_clip_ends(void)
{
    create();
    start();
    TEST_ASSERT_EQUAL_INT(COLD_COUNTDOWN, phase());
    TEST_ASSERT_EQUAL_UINT32(3200, cold_protocol_outputs(cp)->remaining_ms);
    expect_cue(0, 0, COLD_CUE_GO, false);
    expect_scene(0, 0, COLD_SCENE_WARM); /* the lights answer the press */
    run_to(3190);
    TEST_ASSERT_EQUAL_INT(COLD_COUNTDOWN, phase());
    TEST_ASSERT_EQUAL_UINT32(10, cold_protocol_outputs(cp)->remaining_ms);
    run_to(3200);
    TEST_ASSERT_EQUAL_INT(COLD_WARM, phase());
    TEST_ASSERT_EQUAL_UINT32(120000, cold_protocol_outputs(cp)->remaining_ms);
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_NONE, cold_protocol_outputs(cp)->music);
    TEST_ASSERT_EQUAL_INT(1, n_cues);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n_scenes, "WARM was already issued at start");
}

static void test_full_run_timeline(void)
{
    create();
    start();
    const uint32_t warm = 3200;
    const uint32_t cold = warm + 120000;
    const uint32_t after = cold + 90000;
    const uint32_t end = after + 300000;

    run_to(cold - 10);
    TEST_ASSERT_EQUAL_INT(COLD_WARM, phase());
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_NONE, cold_protocol_outputs(cp)->music);
    run_to(cold);
    TEST_ASSERT_EQUAL_INT(COLD_COLD, phase());
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_HYPE, cold_protocol_outputs(cp)->music);
    TEST_ASSERT_EQUAL_UINT32(90000, cold_protocol_outputs(cp)->remaining_ms);
    run_to(after - 10);
    TEST_ASSERT_EQUAL_INT(COLD_COLD, phase());
    run_to(after);
    TEST_ASSERT_EQUAL_INT(COLD_AFTER, phase());
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_CALM, cold_protocol_outputs(cp)->music);
    run_to(end - 10);
    TEST_ASSERT_EQUAL_INT(COLD_AFTER, phase());
    TEST_ASSERT_EQUAL_UINT32(10, cold_protocol_outputs(cp)->remaining_ms);
    TEST_ASSERT_EQUAL_INT(0, n_ends);
    run_to(end + 5000);
    TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase());
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_NONE, cold_protocol_outputs(cp)->music);
    TEST_ASSERT_EQUAL_UINT32(0, cold_protocol_outputs(cp)->remaining_ms);
    TEST_ASSERT_EQUAL_INT(1, n_ends);
    TEST_ASSERT_EQUAL_INT(COLD_END_FINISHED, end_reason);
    TEST_ASSERT_EQUAL_UINT32(end, end_time);

    TEST_ASSERT_EQUAL_INT(5, n_cues);
    expect_cue(0, 0, COLD_CUE_GO, false);
    expect_cue(1, cold - 20000, COLD_CUE_WARNING, false);
    expect_cue(2, cold - 3500, COLD_CUE_TO_COLD, false); /* ends exactly on the change */
    expect_cue(3, after - 20000, COLD_CUE_WARNING, false);
    expect_cue(4, after - 2900, COLD_CUE_DONE, false);

    TEST_ASSERT_EQUAL_INT(3, n_scenes);
    expect_scene(0, 0, COLD_SCENE_WARM);
    expect_scene(1, cold, COLD_SCENE_COLD);
    expect_scene(2, after, COLD_SCENE_AFTER);
}

static void test_warning_is_skipped_when_the_phase_is_too_short(void)
{
    create();
    prof.warm_s = 0;
    prof.calm = false;
    prof.cold_s = 21; /* 21.0 s is not longer than 20 s + 1.8 s */
    start();
    run_to(40000);
    TEST_ASSERT_EQUAL_INT(2, n_cues);
    expect_cue(1, 3200 + 21000 - 2900, COLD_CUE_DONE, false);

    tearDown();
    setUp();
    create();
    prof.warm_s = 0;
    prof.calm = false;
    prof.cold_s = 22;
    start();
    run_to(40000);
    TEST_ASSERT_EQUAL_INT(3, n_cues);
    expect_cue(1, 3200 + 2000, COLD_CUE_WARNING, false);
    expect_cue(2, 3200 + 22000 - 2900, COLD_CUE_DONE, false);

    /* warning and final cue would overlap: 12 s + 9 s > 20 s */
    tearDown();
    setUp();
    create();
    prof.warm_s = 0;
    prof.calm = false;
    prof.cold_s = 60;
    clips[COLD_CUE_WARNING] = 12000;
    clips[COLD_CUE_DONE] = 9000;
    start();
    run_to(80000);
    TEST_ASSERT_EQUAL_INT(2, n_cues);
    expect_cue(1, 3200 + 60000 - 9000, COLD_CUE_DONE, false);
}

static void test_voice_off_gives_beeps_at_the_same_moments(void)
{
    create();
    prof.voice = false;
    start();
    TEST_ASSERT_EQUAL_INT_MESSAGE(COLD_WARM, phase(), "a beep has no countdown");
    TEST_ASSERT_EQUAL_UINT32(120000, cold_protocol_outputs(cp)->remaining_ms);
    expect_cue(0, 0, COLD_CUE_GO, true);
    expect_scene(0, 0, COLD_SCENE_WARM);
    run_to(120000 + 90000 + 1000);
    TEST_ASSERT_EQUAL_INT(COLD_AFTER, phase());
    TEST_ASSERT_EQUAL_INT(5, n_cues);
    expect_cue(1, 100000, COLD_CUE_WARNING, true);
    expect_cue(2, 120000, COLD_CUE_TO_COLD, true);
    expect_cue(3, 190000, COLD_CUE_WARNING, true);
    expect_cue(4, 210000, COLD_CUE_DONE, true);
}

static void test_missing_or_too_long_clip_becomes_a_beep(void)
{
    create();
    clips[COLD_CUE_TO_COLD] = 0;  /* file missing */
    clips[COLD_CUE_DONE] = 31000; /* longer than the cold phase */
    prof.warm_s = 60;
    prof.cold_s = 30;
    start();
    run_to(3200 + 60000 + 30000 + 100);
    TEST_ASSERT_EQUAL_INT(5, n_cues);
    expect_cue(0, 0, COLD_CUE_GO, false);
    expect_cue(1, 3200 + 40000, COLD_CUE_WARNING, false);
    expect_cue(2, 3200 + 60000, COLD_CUE_TO_COLD, true);
    expect_cue(3, 3200 + 60000 + 10000, COLD_CUE_WARNING, false);
    expect_cue(4, 3200 + 60000 + 30000, COLD_CUE_DONE, true);

    /* no clips at all */
    tearDown();
    setUp();
    create();
    t0 = now;
    TEST_ASSERT_TRUE(cold_protocol_start(cp, &prof, NULL, now));
    collect();
    expect_cue(0, 0, COLD_CUE_GO, true);
    TEST_ASSERT_EQUAL_INT(COLD_WARM, phase());
}

static void test_warm_zero_goes_straight_to_cold(void)
{
    create();
    prof.warm_s = 0;
    start();
    TEST_ASSERT_EQUAL_INT(COLD_COUNTDOWN, phase());
    expect_scene(0, 0, COLD_SCENE_COLD);
    run_to(3200);
    TEST_ASSERT_EQUAL_INT(COLD_COLD, phase());
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_HYPE, cold_protocol_outputs(cp)->music);
    run_to(3200 + 90000);
    TEST_ASSERT_EQUAL_INT(COLD_AFTER, phase());
    TEST_ASSERT_EQUAL_INT(3, n_cues); /* GO, WARNING, DONE - no TO_COLD */
    expect_cue(1, 3200 + 70000, COLD_CUE_WARNING, false);
    expect_cue(2, 3200 + 90000 - 2900, COLD_CUE_DONE, false);
    TEST_ASSERT_EQUAL_INT(2, n_scenes);
}

static void test_switches_for_music_and_calm_phase(void)
{
    create();
    prof.music = false;
    prof.calm = false;
    start();
    run_to(3200 + 120000 + 50);
    TEST_ASSERT_EQUAL_INT(COLD_COLD, phase());
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_NONE, cold_protocol_outputs(cp)->music);
    run_to(3200 + 120000 + 90000);
    TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase());
    TEST_ASSERT_EQUAL_INT(1, n_ends);
    TEST_ASSERT_EQUAL_INT(COLD_END_FINISHED, end_reason);
    TEST_ASSERT_EQUAL_UINT32(3200 + 120000 + 90000, end_time);
    TEST_ASSERT_EQUAL_INT(2, n_scenes); /* WARM, COLD */
}

static void test_next_skips_to_the_following_phase(void)
{
    create();
    TEST_ASSERT_FALSE(cold_protocol_next(cp, now));
    start();
    run_to(1000);
    TEST_ASSERT_TRUE(cold_protocol_next(cp, now)); /* countdown -> warm */
    collect();
    TEST_ASSERT_EQUAL_INT(COLD_WARM, phase());
    TEST_ASSERT_EQUAL_UINT32(120000, cold_protocol_outputs(cp)->remaining_ms);
    expect_cue(1, 1000, COLD_CUE_GO, true);

    run_to(5000);
    TEST_ASSERT_TRUE(cold_protocol_next(cp, now)); /* warm -> cold */
    collect();
    TEST_ASSERT_EQUAL_INT(COLD_COLD, phase());
    TEST_ASSERT_EQUAL_UINT32(90000, cold_protocol_outputs(cp)->remaining_ms);
    expect_cue(2, 5000, COLD_CUE_TO_COLD, true);
    expect_scene(1, 5000, COLD_SCENE_COLD);

    run_to(9000);
    TEST_ASSERT_TRUE(cold_protocol_next(cp, now)); /* cold -> after */
    collect();
    TEST_ASSERT_EQUAL_INT(COLD_AFTER, phase());
    expect_cue(3, 9000, COLD_CUE_DONE, true);
    expect_scene(2, 9000, COLD_SCENE_AFTER);

    run_to(12000);
    TEST_ASSERT_TRUE(cold_protocol_next(cp, now)); /* after -> end */
    collect();
    TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase());
    TEST_ASSERT_EQUAL_INT(4, n_cues);
    TEST_ASSERT_EQUAL_INT(1, n_ends);
    TEST_ASSERT_EQUAL_INT(COLD_END_FINISHED, end_reason);
    TEST_ASSERT_FALSE(cold_protocol_next(cp, now));

    /* the skipped phase's own cues never appear */
    run_to(400000);
    TEST_ASSERT_EQUAL_INT(4, n_cues);
    TEST_ASSERT_EQUAL_INT(1, n_ends);
}

static void test_abort_from_every_phase(void)
{
    const uint32_t at[] = {1000, 3200 + 50000, 3200 + 120000 + 88000, 3200 + 120000 + 90000 + 1000};
    const cold_phase_t expected[] = {COLD_COUNTDOWN, COLD_WARM, COLD_COLD, COLD_AFTER};
    for (int i = 0; i < 4; i++) {
        setUp();
        create();
        TEST_ASSERT_FALSE(cold_protocol_abort(cp, now));
        start();
        run_to(at[i] - 10);
        now += STEP; /* the abort itself brings the state up to date: a cue becomes due and is dropped */
        TEST_ASSERT_EQUAL_INT(expected[i], phase());
        TEST_ASSERT_TRUE(cold_protocol_abort(cp, now));
        TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase());
        TEST_ASSERT_EQUAL_INT(COLD_MUSIC_NONE, cold_protocol_outputs(cp)->music);
        const int before = n_cues;
        const int scenes_before = n_scenes;
        collect();
        TEST_ASSERT_EQUAL_INT_MESSAGE(before, n_cues, "abort drops a pending cue");
        TEST_ASSERT_EQUAL_INT_MESSAGE(scenes_before, n_scenes, "abort drops a pending scene");
        TEST_ASSERT_EQUAL_INT(1, n_ends);
        TEST_ASSERT_EQUAL_INT(COLD_END_ABORTED, end_reason);
        run_to(at[i] + 500000);
        TEST_ASSERT_EQUAL_INT(before, n_cues);
        TEST_ASSERT_EQUAL_INT(1, n_ends);
        TEST_ASSERT_FALSE(cold_protocol_abort(cp, now));
        tearDown();
    }
}

static void test_profile_change_applies_at_the_next_phase_change(void)
{
    create();
    cold_profile_t zen = prof;
    zen.cold_s = 45;
    zen.music = false;
    zen.voice = false;
    zen.calm = false;
    TEST_ASSERT_FALSE_MESSAGE(cold_protocol_set_profile(cp, &zen), "idle: the profile comes with start()");
    start();
    run_to(3200 + 30000);
    TEST_ASSERT_TRUE(cold_protocol_set_profile(cp, &zen));
    cold_profile_t bad = zen;
    bad.cold_s = 5;
    TEST_ASSERT_FALSE(cold_protocol_set_profile(cp, &bad));
    TEST_ASSERT_EQUAL_UINT32(90000, cold_protocol_outputs(cp)->remaining_ms); /* warm is unchanged */
    run_to(3200 + 120000);
    /* the running phase kept its plan: voice cues as before */
    expect_cue(1, 3200 + 100000, COLD_CUE_WARNING, false);
    expect_cue(2, 3200 + 120000 - 3500, COLD_CUE_TO_COLD, false);
    TEST_ASSERT_EQUAL_INT(COLD_COLD, phase());
    TEST_ASSERT_EQUAL_UINT32(45000, cold_protocol_outputs(cp)->remaining_ms);
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_NONE, cold_protocol_outputs(cp)->music);
    run_to(3200 + 120000 + 45000);
    expect_cue(3, 3200 + 120000 + 25000, COLD_CUE_WARNING, true);
    expect_cue(4, 3200 + 120000 + 45000, COLD_CUE_DONE, true);
    TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase()); /* calm is off in the new profile */
    TEST_ASSERT_EQUAL_INT(COLD_END_FINISHED, end_reason);
}

static void test_start_is_refused_while_running_or_with_a_bad_profile(void)
{
    create();
    cold_profile_t bad = prof;
    bad.cold_s = 601;
    TEST_ASSERT_FALSE(cold_protocol_start(cp, &bad, clips, now));
    TEST_ASSERT_FALSE(cold_protocol_start(cp, NULL, clips, now));
    TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase());
    start();
    run_to(2000);
    TEST_ASSERT_FALSE(cold_protocol_start(cp, &prof, clips, now));
    TEST_ASSERT_EQUAL_UINT32(1200, cold_protocol_outputs(cp)->remaining_ms);
    run_to(60000);
    TEST_ASSERT_FALSE(cold_protocol_start(cp, &prof, clips, now));
    TEST_ASSERT_TRUE(cold_protocol_abort(cp, now));
    /* a new run drops the untaken end of the old one */
    n_cues = n_scenes = n_ends = 0;
    start();
    TEST_ASSERT_EQUAL_INT(0, n_ends);
    TEST_ASSERT_EQUAL_INT(COLD_COUNTDOWN, phase());
    expect_cue(0, 0, COLD_CUE_GO, false);
}

static void test_late_call_catches_up_from_the_planned_times(void)
{
    create();
    start();
    now = t0 + 3200 + 120000 + 5000; /* nothing was called for two minutes */
    cold_protocol_tick(cp, now);
    TEST_ASSERT_EQUAL_INT(COLD_COLD, phase());
    TEST_ASSERT_EQUAL_UINT32(85000, cold_protocol_outputs(cp)->remaining_ms);
    TEST_ASSERT_EQUAL_INT(COLD_MUSIC_HYPE, cold_protocol_outputs(cp)->music);
    collect();
    TEST_ASSERT_EQUAL_INT(2, n_cues);
    TEST_ASSERT_EQUAL_INT_MESSAGE(COLD_CUE_TO_COLD, cues[1].cue.id, "only the latest due cue is kept");
    TEST_ASSERT_EQUAL_INT(2, n_scenes);
    TEST_ASSERT_EQUAL_INT(COLD_SCENE_COLD, scenes[1].scene);

    /* a late next() also starts from the up-to-date state */
    now = t0 + 3200 + 120000 + 90000 + 1000;
    TEST_ASSERT_TRUE(cold_protocol_next(cp, now));
    TEST_ASSERT_EQUAL_INT_MESSAGE(COLD_IDLE, phase(), "next() arrived in AFTER and ended the run");
}

static void test_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 60000u;
    create();
    start();
    run_to(3200 + 120000 + 90000 + 300000 + 100);
    TEST_ASSERT_EQUAL_INT(COLD_IDLE, phase());
    TEST_ASSERT_EQUAL_INT(5, n_cues);
    expect_cue(2, 3200 + 120000 - 3500, COLD_CUE_TO_COLD, false);
    expect_cue(4, 3200 + 120000 + 90000 - 2900, COLD_CUE_DONE, false);
    TEST_ASSERT_EQUAL_UINT32(3200 + 120000 + 90000 + 300000, end_time);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_countdown_then_warm_begins_when_the_clip_ends);
    RUN_TEST(test_full_run_timeline);
    RUN_TEST(test_warning_is_skipped_when_the_phase_is_too_short);
    RUN_TEST(test_voice_off_gives_beeps_at_the_same_moments);
    RUN_TEST(test_missing_or_too_long_clip_becomes_a_beep);
    RUN_TEST(test_warm_zero_goes_straight_to_cold);
    RUN_TEST(test_switches_for_music_and_calm_phase);
    RUN_TEST(test_next_skips_to_the_following_phase);
    RUN_TEST(test_abort_from_every_phase);
    RUN_TEST(test_profile_change_applies_at_the_next_phase_change);
    RUN_TEST(test_start_is_refused_while_running_or_with_a_bad_profile);
    RUN_TEST(test_late_call_catches_up_from_the_planned_times);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
