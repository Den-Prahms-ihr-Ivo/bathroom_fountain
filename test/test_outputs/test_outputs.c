/*
 * Tests for lib/outputs – BEH-05/06/07, AUD-05/06/07 (what to switch, in
 * which order, and retrying what failed).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "amp_volume.h"
#include "fade_curve.h"
#include "mode_controller.h"
#include "outputs.h"

static outputs_t *o;

void setUp(void)
{
    o = outputs_create();
    TEST_ASSERT_NOT_NULL(o);
}

void tearDown(void)
{
    outputs_destroy(o);
    o = NULL;
}

/* ------------------------------------------------------------- helpers --- */

static outputs_state_t W(bool candles, bool fountain, bool amp, bool music, uint8_t vol)
{
    outputs_state_t s = {candles, fountain, amp, music, vol};
    return s;
}

#define A(kind, value) ((output_action_t){(kind), (value)})

static size_t plan(const outputs_state_t *want, output_action_t *acts)
{
    memset(acts, 0xEE, sizeof(output_action_t) * OUTPUTS_MAX_ACTIONS);
    const size_t n = outputs_plan(o, want, acts);
    TEST_ASSERT_TRUE(n <= OUTPUTS_MAX_ACTIONS);
    return n;
}

static void expect_plan(outputs_state_t want, const output_action_t *expect, size_t n_expect)
{
    output_action_t acts[OUTPUTS_MAX_ACTIONS];
    const size_t n = plan(&want, acts);
    TEST_ASSERT_EQUAL_size_t(n_expect, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(expect[i].kind, acts[i].kind, "action kind / order");
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(expect[i].value, acts[i].value, "action value");
    }
}

/* Plan and confirm the first `n_ok` actions (all if n_ok < 0). Returns the plan size. */
static size_t apply(outputs_state_t want, int n_ok)
{
    output_action_t acts[OUTPUTS_MAX_ACTIONS];
    const size_t n = plan(&want, acts);
    for (size_t i = 0; i < n && (n_ok < 0 || (int)i < n_ok); i++) {
        outputs_done(o, &acts[i]);
    }
    return n;
}

static void expect_idle(outputs_state_t want)
{
    output_action_t acts[OUTPUTS_MAX_ACTIONS];
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, plan(&want, acts), "nothing left to do");
}

/* --------------------------------------------------------------- plan --- */

static void test_first_plan_sends_everything_amp_on(void)
{
    const output_action_t e[] = {A(OUTPUT_CANDLES, 1), A(OUTPUT_FOUNTAIN, 0), A(OUTPUT_AMP_POWER, 1),
                                 A(OUTPUT_VOLUME, 0x50), A(OUTPUT_MUSIC, 1)};
    expect_plan(W(1, 0, 1, 1, 0x50), e, 5);
}

static void test_first_plan_sends_everything_amp_off(void)
{
    const output_action_t e[] = {A(OUTPUT_CANDLES, 0), A(OUTPUT_FOUNTAIN, 0), A(OUTPUT_MUSIC, 0),
                                 A(OUTPUT_AMP_POWER, 0)};
    expect_plan(W(0, 0, 0, 0, 0x50), e, 4);
}

static void test_planning_alone_changes_nothing(void)
{
    const output_action_t e[] = {A(OUTPUT_CANDLES, 1), A(OUTPUT_FOUNTAIN, 1), A(OUTPUT_AMP_POWER, 1),
                                 A(OUTPUT_VOLUME, 0x40), A(OUTPUT_MUSIC, 1)};
    expect_plan(W(1, 1, 1, 1, 0x40), e, 5);
    expect_plan(W(1, 1, 1, 1, 0x40), e, 5);
}

static void test_nothing_to_do_once_confirmed(void)
{
    apply(W(1, 1, 1, 1, 0x40), -1);
    expect_idle(W(1, 1, 1, 1, 0x40));
    apply(W(0, 0, 0, 0, 0xFF), -1);
    expect_idle(W(0, 0, 0, 0, 0xFF));
}

static void test_only_changed_items_are_planned(void)
{
    apply(W(1, 1, 1, 1, 0x50), -1);

    const output_action_t vol[] = {A(OUTPUT_VOLUME, 0x52)};
    expect_plan(W(1, 1, 1, 1, 0x52), vol, 1);

    const output_action_t candles[] = {A(OUTPUT_CANDLES, 0)};
    expect_plan(W(0, 1, 1, 1, 0x50), candles, 1);

    const output_action_t fountain[] = {A(OUTPUT_FOUNTAIN, 0)};
    expect_plan(W(1, 0, 1, 1, 0x50), fountain, 1);

    const output_action_t music[] = {A(OUTPUT_MUSIC, 0)};
    expect_plan(W(1, 1, 1, 0, 0x50), music, 1);
}

static void test_volume_ignored_while_amp_off(void)
{
    apply(W(0, 0, 0, 0, 0x50), -1);
    expect_idle(W(0, 0, 0, 0, 0x60));
    expect_idle(W(0, 0, 0, 0, 0xFF));
}

static void test_music_toggles_alone_while_amp_stays_on(void)
{
    apply(W(1, 1, 1, 0, 0x50), -1);
    const output_action_t on[] = {A(OUTPUT_MUSIC, 1)};
    expect_plan(W(1, 1, 1, 1, 0x50), on, 1);
}

/* ------------------------------------------------------------ ordering --- */

static void test_entering_ambient_order(void)
{
    apply(W(0, 0, 0, 0, 0xFF), -1);
    const output_action_t e[] = {A(OUTPUT_CANDLES, 1), A(OUTPUT_FOUNTAIN, 1), A(OUTPUT_AMP_POWER, 1),
                                 A(OUTPUT_VOLUME, 0xFF), A(OUTPUT_MUSIC, 1)};
    expect_plan(W(1, 1, 1, 1, 0xFF), e, 5);
}

static void test_leaving_ambient_order(void)
{
    apply(W(1, 1, 1, 1, 0xFF), -1);
    const output_action_t e[] = {A(OUTPUT_CANDLES, 0), A(OUTPUT_FOUNTAIN, 0), A(OUTPUT_MUSIC, 0),
                                 A(OUTPUT_AMP_POWER, 0)};
    expect_plan(W(0, 0, 0, 0, 0xFF), e, 4);
}

static void test_power_on_always_resends_volume(void)
{
    apply(W(1, 1, 1, 1, 0x50), -1);
    apply(W(1, 1, 0, 1, 0x50), -1); /* amp off, music stays */
    const output_action_t e[] = {A(OUTPUT_AMP_POWER, 1), A(OUTPUT_VOLUME, 0x50)};
    expect_plan(W(1, 1, 1, 1, 0x50), e, 2);

    /* even if a volume was (wrongly) confirmed while the amplifier was off */
    apply(W(1, 1, 0, 1, 0x50), -1);
    outputs_done(o, &A(OUTPUT_VOLUME, 0x50));
    expect_plan(W(1, 1, 1, 1, 0x50), e, 2);
}

static void test_confirming_power_forgets_the_volume(void)
{
    apply(W(0, 0, 0, 0, 0xFF), -1);
    /* caller confirms in the wrong order: volume first, then power */
    outputs_done(o, &A(OUTPUT_VOLUME, 0x50));
    outputs_done(o, &A(OUTPUT_AMP_POWER, 1));
    const output_action_t e[] = {A(OUTPUT_VOLUME, 0x50)};
    expect_plan(W(0, 0, 1, 0, 0x50), e, 1);
}

/* ------------------------------------------------------------- retries --- */

static void test_unconfirmed_action_is_planned_again(void)
{
    output_action_t acts[OUTPUTS_MAX_ACTIONS];
    outputs_state_t want = W(1, 0, 1, 1, 0x50);
    const size_t n = plan(&want, acts);
    for (size_t i = 0; i < n; i++) {
        if (acts[i].kind != OUTPUT_FOUNTAIN) {
            outputs_done(o, &acts[i]);
        }
    }
    const output_action_t e[] = {A(OUTPUT_FOUNTAIN, 0)};
    expect_plan(want, e, 1);
}

static void test_failed_power_up_retries_power_volume_and_music(void)
{
    apply(W(0, 0, 0, 0, 0xFF), -1);
    TEST_ASSERT_EQUAL_size_t(5, apply(W(1, 1, 1, 1, 0x48), 2)); /* amp power-up fails */
    const output_action_t e[] = {A(OUTPUT_AMP_POWER, 1), A(OUTPUT_VOLUME, 0x48), A(OUTPUT_MUSIC, 1)};
    expect_plan(W(1, 1, 1, 1, 0x48), e, 3);
}

static void test_failed_volume_write_is_retried_alone(void)
{
    apply(W(1, 1, 1, 1, 0x50), -1);
    TEST_ASSERT_EQUAL_size_t(1, apply(W(1, 1, 1, 1, 0x60), 0));
    const output_action_t e[] = {A(OUTPUT_VOLUME, 0x60)};
    expect_plan(W(1, 1, 1, 1, 0x60), e, 1);
}

static void test_done_records_the_action_not_the_latest_want(void)
{
    apply(W(1, 1, 1, 1, 0x50), -1);
    outputs_done(o, &A(OUTPUT_VOLUME, 0x58)); /* planned earlier, want moved on */
    const output_action_t e[] = {A(OUTPUT_VOLUME, 0x5A)};
    expect_plan(W(1, 1, 1, 1, 0x5A), e, 1);
    expect_idle(W(1, 1, 1, 1, 0x58));
}

static void test_invalidate_makes_everything_unknown(void)
{
    apply(W(1, 1, 1, 1, 0x50), -1);
    outputs_invalidate(o);
    const output_action_t e[] = {A(OUTPUT_CANDLES, 1), A(OUTPUT_FOUNTAIN, 1), A(OUTPUT_AMP_POWER, 1),
                                 A(OUTPUT_VOLUME, 0x50), A(OUTPUT_MUSIC, 1)};
    expect_plan(W(1, 1, 1, 1, 0x50), e, 5);
}

static void test_null_arguments(void)
{
    output_action_t acts[OUTPUTS_MAX_ACTIONS];
    outputs_state_t want = W(1, 1, 1, 1, 0x50);
    TEST_ASSERT_EQUAL_size_t(0, outputs_plan(NULL, &want, acts));
    TEST_ASSERT_EQUAL_size_t(0, outputs_plan(o, NULL, acts));
    TEST_ASSERT_EQUAL_size_t(0, outputs_plan(o, &want, NULL));
    outputs_done(NULL, &A(OUTPUT_CANDLES, 1));
    outputs_done(o, NULL);
    outputs_invalidate(NULL);
    outputs_destroy(NULL);
    TEST_ASSERT_EQUAL_size_t(5, outputs_plan(o, &want, acts));
}

/* ------------------------------------------------- randomised invariants --- */

static int rank(output_action_t a)
{
    switch (a.kind) {
    case OUTPUT_CANDLES: return 0;
    case OUTPUT_FOUNTAIN: return 1;
    case OUTPUT_MUSIC: return a.value ? 5 : 2;
    case OUTPUT_AMP_POWER: return 3;
    case OUTPUT_VOLUME: return 4;
    }
    return 99;
}

/* A model of the real hardware: what is actually switched. */
typedef struct {
    bool candles, fountain, amp, music;
    uint8_t vol;
} hw_t;

static void hw_apply(hw_t *hw, output_action_t a)
{
    switch (a.kind) {
    case OUTPUT_CANDLES: hw->candles = a.value; break;
    case OUTPUT_FOUNTAIN: hw->fountain = a.value; break;
    case OUTPUT_MUSIC: hw->music = a.value; break;
    case OUTPUT_AMP_POWER:
        if (a.value && !hw->amp) hw->vol = 0xFF; /* fresh power-up comes up muted */
        hw->amp = a.value;
        break;
    case OUTPUT_VOLUME:
        TEST_ASSERT_TRUE_MESSAGE(hw->amp, "volume written while the amplifier is off");
        hw->vol = a.value;
        break;
    }
}

static void test_random_sequences_keep_the_rules(void)
{
    srand(2024);
    hw_t hw = {false, false, false, false, 0xFF};
    for (int round = 0; round < 5000; round++) {
        outputs_state_t want = W(rand() & 1, rand() & 1, rand() & 1, rand() & 1, (uint8_t)(0x30 + rand() % 0xD0));
        if (rand() % 50 == 0) {
            outputs_invalidate(o);
        }
        output_action_t acts[OUTPUTS_MAX_ACTIONS];
        const size_t n = plan(&want, acts);

        int music = 0, vol = 0, amp_on = 0;
        for (size_t i = 0; i < n; i++) {
            if (i > 0) TEST_ASSERT_TRUE_MESSAGE(rank(acts[i - 1]) < rank(acts[i]), "fixed order");
            if (acts[i].kind == OUTPUT_MUSIC) music++;
            if (acts[i].kind == OUTPUT_VOLUME) vol++;
            if (acts[i].kind == OUTPUT_AMP_POWER && acts[i].value) amp_on++;
            if (acts[i].kind == OUTPUT_VOLUME) TEST_ASSERT_TRUE(want.amp_on);
        }
        TEST_ASSERT_TRUE(music <= 1);
        if (amp_on) TEST_ASSERT_EQUAL_INT_MESSAGE(1, vol, "power-up must be followed by volume");

        /* the caller gets through a random prefix, as if an action failed */
        const bool all = (rand() % 3) != 0;
        const size_t n_ok = all ? n : (size_t)(rand() % (int)(n + 1));
        for (size_t i = 0; i < n_ok; i++) {
            hw_apply(&hw, acts[i]);
            outputs_done(o, &acts[i]);
        }
        if (n_ok == n) {
            TEST_ASSERT_EQUAL(want.candles, hw.candles);
            TEST_ASSERT_EQUAL(want.fountain, hw.fountain);
            TEST_ASSERT_EQUAL(want.music, hw.music);
            TEST_ASSERT_EQUAL(want.amp_on, hw.amp);
            if (want.amp_on) TEST_ASSERT_EQUAL_HEX8(want.vol_code, hw.vol);
            expect_idle(want);
        }
    }
}

/* ------------------------------------------------- want from mode outputs --- */

static mode_outputs_t M(bool on, float fade)
{
    mode_outputs_t m;
    memset(&m, 0, sizeof(m));
    m.mode = on ? AMBIENCE_AMBIENT : AMBIENCE_NORMAL;
    m.candles_on = on;
    m.fountain_on = on;
    m.amp_enabled = on;
    m.music_playing = on;
    m.fade_gain = fade;
    return m;
}

static void test_want_copies_the_switches(void)
{
    mode_outputs_t m = M(true, 1.0f);
    m.fountain_on = false;
    m.music_playing = false;
    outputs_state_t w;
    outputs_want_from_mode(&m, 1.0f, &w);
    TEST_ASSERT_TRUE(w.candles);
    TEST_ASSERT_FALSE(w.fountain);
    TEST_ASSERT_TRUE(w.amp_on);
    TEST_ASSERT_FALSE(w.music);

    m = M(false, 1.0f);
    m.candles_on = true;
    m.music_playing = true;
    outputs_want_from_mode(&m, 1.0f, &w);
    TEST_ASSERT_TRUE(w.candles);
    TEST_ASSERT_FALSE(w.fountain);
    TEST_ASSERT_FALSE(w.amp_on);
    TEST_ASSERT_TRUE(w.music);

    m = M(false, 1.0f);
    m.fountain_on = true;
    outputs_want_from_mode(&m, 1.0f, &w);
    TEST_ASSERT_FALSE(w.candles);
    TEST_ASSERT_TRUE(w.fountain);
    TEST_ASSERT_FALSE(w.amp_on);
    TEST_ASSERT_FALSE(w.music);

    m = M(false, 0.0f);
    outputs_want_from_mode(&m, 1.0f, &w);
    TEST_ASSERT_FALSE(w.candles);
    TEST_ASSERT_FALSE(w.fountain);
    TEST_ASSERT_FALSE(w.amp_on);
    TEST_ASSERT_FALSE(w.music);
}

static void test_want_volume_code(void)
{
    outputs_state_t w;
    mode_outputs_t m = M(true, 1.0f);
    outputs_want_from_mode(&m, 1.0f, &w);
    TEST_ASSERT_EQUAL_HEX8(0x30, w.vol_code);

    outputs_want_from_mode(&m, 0.5f, &w);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x3C, w.vol_code, "user volume -6 dB");

    outputs_want_from_mode(&m, 0.0f, &w);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, w.vol_code, "volume step 0 mutes");

    m.fade_gain = 0.0f;
    outputs_want_from_mode(&m, 1.0f, &w);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, w.vol_code, "fade at 0 mutes");
}

static void test_want_applies_the_fade_curve(void)
{
    outputs_state_t w;
    mode_outputs_t m = M(true, 0.5f);
    outputs_want_from_mode(&m, 1.0f, &w);
    /* halfway with the default -50 dB floor is -25 dB = 50 steps; a linear
     * fade would give -6 dB = 0x3C */
    TEST_ASSERT_EQUAL_HEX8(0x62, w.vol_code);

    outputs_want_from_mode(&m, 0.5f, &w);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x6E, w.vol_code, "-25 dB fade + -6 dB volume");

    m.fade_gain = 0.8f;
    outputs_want_from_mode(&m, 0.3f, &w);
    const uint8_t expect = amp_volume_code_for(0.3f, fade_curve_gain(0.8f, FADE_CURVE_FLOOR_DB_DEFAULT));
    TEST_ASSERT_EQUAL_HEX8(expect, w.vol_code);
}

static void test_want_tiny_fade_is_not_mute(void)
{
    outputs_state_t w;
    mode_outputs_t m = M(true, 0.001f);
    outputs_want_from_mode(&m, 1.0f, &w);
    TEST_ASSERT_TRUE(w.vol_code != 0xFF);
}

static void test_want_null_leaves_output_untouched(void)
{
    outputs_state_t w = W(1, 0, 1, 0, 0x77);
    outputs_want_from_mode(NULL, 1.0f, &w);
    TEST_ASSERT_EQUAL_HEX8(0x77, w.vol_code);
    TEST_ASSERT_TRUE(w.candles);
    mode_outputs_t m = M(true, 1.0f);
    outputs_want_from_mode(&m, 1.0f, NULL);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_first_plan_sends_everything_amp_on);
    RUN_TEST(test_first_plan_sends_everything_amp_off);
    RUN_TEST(test_planning_alone_changes_nothing);
    RUN_TEST(test_nothing_to_do_once_confirmed);
    RUN_TEST(test_only_changed_items_are_planned);
    RUN_TEST(test_volume_ignored_while_amp_off);
    RUN_TEST(test_music_toggles_alone_while_amp_stays_on);
    RUN_TEST(test_entering_ambient_order);
    RUN_TEST(test_leaving_ambient_order);
    RUN_TEST(test_power_on_always_resends_volume);
    RUN_TEST(test_confirming_power_forgets_the_volume);
    RUN_TEST(test_unconfirmed_action_is_planned_again);
    RUN_TEST(test_failed_power_up_retries_power_volume_and_music);
    RUN_TEST(test_failed_volume_write_is_retried_alone);
    RUN_TEST(test_done_records_the_action_not_the_latest_want);
    RUN_TEST(test_invalidate_makes_everything_unknown);
    RUN_TEST(test_null_arguments);
    RUN_TEST(test_random_sequences_keep_the_rules);
    RUN_TEST(test_want_copies_the_switches);
    RUN_TEST(test_want_volume_code);
    RUN_TEST(test_want_applies_the_fade_curve);
    RUN_TEST(test_want_tiny_fade_is_not_mute);
    RUN_TEST(test_want_null_leaves_output_untouched);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
