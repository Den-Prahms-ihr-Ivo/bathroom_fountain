/*
 * Tests for lib/mode_volume – requirements AUD-13, AUD-14.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "mode_volume.h"

#define T0 50000u

static volume_config_t cfg;
static mode_volume_t *mv;

void setUp(void)
{
    volume_config_default(&cfg);
    cfg.steps = 32;
    cfg.max_step = 32;
    cfg.default_step = 12;
    cfg.save_delay_ms = 2000;
    mv = NULL;
}

void tearDown(void)
{
    mode_volume_destroy(mv);
    mv = NULL;
}

static void create(size_t count, const bool *has, const uint8_t *saved)
{
    mv = mode_volume_create(&cfg, count, has, saved);
    TEST_ASSERT_NOT_NULL_MESSAGE(mv, "mode_volume_create returned NULL for valid arguments");
}

static void test_create_rejects_bad_arguments(void)
{
    const bool has[2] = {true, true};
    const uint8_t saved[2] = {5, 6};
    TEST_ASSERT_NULL(mode_volume_create(NULL, 2, NULL, NULL));
    TEST_ASSERT_NULL(mode_volume_create(&cfg, 0, NULL, NULL));
    TEST_ASSERT_NULL(mode_volume_create(&cfg, MODE_VOLUME_MAX + 1, NULL, NULL));
    TEST_ASSERT_NULL_MESSAGE(mode_volume_create(&cfg, 2, has, NULL), "has_saved without saved");
    TEST_ASSERT_NULL_MESSAGE(mode_volume_create(&cfg, 2, NULL, saved), "saved without has_saved");
    volume_config_t bad = cfg;
    bad.min_db = 3.0f; /* invalid for volume_create() */
    TEST_ASSERT_NULL(mode_volume_create(&bad, 2, NULL, NULL));
    mode_volume_t *eight = mode_volume_create(&cfg, MODE_VOLUME_MAX, NULL, NULL);
    TEST_ASSERT_NOT_NULL(eight);
    TEST_ASSERT_EQUAL_size_t(MODE_VOLUME_MAX, mode_volume_count(eight));
    mode_volume_destroy(eight);
    mode_volume_destroy(NULL);
    TEST_ASSERT_EQUAL_size_t(0, mode_volume_count(NULL));
}

static void test_start_values(void)
{
    const bool has[3] = {true, false, true};
    const uint8_t saved[3] = {20, 30, 40}; /* 40 is above the cap of 32 */
    create(3, has, saved);
    TEST_ASSERT_EQUAL_size_t(3, mode_volume_count(mv));
    TEST_ASSERT_EQUAL_UINT8(20, mode_volume_step(mv, 0));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(12, mode_volume_step(mv, 1), "nothing saved: default step");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(32, mode_volume_step(mv, 2), "clamped to the cap");
}

static void test_nothing_saved_at_all(void)
{
    create(2, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT8(12, mode_volume_step(mv, 0));
    TEST_ASSERT_EQUAL_UINT8(12, mode_volume_step(mv, 1));
    int m = -5;
    uint8_t s = 0;
    TEST_ASSERT_FALSE(mode_volume_take_save(mv, T0 + 100000, &m, &s));
}

static void test_turn_changes_only_that_mode(void)
{
    create(3, NULL, NULL);
    TEST_ASSERT_TRUE(mode_volume_turn(mv, 2, 5, T0));
    TEST_ASSERT_EQUAL_UINT8(12, mode_volume_step(mv, 0));
    TEST_ASSERT_EQUAL_UINT8(12, mode_volume_step(mv, 1));
    TEST_ASSERT_EQUAL_UINT8(17, mode_volume_step(mv, 2));
    TEST_ASSERT_TRUE(mode_volume_turn(mv, 0, -3, T0));
    TEST_ASSERT_EQUAL_UINT8(9, mode_volume_step(mv, 0));
    TEST_ASSERT_EQUAL_UINT8(17, mode_volume_step(mv, 2));
}

static void test_turn_without_mode_is_ignored(void)
{
    create(2, NULL, NULL);
    TEST_ASSERT_FALSE(mode_volume_turn(mv, MODE_VOLUME_NONE, 4, T0));
    TEST_ASSERT_FALSE(mode_volume_turn(mv, 2, 4, T0));
    TEST_ASSERT_FALSE(mode_volume_turn(mv, -7, 4, T0));
    TEST_ASSERT_FALSE(mode_volume_turn(NULL, 0, 4, T0));
    TEST_ASSERT_EQUAL_UINT8(12, mode_volume_step(mv, 0));
    TEST_ASSERT_EQUAL_UINT8(12, mode_volume_step(mv, 1));
    int m;
    uint8_t s;
    TEST_ASSERT_FALSE_MESSAGE(mode_volume_take_save(mv, T0 + 100000, &m, &s), "ignored turns save nothing");
}

static void test_turn_at_limit_still_counts(void)
{
    const bool has[1] = {true};
    const uint8_t saved[1] = {32};
    create(1, has, saved);
    TEST_ASSERT_TRUE(mode_volume_turn(mv, 0, 3, T0));
    TEST_ASSERT_EQUAL_UINT8(32, mode_volume_step(mv, 0));
}

static void test_gain_follows_the_mode(void)
{
    create(2, NULL, NULL);
    mode_volume_turn(mv, 1, -12, T0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mode_volume_gain(mv, 1)); /* step 0 = mute */
    TEST_ASSERT_TRUE(mode_volume_gain(mv, 0) > 0.0f);
    mode_volume_turn(mv, 0, 20, T0);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, mode_volume_gain(mv, 0)); /* full scale */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mode_volume_gain(mv, 5));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mode_volume_gain(NULL, 0));
    TEST_ASSERT_EQUAL_UINT8(0, mode_volume_step(mv, MODE_VOLUME_NONE));
    TEST_ASSERT_EQUAL_UINT8(0, mode_volume_step(NULL, 0));
}

static void test_each_mode_saves_once_after_rest(void)
{
    create(3, NULL, NULL);
    mode_volume_turn(mv, 2, 2, T0);
    mode_volume_turn(mv, 0, 1, T0 + 500);
    int m = -5;
    uint8_t s = 0;
    TEST_ASSERT_FALSE(mode_volume_take_save(mv, T0 + 1999, &m, &s));
    /* mode 2 rested first, but nothing of mode 0 is due yet */
    TEST_ASSERT_TRUE(mode_volume_take_save(mv, T0 + 2000, &m, &s));
    TEST_ASSERT_EQUAL_INT(2, m);
    TEST_ASSERT_EQUAL_UINT8(14, s);
    TEST_ASSERT_FALSE(mode_volume_take_save(mv, T0 + 2400, &m, &s));
    TEST_ASSERT_TRUE(mode_volume_take_save(mv, T0 + 2500, &m, &s));
    TEST_ASSERT_EQUAL_INT(0, m);
    TEST_ASSERT_EQUAL_UINT8(13, s);
    TEST_ASSERT_FALSE(mode_volume_take_save(mv, T0 + 60000, &m, &s));
}

static void test_one_mode_per_call_lowest_first(void)
{
    create(3, NULL, NULL);
    mode_volume_turn(mv, 2, 1, T0);
    mode_volume_turn(mv, 1, -1, T0);
    int m = -5;
    uint8_t s = 0;
    TEST_ASSERT_TRUE(mode_volume_take_save(mv, T0 + 5000, &m, &s));
    TEST_ASSERT_EQUAL_INT(1, m);
    TEST_ASSERT_EQUAL_UINT8(11, s);
    TEST_ASSERT_TRUE(mode_volume_take_save(mv, T0 + 5000, &m, &s));
    TEST_ASSERT_EQUAL_INT(2, m);
    TEST_ASSERT_EQUAL_UINT8(13, s);
    TEST_ASSERT_FALSE(mode_volume_take_save(mv, T0 + 5000, &m, &s));
}

static void test_take_save_null_outputs(void)
{
    create(1, NULL, NULL);
    mode_volume_turn(mv, 0, 1, T0);
    int m;
    uint8_t s;
    TEST_ASSERT_FALSE(mode_volume_take_save(mv, T0 + 5000, NULL, &s));
    TEST_ASSERT_FALSE(mode_volume_take_save(mv, T0 + 5000, &m, NULL));
    TEST_ASSERT_FALSE(mode_volume_take_save(NULL, T0 + 5000, &m, &s));
    /* nothing was taken by the refused calls */
    TEST_ASSERT_TRUE(mode_volume_take_save(mv, T0 + 5000, &m, &s));
    TEST_ASSERT_EQUAL_INT(0, m);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_rejects_bad_arguments);
    RUN_TEST(test_start_values);
    RUN_TEST(test_nothing_saved_at_all);
    RUN_TEST(test_turn_changes_only_that_mode);
    RUN_TEST(test_turn_without_mode_is_ignored);
    RUN_TEST(test_turn_at_limit_still_counts);
    RUN_TEST(test_gain_follows_the_mode);
    RUN_TEST(test_each_mode_saves_once_after_rest);
    RUN_TEST(test_one_mode_per_call_lowest_first);
    RUN_TEST(test_take_save_null_outputs);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
