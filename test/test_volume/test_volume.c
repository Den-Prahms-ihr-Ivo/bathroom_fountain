/*
 * Tests for lib/volume – requirement AUD-05.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "volume.h"

#define T0 50000u

static volume_config_t cfg;
static volume_t *v;

void setUp(void)
{
    volume_config_default(&cfg);
    cfg.steps = 32;
    cfg.max_step = 32;
    cfg.default_step = 12;
    cfg.min_db = -48.0f;
    cfg.save_delay_ms = 2000;
    v = NULL;
}

void tearDown(void)
{
    volume_destroy(v);
    v = NULL;
}

static void create(bool has_saved, uint8_t saved)
{
    v = volume_create(&cfg, has_saved, saved);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "volume_create returned NULL for a valid config");
}

static float db_to_gain(float db)
{
    return powf(10.0f, db / 20.0f);
}

/* ------------------------------------------------------------------------ */
/* Configuration and start-up                                               */
/* ------------------------------------------------------------------------ */

static void test_defaults(void)
{
    volume_config_t d;
    memset(&d, 0xA5, sizeof(d));
    volume_config_default(&d);
    TEST_ASSERT_EQUAL_UINT8(32, d.steps);
    TEST_ASSERT_EQUAL_UINT8(32, d.max_step);
    TEST_ASSERT_EQUAL_UINT8(12, d.default_step);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -48.0f, d.min_db);
    TEST_ASSERT_EQUAL_UINT32(2000, d.save_delay_ms);
}

static void test_create_rejects_invalid_config(void)
{
    create(false, 0);

    TEST_ASSERT_NULL(volume_create(NULL, false, 0));

    volume_config_t bad = cfg;
    bad.steps = 1;
    TEST_ASSERT_NULL_MESSAGE(volume_create(&bad, false, 0), "steps < 2");

    bad = cfg;
    bad.max_step = 0;
    TEST_ASSERT_NULL_MESSAGE(volume_create(&bad, false, 0), "max_step == 0");

    bad = cfg;
    bad.max_step = 33;
    TEST_ASSERT_NULL_MESSAGE(volume_create(&bad, false, 0), "max_step > steps");

    bad = cfg;
    bad.max_step = 10;
    TEST_ASSERT_NULL_MESSAGE(volume_create(&bad, false, 0), "default_step > max_step");

    bad = cfg;
    bad.min_db = 0.0f;
    TEST_ASSERT_NULL_MESSAGE(volume_create(&bad, false, 0), "min_db >= 0");
}

static void test_starts_at_default_without_saved_value(void)
{
    create(false, 0);
    TEST_ASSERT_EQUAL_UINT8(12, volume_step(v));
}

static void test_starts_at_saved_value(void) /* AUD-05: survives restarts */
{
    create(true, 20);
    TEST_ASSERT_EQUAL_UINT8(20, volume_step(v));
}

static void test_saved_value_above_cap_is_clamped(void) /* AUD-05: cap */
{
    cfg.max_step = 24;
    create(true, 30);
    TEST_ASSERT_EQUAL_UINT8(24, volume_step(v));
}

static void test_nothing_to_save_after_start(void)
{
    create(true, 20);
    uint8_t s;
    TEST_ASSERT_FALSE(volume_take_save(v, T0 + 60000, &s));
}

/* ------------------------------------------------------------------------ */
/* Turning                                                                  */
/* ------------------------------------------------------------------------ */

static void test_turning_changes_step(void)
{
    create(false, 0);
    volume_turn(v, 3, T0);
    TEST_ASSERT_EQUAL_UINT8(15, volume_step(v));
    volume_turn(v, -5, T0 + 10);
    TEST_ASSERT_EQUAL_UINT8(10, volume_step(v));
}

static void test_turning_clamps_at_zero_and_cap(void) /* AUD-05: cap */
{
    cfg.max_step = 24;
    create(false, 0);
    volume_turn(v, 100, T0);
    TEST_ASSERT_EQUAL_UINT8(24, volume_step(v));
    volume_turn(v, -100, T0 + 10);
    TEST_ASSERT_EQUAL_UINT8(0, volume_step(v));
}

static void test_extreme_detent_counts_do_not_overflow(void)
{
    create(false, 0);
    volume_turn(v, INT32_MAX, T0);
    TEST_ASSERT_EQUAL_UINT8(32, volume_step(v));
    volume_turn(v, INT32_MIN, T0 + 10);
    TEST_ASSERT_EQUAL_UINT8(0, volume_step(v));
}

/* ------------------------------------------------------------------------ */
/* Gain curve                                                               */
/* ------------------------------------------------------------------------ */

static void test_step_zero_is_mute_and_full_scale_is_unity(void)
{
    create(true, 0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, volume_gain(v));
    volume_turn(v, 32, T0);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, volume_gain(v));
}

static void test_steps_are_equal_in_db(void)
{
    create(true, 1);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, db_to_gain(-48.0f), volume_gain(v));

    volume_turn(v, 16, T0); /* step 17: -48 * 15/31 dB */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, db_to_gain(-48.0f * 15.0f / 31.0f), volume_gain(v));
}

static void test_gain_rises_monotonically(void)
{
    create(true, 0);
    float prev = volume_gain(v);
    for (int s = 1; s <= 32; s++) {
        volume_turn(v, 1, T0 + (uint32_t)s);
        TEST_ASSERT_TRUE(volume_gain(v) > prev);
        prev = volume_gain(v);
    }
}

static void test_cap_limits_gain(void)
{
    cfg.max_step = 24;
    create(false, 0);
    volume_turn(v, 100, T0);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, db_to_gain(-48.0f * 8.0f / 31.0f), volume_gain(v));
}

/* ------------------------------------------------------------------------ */
/* Saving                                                                   */
/* ------------------------------------------------------------------------ */

static void test_saves_once_after_rest_time(void)
{
    create(false, 0);
    uint8_t s = 0;
    volume_turn(v, 2, T0);
    TEST_ASSERT_FALSE(volume_take_save(v, T0 + 1999, &s));
    TEST_ASSERT_TRUE(volume_take_save(v, T0 + 2000, &s));
    TEST_ASSERT_EQUAL_UINT8(14, s);
    TEST_ASSERT_FALSE_MESSAGE(volume_take_save(v, T0 + 5000, &s), "must save only once per change");
}

static void test_each_turn_restarts_rest_time(void)
{
    create(false, 0);
    uint8_t s = 0;
    volume_turn(v, 1, T0);
    volume_turn(v, 1, T0 + 1500);
    TEST_ASSERT_FALSE(volume_take_save(v, T0 + 3000, &s));
    TEST_ASSERT_TRUE(volume_take_save(v, T0 + 3500, &s));
    TEST_ASSERT_EQUAL_UINT8(14, s);
}

static void test_no_save_when_value_returns_to_saved(void)
{
    create(true, 20);
    uint8_t s;
    volume_turn(v, 1, T0);
    volume_turn(v, -1, T0 + 100);
    TEST_ASSERT_FALSE(volume_take_save(v, T0 + 10000, &s));
}

static void test_no_save_when_turning_against_cap(void)
{
    create(true, 32);
    uint8_t s;
    volume_turn(v, 5, T0);
    TEST_ASSERT_FALSE(volume_take_save(v, T0 + 10000, &s));
}

static void test_second_change_is_saved_again(void)
{
    create(false, 0);
    uint8_t s = 0;
    volume_turn(v, 1, T0);
    TEST_ASSERT_TRUE(volume_take_save(v, T0 + 2000, &s));
    volume_turn(v, 1, T0 + 3000);
    TEST_ASSERT_TRUE(volume_take_save(v, T0 + 5000, &s));
    TEST_ASSERT_EQUAL_UINT8(14, s);
}

static void test_save_timing_works_across_counter_wrap(void)
{
    create(false, 0);
    uint8_t s = 0;
    const uint32_t t = UINT32_MAX - 500u;
    volume_turn(v, 1, t);
    TEST_ASSERT_FALSE(volume_take_save(v, t + 1000u, &s));
    TEST_ASSERT_TRUE(volume_take_save(v, t + 2000u, &s));
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_create_rejects_invalid_config);
    RUN_TEST(test_starts_at_default_without_saved_value);
    RUN_TEST(test_starts_at_saved_value);
    RUN_TEST(test_saved_value_above_cap_is_clamped);
    RUN_TEST(test_nothing_to_save_after_start);
    RUN_TEST(test_turning_changes_step);
    RUN_TEST(test_turning_clamps_at_zero_and_cap);
    RUN_TEST(test_extreme_detent_counts_do_not_overflow);
    RUN_TEST(test_step_zero_is_mute_and_full_scale_is_unity);
    RUN_TEST(test_steps_are_equal_in_db);
    RUN_TEST(test_gain_rises_monotonically);
    RUN_TEST(test_cap_limits_gain);
    RUN_TEST(test_saves_once_after_rest_time);
    RUN_TEST(test_each_turn_restarts_rest_time);
    RUN_TEST(test_no_save_when_value_returns_to_saved);
    RUN_TEST(test_no_save_when_turning_against_cap);
    RUN_TEST(test_second_change_is_saved_again);
    RUN_TEST(test_save_timing_works_across_counter_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
