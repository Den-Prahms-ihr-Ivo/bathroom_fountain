/*
 * Tests for lib/speaker_session – requirements BED-44 ... BED-48, BED-52.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "speaker_session.h"

#define T0 1000u

static speaker_session_t *s;

void setUp(void)
{
    s = speaker_session_create(12, T0);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, "speaker_session_create returned NULL");
}

void tearDown(void)
{
    speaker_session_destroy(s);
    s = NULL;
}

static link_audio_t msg(uint8_t cmd, uint8_t flags, uint8_t volume, uint16_t fade_ds, const char *folder)
{
    link_audio_t m;
    memset(&m, 0, sizeof(m));
    m.cmd = cmd;
    m.flags = flags;
    m.volume = volume;
    m.fade_ds = fade_ds;
    if (folder) {
        strncpy(m.folder, folder, LINK_FOLDER_LEN);
    }
    return m;
}

static bool play(const char *folder, uint8_t flags, uint8_t volume, uint16_t fade_ds, uint32_t now)
{
    link_audio_t m = msg(LINK_AUDIO_PLAY, flags, volume, fade_ds, folder);
    return speaker_session_audio(s, &m, now);
}

static bool stop(uint16_t fade_ds, uint32_t now)
{
    link_audio_t m = msg(LINK_AUDIO_STOP, 0, 0, fade_ds, NULL);
    return speaker_session_audio(s, &m, now);
}

static const spk_outputs_t *o(void)
{
    return speaker_session_outputs(s);
}

static void expect_act(spk_act_type_t type)
{
    spk_act_t a;
    memset(&a, 0xEE, sizeof(a));
    TEST_ASSERT_TRUE_MESSAGE(speaker_session_take(s, &a), "an action was expected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(type, a.type, "action type");
    if (type != SPK_ACT_START) {
        TEST_ASSERT_EQUAL_STRING("", a.folder);
    }
}

static void expect_start(const char *folder, bool resume)
{
    spk_act_t a;
    memset(&a, 0xEE, sizeof(a));
    TEST_ASSERT_TRUE_MESSAGE(speaker_session_take(s, &a), "START expected");
    TEST_ASSERT_EQUAL_INT(SPK_ACT_START, a.type);
    TEST_ASSERT_EQUAL_STRING(folder, a.folder);
    TEST_ASSERT_EQUAL(resume, a.resume);
}

static void expect_none(void)
{
    spk_act_t a;
    TEST_ASSERT_FALSE_MESSAGE(speaker_session_take(s, &a), "no (further) action expected");
}

static void drain(void)
{
    spk_act_t a;
    while (speaker_session_take(s, &a)) {
    }
}

static void expect_state(spk_state_t st, const char *folder, bool kept)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(st, o()->state, "state");
    TEST_ASSERT_EQUAL_STRING(folder, o()->folder);
    TEST_ASSERT_EQUAL_MESSAGE(kept, o()->kept, "kept");
    TEST_ASSERT_EQUAL_MESSAGE(st == SPK_PLAYING || st == SPK_FADING_OUT, o()->amp_on, "amp_on");
}

/* ---- start ------------------------------------------------------------- */

static void test_starts_idle(void)
{
    expect_state(SPK_IDLE, "", false);
    TEST_ASSERT_EQUAL_UINT8(12, o()->volume);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, speaker_session_level(s, T0));
    expect_none();
    speaker_session_destroy(s);
    s = speaker_session_create(99, T0);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(32, o()->volume, "clamped");
    speaker_session_destroy(NULL);
    TEST_ASSERT_NULL(speaker_session_outputs(NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, speaker_session_level(NULL, T0));
}

/* ---- play -------------------------------------------------------------- */

static void test_play_starts_and_fades_in(void)
{
    TEST_ASSERT_TRUE(play("mood_rain", 0, 14, 30, T0));
    expect_start("mood_rain", false);
    expect_none();
    expect_state(SPK_PLAYING, "mood_rain", false);
    TEST_ASSERT_EQUAL_UINT8(14, o()->volume);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, speaker_session_level(s, T0));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, speaker_session_level(s, T0 + 1500));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, speaker_session_level(s, T0 + 3000));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, speaker_session_level(s, T0 + 60000));
}

static void test_play_without_fade_is_full_at_once(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, speaker_session_level(s, T0));
    expect_state(SPK_PLAYING, "sleep", true);
}

static void test_play_resume_flag_is_passed_on(void)
{
    play("sleep", LINK_AUDIO_FLAG_RESUME | LINK_AUDIO_FLAG_KEEP, 8, 30, T0);
    expect_start("sleep", true);
    expect_state(SPK_PLAYING, "sleep", true);
}

static void test_play_refused(void)
{
    TEST_ASSERT_FALSE_MESSAGE(play("", 0, 10, 0, T0), "empty folder");
    TEST_ASSERT_FALSE_MESSAGE(play("true crime", 0, 10, 0, T0), "invalid folder");
    TEST_ASSERT_FALSE_MESSAGE(play("sleep", 0, 33, 0, T0), "volume above 32");
    TEST_ASSERT_FALSE(speaker_session_audio(s, NULL, T0));
    TEST_ASSERT_FALSE(speaker_session_audio(NULL, NULL, T0));
    link_audio_t m = msg(9, 0, 0, 0, "sleep");
    TEST_ASSERT_FALSE_MESSAGE(speaker_session_audio(s, &m, T0), "unknown cmd");
    expect_none();
    expect_state(SPK_IDLE, "", false);
}

static void test_play_replaces_unkept_session_without_save(void)
{
    play("mood_rain", 0, 14, 30, T0);
    drain();
    TEST_ASSERT_TRUE(play("meditation", 0, 10, 30, T0 + 5000));
    expect_start("meditation", false);
    expect_none();
    expect_state(SPK_PLAYING, "meditation", false);
    TEST_ASSERT_EQUAL_UINT8(10, o()->volume);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, speaker_session_level(s, T0 + 5000));
}

static void test_play_replacing_kept_session_saves_first(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 30, T0);
    drain();
    TEST_ASSERT_TRUE(play("mood_rain", 0, 14, 30, T0 + 5000));
    expect_act(SPK_ACT_SAVE);
    expect_start("mood_rain", false);
    expect_none();
    expect_state(SPK_PLAYING, "mood_rain", false);
}

static void test_fresh_play_of_same_kept_folder_saves(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 30, T0);
    drain();
    TEST_ASSERT_TRUE(play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 30, T0 + 5000));
    expect_act(SPK_ACT_SAVE);
    expect_start("sleep", false);
    expect_none();
}

/* ---- stop -------------------------------------------------------------- */

static void test_stop_fades_out_then_halts(void)
{
    play("mood_rain", 0, 14, 0, T0);
    drain();
    TEST_ASSERT_TRUE(stop(20, T0 + 1000));
    expect_none(); /* not kept: no SAVE; HALT comes at the end */
    expect_state(SPK_FADING_OUT, "mood_rain", false);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, speaker_session_level(s, T0 + 2000));
    speaker_session_tick(s, T0 + 2999);
    expect_none();
    expect_state(SPK_FADING_OUT, "mood_rain", false);
    speaker_session_tick(s, T0 + 3000);
    expect_act(SPK_ACT_HALT);
    expect_none();
    expect_state(SPK_IDLE, "", false);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, speaker_session_level(s, T0 + 3000));
}

static void test_stop_of_kept_session_saves_at_once(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    drain();
    TEST_ASSERT_TRUE(stop(1200, T0 + 1000));
    expect_act(SPK_ACT_SAVE); /* before the fade */
    expect_none();
    expect_state(SPK_FADING_OUT, "sleep", true);
    speaker_session_tick(s, T0 + 1000 + 120000);
    expect_act(SPK_ACT_HALT);
    expect_none(); /* no second SAVE */
}

static void test_stop_without_fade(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    drain();
    TEST_ASSERT_TRUE(stop(0, T0 + 10));
    expect_act(SPK_ACT_SAVE);
    expect_act(SPK_ACT_HALT);
    expect_none();
    expect_state(SPK_IDLE, "", false);
}

static void test_stop_during_fade_in_starts_from_current_level(void)
{
    play("mood_rain", 0, 14, 40, T0); /* 4 s fade-in */
    drain();
    stop(10, T0 + 2000); /* at 0.5 */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, speaker_session_level(s, T0 + 2000));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, speaker_session_level(s, T0 + 2500));
    speaker_session_tick(s, T0 + 3000);
    expect_act(SPK_ACT_HALT);
}

static void test_second_stop_restarts_fade(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    stop(100, T0); /* 10 s */
    drain();
    TEST_ASSERT_TRUE(stop(10, T0 + 5000)); /* from 0.5 over 1 s */
    expect_none(); /* no second SAVE */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f, speaker_session_level(s, T0 + 5500));
    speaker_session_tick(s, T0 + 6000);
    expect_act(SPK_ACT_HALT);
    expect_state(SPK_IDLE, "", false);

    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0 + 7000);
    stop(100, T0 + 7000);
    drain();
    TEST_ASSERT_TRUE(stop(0, T0 + 8000));
    expect_act(SPK_ACT_HALT);
    expect_none();
    expect_state(SPK_IDLE, "", false);
}

static void test_stop_when_idle_is_ignored(void)
{
    TEST_ASSERT_FALSE(stop(20, T0));
    expect_none();
}

/* ---- fade cancel (sleep pressed during its fade-out) ------------------- */

static void test_resume_of_same_folder_cancels_fade(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    stop(100, T0 + 1000); /* 10 s fade */
    drain();
    TEST_ASSERT_TRUE(play("sleep", LINK_AUDIO_FLAG_RESUME | LINK_AUDIO_FLAG_KEEP, 9, 20, T0 + 6000)); /* at 0.5 */
    expect_none(); /* no START, no SAVE: it plays on */
    expect_state(SPK_PLAYING, "sleep", true);
    TEST_ASSERT_EQUAL_UINT8(9, o()->volume);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.5f, speaker_session_level(s, T0 + 6000));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.75f, speaker_session_level(s, T0 + 7000));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, speaker_session_level(s, T0 + 8000));
    speaker_session_tick(s, T0 + 20000);
    expect_none(); /* the old fade's end does nothing */
    expect_state(SPK_PLAYING, "sleep", true);
}

static void test_resume_of_same_folder_while_playing(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    drain();
    TEST_ASSERT_TRUE(play("sleep", LINK_AUDIO_FLAG_RESUME | LINK_AUDIO_FLAG_KEEP, 8, 30, T0 + 100));
    expect_none();
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, speaker_session_level(s, T0 + 100));
}

static void test_resume_of_other_folder_during_fade_starts_it(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    stop(100, T0 + 1000);
    drain();
    TEST_ASSERT_TRUE(play("mood_rain", LINK_AUDIO_FLAG_RESUME, 14, 30, T0 + 2000));
    expect_start("mood_rain", true); /* no SAVE: saved at the STOP */
    expect_none();
    expect_state(SPK_PLAYING, "mood_rain", false);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, speaker_session_level(s, T0 + 2000));
}

/* ---- next and volume --------------------------------------------------- */

static void test_next(void)
{
    link_audio_t n = msg(LINK_AUDIO_NEXT, 0, 0, 0, NULL);
    TEST_ASSERT_FALSE(speaker_session_audio(s, &n, T0));
    expect_none();
    play("mood_rain", 0, 14, 0, T0);
    drain();
    TEST_ASSERT_TRUE(speaker_session_audio(s, &n, T0 + 1));
    expect_act(SPK_ACT_NEXT);
    stop(50, T0 + 2);
    TEST_ASSERT_TRUE_MESSAGE(speaker_session_audio(s, &n, T0 + 3), "also while fading out");
    expect_act(SPK_ACT_NEXT);
    expect_none();
}

static void test_volume_in_every_state(void)
{
    link_audio_t v = msg(LINK_AUDIO_VOLUME, 0, 20, 0, NULL);
    TEST_ASSERT_TRUE(speaker_session_audio(s, &v, T0));
    TEST_ASSERT_EQUAL_UINT8(20, o()->volume);
    play("mood_rain", 0, 14, 0, T0);
    v.volume = 3;
    TEST_ASSERT_TRUE(speaker_session_audio(s, &v, T0 + 1));
    TEST_ASSERT_EQUAL_UINT8(3, o()->volume);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f, speaker_session_level(s, T0 + 1), "volume does not touch the fade");
    speaker_session_ring(s, true, T0 + 2);
    v.volume = 32;
    TEST_ASSERT_TRUE(speaker_session_audio(s, &v, T0 + 3));
    TEST_ASSERT_EQUAL_UINT8(32, o()->volume);
    v.volume = 33;
    TEST_ASSERT_FALSE(speaker_session_audio(s, &v, T0 + 4));
    TEST_ASSERT_EQUAL_UINT8(32, o()->volume);
}

/* ---- alarm, standby, ended --------------------------------------------- */

static void test_ring_stops_kept_session_with_save(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    drain();
    speaker_session_ring(s, true, T0 + 100);
    expect_act(SPK_ACT_SAVE);
    expect_act(SPK_ACT_HALT);
    expect_none();
    expect_state(SPK_ALARM, "", false);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, speaker_session_level(s, T0 + 100));
    speaker_session_ring(s, true, T0 + 200);
    expect_none();
}

static void test_ring_during_fade_halts_without_save(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    stop(100, T0 + 10);
    drain();
    speaker_session_ring(s, true, T0 + 100);
    expect_act(SPK_ACT_HALT);
    expect_none();
    speaker_session_tick(s, T0 + 60000);
    expect_none();
    expect_state(SPK_ALARM, "", false);
}

static void test_ring_when_idle(void)
{
    speaker_session_ring(s, false, T0);
    expect_state(SPK_IDLE, "", false);
    speaker_session_ring(s, true, T0);
    expect_none();
    expect_state(SPK_ALARM, "", false);
}

static void test_audio_ignored_while_ringing(void)
{
    speaker_session_ring(s, true, T0);
    TEST_ASSERT_FALSE(play("mood_rain", 0, 14, 0, T0 + 1));
    TEST_ASSERT_FALSE(stop(0, T0 + 2));
    link_audio_t n = msg(LINK_AUDIO_NEXT, 0, 0, 0, NULL);
    TEST_ASSERT_FALSE(speaker_session_audio(s, &n, T0 + 3));
    expect_none();
    expect_state(SPK_ALARM, "", false);
    TEST_ASSERT_FALSE(speaker_session_standby(s, T0 + 4));
    TEST_ASSERT_FALSE(speaker_session_ended(s, T0 + 5));

    speaker_session_ring(s, false, T0 + 10);
    expect_none();
    expect_state(SPK_IDLE, "", false);
    TEST_ASSERT_TRUE(play("mood_rain", 0, 14, 0, T0 + 11));
    expect_start("mood_rain", false);
}

static void test_standby(void)
{
    TEST_ASSERT_FALSE(speaker_session_standby(s, T0));
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    drain();
    TEST_ASSERT_TRUE(speaker_session_standby(s, T0 + 1));
    expect_act(SPK_ACT_SAVE);
    expect_act(SPK_ACT_HALT);
    expect_none();
    expect_state(SPK_IDLE, "", false);

    play("mood_rain", 0, 14, 0, T0 + 2);
    stop(50, T0 + 3);
    drain();
    TEST_ASSERT_TRUE(speaker_session_standby(s, T0 + 4));
    expect_act(SPK_ACT_HALT);
    expect_none();
}

static void test_ended(void)
{
    TEST_ASSERT_FALSE(speaker_session_ended(s, T0));
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    drain();
    TEST_ASSERT_TRUE(speaker_session_ended(s, T0 + 1));
    expect_none(); /* nothing to save or halt */
    expect_state(SPK_IDLE, "", false);
    TEST_ASSERT_FALSE(stop(10, T0 + 2));
}

/* ---- clock and queue --------------------------------------------------- */

static void test_late_call_finishes_fade_first(void)
{
    play("mood_rain", 0, 14, 0, T0);
    stop(10, T0);
    drain();
    /* the fade ended long ago; a STOP now finds the speaker idle */
    TEST_ASSERT_FALSE(stop(10, T0 + 50000));
    expect_act(SPK_ACT_HALT);
    expect_none();
    expect_state(SPK_IDLE, "", false);
}

static void test_fade_across_counter_wrap(void)
{
    const uint32_t t = 0xFFFFFFFFu - 500u;
    play("mood_rain", 0, 14, 10, t); /* 1 s fade-in across the wrap */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f, speaker_session_level(s, t + 500u));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, speaker_session_level(s, t + 1000u));
    drain();
    stop(10, t + 1000u);
    speaker_session_tick(s, t + 1999u);
    expect_none();
    speaker_session_tick(s, t + 2000u);
    expect_act(SPK_ACT_HALT);
}

static void test_long_fade(void)
{
    play("sleep", LINK_AUDIO_FLAG_KEEP, 8, 0, T0);
    stop(65535, T0); /* 6553.5 s */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.5f, speaker_session_level(s, T0 + 3276750u));
}

static void test_queue_drops_oldest(void)
{
    play("mood_rain", 0, 14, 0, T0); /* START */
    link_audio_t n = msg(LINK_AUDIO_NEXT, 0, 0, 0, NULL);
    for (int i = 0; i < SPK_ACTS; i++) {
        speaker_session_audio(s, &n, T0);
    }
    for (int i = 0; i < SPK_ACTS; i++) {
        expect_act(SPK_ACT_NEXT);
    }
    expect_none();
    TEST_ASSERT_FALSE(speaker_session_take(s, NULL));
    TEST_ASSERT_FALSE(speaker_session_take(NULL, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_idle);
    RUN_TEST(test_play_starts_and_fades_in);
    RUN_TEST(test_play_without_fade_is_full_at_once);
    RUN_TEST(test_play_resume_flag_is_passed_on);
    RUN_TEST(test_play_refused);
    RUN_TEST(test_play_replaces_unkept_session_without_save);
    RUN_TEST(test_play_replacing_kept_session_saves_first);
    RUN_TEST(test_fresh_play_of_same_kept_folder_saves);
    RUN_TEST(test_stop_fades_out_then_halts);
    RUN_TEST(test_stop_of_kept_session_saves_at_once);
    RUN_TEST(test_stop_without_fade);
    RUN_TEST(test_stop_during_fade_in_starts_from_current_level);
    RUN_TEST(test_second_stop_restarts_fade);
    RUN_TEST(test_stop_when_idle_is_ignored);
    RUN_TEST(test_resume_of_same_folder_cancels_fade);
    RUN_TEST(test_resume_of_same_folder_while_playing);
    RUN_TEST(test_resume_of_other_folder_during_fade_starts_it);
    RUN_TEST(test_next);
    RUN_TEST(test_volume_in_every_state);
    RUN_TEST(test_ring_stops_kept_session_with_save);
    RUN_TEST(test_ring_during_fade_halts_without_save);
    RUN_TEST(test_ring_when_idle);
    RUN_TEST(test_audio_ignored_while_ringing);
    RUN_TEST(test_standby);
    RUN_TEST(test_ended);
    RUN_TEST(test_late_call_finishes_fade_first);
    RUN_TEST(test_fade_across_counter_wrap);
    RUN_TEST(test_long_fade);
    RUN_TEST(test_queue_drops_oldest);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
