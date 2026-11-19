/*
 * Tests for lib/light_ramp – requirements BED-30 ... BED-33.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "light_ramp.h"

static light_ramp_config_t cfg;
static light_ramp_t *lr;
static uint32_t now;

void setUp(void)
{
    light_ramp_config_default(&cfg);
    cfg.max_duty = 16383;
    cfg.gamma = 2.2f;
    lr = NULL;
    now = 40000u;
}

void tearDown(void)
{
    light_ramp_destroy(lr);
    lr = NULL;
}

static void create(void)
{
    lr = light_ramp_create(&cfg, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(lr, "light_ramp_create returned NULL for a valid config");
}

static void test_defaults_and_validation(void)
{
    light_ramp_config_t d;
    memset(&d, 0, sizeof(d));
    light_ramp_config_default(&d);
    TEST_ASSERT_EQUAL_UINT16(16383, d.max_duty);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.2f, d.gamma);
    create();
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_level(lr, now));
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_now(lr, now));
    TEST_ASSERT_FALSE(light_ramp_busy(lr, now));

    light_ramp_config_t bad = d;
    bad.max_duty = 254;
    TEST_ASSERT_NULL(light_ramp_create(&bad, now));
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_duty(&bad, 1000));
    bad.max_duty = 16384;
    TEST_ASSERT_NULL(light_ramp_create(&bad, now));
    bad = d;
    bad.gamma = 0.9f;
    TEST_ASSERT_NULL(light_ramp_create(&bad, now));
    bad.gamma = 3.1f;
    TEST_ASSERT_NULL(light_ramp_create(&bad, now));
    TEST_ASSERT_NULL(light_ramp_create(NULL, now));
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_duty(NULL, 1000));
    light_ramp_destroy(NULL);
}

static void test_duty_fixed_points(void)
{
    TEST_ASSERT_EQUAL_UINT16(16383, light_ramp_duty(&cfg, 1000));
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_duty(&cfg, 0));
    TEST_ASSERT_EQUAL_UINT16(16383, light_ramp_duty(&cfg, 1001));
    TEST_ASSERT_EQUAL_UINT16(16383, light_ramp_duty(&cfg, 65535));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1, light_ramp_duty(&cfg, 1), "on is never rounded down to dark");
    cfg.max_duty = 255;
    cfg.gamma = 3.0f;
    TEST_ASSERT_EQUAL_UINT16(1, light_ramp_duty(&cfg, 1));
    TEST_ASSERT_EQUAL_UINT16(1, light_ramp_duty(&cfg, 100)); /* 255 * 0.001 rounds to 0 without the floor */
    TEST_ASSERT_EQUAL_UINT16(255, light_ramp_duty(&cfg, 1000));
}

static void test_duty_follows_the_gamma_curve(void)
{
    TEST_ASSERT_UINT16_WITHIN(2, 3565, light_ramp_duty(&cfg, 500)); /* 16383 * 0.5^2.2 */
    TEST_ASSERT_UINT16_WITHIN(2, 103, light_ramp_duty(&cfg, 100));  /* 16383 * 0.1^2.2 */
    TEST_ASSERT_UINT16_WITHIN(2, 12993, light_ramp_duty(&cfg, 900));
    cfg.gamma = 1.0f; /* linear */
    TEST_ASSERT_UINT16_WITHIN(1, 8192, light_ramp_duty(&cfg, 500));
    TEST_ASSERT_UINT16_WITHIN(1, 16, light_ramp_duty(&cfg, 1));
    cfg.gamma = 2.0f;
    cfg.max_duty = 4095;
    TEST_ASSERT_UINT16_WITHIN(1, 1024, light_ramp_duty(&cfg, 500));
    for (int g = 10; g <= 30; g += 4) {
        cfg.gamma = (float)g / 10.0f;
        for (uint16_t level = 0; level <= 1000; level += 7) {
            const double want = 4095.0 * pow(level / 1000.0, (double)cfg.gamma);
            const uint16_t got = light_ramp_duty(&cfg, level);
            if (level > 0 && want < 1.0) {
                TEST_ASSERT_EQUAL_UINT16(1, got);
            } else {
                TEST_ASSERT_UINT16_WITHIN(1, (uint16_t)lrint(want), got);
            }
        }
    }
}

static void test_duty_never_decreases(void)
{
    const float gammas[] = {1.0f, 1.8f, 2.2f, 3.0f};
    const uint16_t maxes[] = {255, 4095, 16383};
    for (int g = 0; g < 4; g++) {
        for (int m = 0; m < 3; m++) {
            cfg.gamma = gammas[g];
            cfg.max_duty = maxes[m];
            uint16_t prev = 0;
            for (uint16_t level = 0; level <= 1000; level++) {
                const uint16_t d = light_ramp_duty(&cfg, level);
                TEST_ASSERT_TRUE(d >= prev);
                TEST_ASSERT_TRUE(d <= cfg.max_duty);
                prev = d;
            }
            TEST_ASSERT_EQUAL_UINT16(cfg.max_duty, prev);
        }
    }
}

static void test_fade_is_linear_in_time(void)
{
    create();
    light_ramp_set(lr, 800, 20 * 60 * 1000, now); /* sunrise to 80 % over 20 minutes */
    TEST_ASSERT_TRUE(light_ramp_busy(lr, now));
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_level(lr, now));
    now += 5 * 60 * 1000;
    TEST_ASSERT_UINT16_WITHIN(1, 200, light_ramp_level(lr, now));
    now += 5 * 60 * 1000;
    TEST_ASSERT_UINT16_WITHIN(1, 400, light_ramp_level(lr, now));
    TEST_ASSERT_UINT16_WITHIN(3, 2182, light_ramp_now(lr, now)); /* 16383 * 0.4^2.2 */
    now += 10 * 60 * 1000 - 1;
    TEST_ASSERT_TRUE(light_ramp_busy(lr, now));
    now += 1;
    TEST_ASSERT_EQUAL_UINT16(800, light_ramp_level(lr, now));
    TEST_ASSERT_FALSE(light_ramp_busy(lr, now));
    now += 3600000;
    TEST_ASSERT_EQUAL_UINT16(800, light_ramp_level(lr, now));
    TEST_ASSERT_EQUAL_UINT16(light_ramp_duty(&cfg, 800), light_ramp_now(lr, now));
}

static void test_fade_steps_are_small_and_monotonic(void)
{
    create();
    light_ramp_set(lr, 1000, 10000, now);
    uint16_t prev = 0;
    for (int i = 0; i <= 500; i++) { /* every 20 ms */
        const uint16_t level = light_ramp_level(lr, now);
        TEST_ASSERT_TRUE(level >= prev);
        TEST_ASSERT_TRUE_MESSAGE(level - prev <= 3, "20 ms of a 10 s fade are 2 per mille");
        prev = level;
        now += 20;
    }
    TEST_ASSERT_EQUAL_UINT16(1000, prev);
    light_ramp_set(lr, 0, 10000, now); /* and down again */
    for (int i = 0; i <= 500; i++) {
        const uint16_t level = light_ramp_level(lr, now);
        TEST_ASSERT_TRUE(level <= prev);
        prev = level;
        now += 20;
    }
    TEST_ASSERT_EQUAL_UINT16(0, prev);
}

static void test_zero_duration_jumps(void)
{
    create();
    light_ramp_set(lr, 300, 0, now);
    TEST_ASSERT_EQUAL_UINT16(300, light_ramp_level(lr, now));
    TEST_ASSERT_FALSE(light_ramp_busy(lr, now));
    light_ramp_set(lr, 2000, 0, now); /* clamped */
    TEST_ASSERT_EQUAL_UINT16(1000, light_ramp_level(lr, now));
    light_ramp_set(lr, 1000, 5000, now); /* already there: nothing to do */
    TEST_ASSERT_FALSE(light_ramp_busy(lr, now));
}

static void test_new_target_during_a_fade_starts_from_the_current_level(void)
{
    create();
    light_ramp_set(lr, 1000, 10000, now);
    now += 4000;
    TEST_ASSERT_UINT16_WITHIN(1, 400, light_ramp_level(lr, now));
    light_ramp_set(lr, 0, 2000, now); /* snooze: fade out from where it is */
    TEST_ASSERT_UINT16_WITHIN(1, 400, light_ramp_level(lr, now));
    now += 1000;
    TEST_ASSERT_UINT16_WITHIN(1, 200, light_ramp_level(lr, now));
    now += 1000;
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_level(lr, now));
    TEST_ASSERT_FALSE(light_ramp_busy(lr, now));
}

static void test_clock_wrap_and_long_durations(void)
{
    now = 0xFFFFFFFFu - 600000u;
    create();
    light_ramp_set(lr, 1000, 1200000, now);
    now += 600000; /* about to wrap */
    TEST_ASSERT_UINT16_WITHIN(1, 500, light_ramp_level(lr, now));
    now += 300000; /* wrapped */
    TEST_ASSERT_UINT16_WITHIN(1, 750, light_ramp_level(lr, now));
    now += 300000;
    TEST_ASSERT_EQUAL_UINT16(1000, light_ramp_level(lr, now));

    light_ramp_set(lr, 0, 0xFFFFFFFFu, now); /* counts as 4000000 ms */
    now += 2000000;
    TEST_ASSERT_UINT16_WITHIN(1, 500, light_ramp_level(lr, now));
    now += 2000000;
    TEST_ASSERT_EQUAL_UINT16(0, light_ramp_level(lr, now));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_duty_fixed_points);
    RUN_TEST(test_duty_follows_the_gamma_curve);
    RUN_TEST(test_duty_never_decreases);
    RUN_TEST(test_fade_is_linear_in_time);
    RUN_TEST(test_fade_steps_are_small_and_monotonic);
    RUN_TEST(test_zero_duration_jumps);
    RUN_TEST(test_new_target_during_a_fade_starts_from_the_current_level);
    RUN_TEST(test_clock_wrap_and_long_durations);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
