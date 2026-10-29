/*
 * Tests for lib/fade_curve – requirement AUD-06 (fades that sound even).
 */
#include <math.h>
#include <unity.h>

#include "fade_curve.h"

#define FLOOR FADE_CURVE_FLOOR_DB_DEFAULT

void setUp(void) {}
void tearDown(void) {}

static float db_of(float g)
{
    return 20.0f * log10f(g);
}

/* ------------------------------------------------------------------------ */

static void test_ends_are_exact(void)
{
    TEST_ASSERT_EQUAL_FLOAT(0.0f, fade_curve_gain(0.0f, FLOOR));
    TEST_ASSERT_TRUE_MESSAGE(fade_curve_gain(1.0f, FLOOR) == 1.0f, "p = 1 must give exactly 1.0");
}

static void test_out_of_range_progress_is_clamped(void)
{
    TEST_ASSERT_TRUE(fade_curve_gain(-0.1f, FLOOR) == 0.0f);
    TEST_ASSERT_TRUE(fade_curve_gain(-INFINITY, FLOOR) == 0.0f);
    TEST_ASSERT_TRUE(fade_curve_gain(1.5f, FLOOR) == 1.0f);
    TEST_ASSERT_TRUE(fade_curve_gain(INFINITY, FLOOR) == 1.0f);
    TEST_ASSERT_TRUE_MESSAGE(fade_curve_gain(NAN, FLOOR) == 0.0f, "NaN progress is silence");
}

static void test_level_is_a_straight_line_in_db(void)
{
    const float ps[] = {0.01f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 0.99f};
    for (unsigned i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        const float expect_db = FLOOR * (1.0f - ps[i]);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, expect_db, db_of(fade_curve_gain(ps[i], FLOOR)));
    }
}

static void test_halfway_is_half_the_floor_in_db(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0562341f, fade_curve_gain(0.5f, -50.0f)); /* -25 dB */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.1f, fade_curve_gain(0.5f, -40.0f));       /* -20 dB */
}

static void test_equal_progress_steps_give_equal_db_steps(void)
{
    const float step_db = -FLOOR / 10.0f;
    for (int i = 1; i < 10; i++) {
        const float a = fade_curve_gain((float)i / 10.0f, FLOOR);
        const float b = fade_curve_gain((float)(i + 1) / 10.0f, FLOOR);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, step_db, db_of(b) - db_of(a));
    }
}

static void test_gain_above_zero_as_soon_as_progress_is(void)
{
    const float g = fade_curve_gain(1e-6f, FLOOR);
    TEST_ASSERT_TRUE(g > 0.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, FLOOR, db_of(g));
    TEST_ASSERT_TRUE_MESSAGE(fade_curve_gain(1e-30f, FADE_CURVE_FLOOR_DB_MIN) > 0.0f,
                             "lowest allowed floor must not underflow to 0");
}

static void test_strictly_increasing(void)
{
    float prev = fade_curve_gain(0.0f, FLOOR);
    for (int i = 1; i <= 1000; i++) {
        const float g = fade_curve_gain((float)i / 1000.0f, FLOOR);
        TEST_ASSERT_TRUE_MESSAGE(g > prev, "gain must rise with progress");
        TEST_ASSERT_TRUE(g <= 1.0f);
        prev = g;
    }
}

static void test_much_quieter_than_linear_halfway(void)
{
    /* the reason for this module: linear would still be at -6 dB */
    TEST_ASSERT_TRUE(fade_curve_gain(0.5f, FLOOR) < 0.5f * 0.25f);
}

static void test_invalid_floor_uses_default(void)
{
    const float expect = fade_curve_gain(0.5f, FLOOR);
    TEST_ASSERT_EQUAL_FLOAT(expect, fade_curve_gain(0.5f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(expect, fade_curve_gain(0.5f, 6.0f));
    TEST_ASSERT_EQUAL_FLOAT(expect, fade_curve_gain(0.5f, NAN));
    TEST_ASSERT_EQUAL_FLOAT(expect, fade_curve_gain(0.5f, -INFINITY));
    TEST_ASSERT_EQUAL_FLOAT(expect, fade_curve_gain(0.5f, -100.5f));
}

static void test_floor_limits_are_accepted(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -50.0f, db_of(fade_curve_gain(0.5f, -100.0f)));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.5f, db_of(fade_curve_gain(0.5f, -1.0f)));
}

static void test_ends_ignore_the_floor(void)
{
    TEST_ASSERT_TRUE(fade_curve_gain(0.0f, -10.0f) == 0.0f);
    TEST_ASSERT_TRUE(fade_curve_gain(1.0f, -10.0f) == 1.0f);
    TEST_ASSERT_TRUE(fade_curve_gain(1.0f, NAN) == 1.0f);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ends_are_exact);
    RUN_TEST(test_out_of_range_progress_is_clamped);
    RUN_TEST(test_level_is_a_straight_line_in_db);
    RUN_TEST(test_halfway_is_half_the_floor_in_db);
    RUN_TEST(test_equal_progress_steps_give_equal_db_steps);
    RUN_TEST(test_gain_above_zero_as_soon_as_progress_is);
    RUN_TEST(test_strictly_increasing);
    RUN_TEST(test_much_quieter_than_linear_halfway);
    RUN_TEST(test_invalid_floor_uses_default);
    RUN_TEST(test_floor_limits_are_accepted);
    RUN_TEST(test_ends_ignore_the_floor);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
