/*
 * Tests for lib/amp_volume – requirements AUD-05, AUD-06, AUD-07.
 */
#include <math.h>
#include <stdint.h>
#include <unity.h>

#include "amp_volume.h"

void setUp(void) {}
void tearDown(void) {}

static float gain_of_db(float db)
{
    return powf(10.0f, db / 20.0f);
}

/* ------------------------------------------------------------------------ */

static void test_unity_and_above_is_zero_db(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x30, amp_volume_code(1.0f));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x30, amp_volume_code(4.0f), "never boost above 0 dB");
}

static void test_zero_negative_and_non_finite_mute(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xFF, amp_volume_code(0.0f));
    TEST_ASSERT_EQUAL_HEX8(0xFF, amp_volume_code(-0.0f));
    TEST_ASSERT_EQUAL_HEX8(0xFF, amp_volume_code(-0.5f));
    TEST_ASSERT_EQUAL_HEX8(0xFF, amp_volume_code(NAN));
    TEST_ASSERT_EQUAL_HEX8(0xFF, amp_volume_code(INFINITY));
    TEST_ASSERT_EQUAL_HEX8(0xFF, amp_volume_code(-INFINITY));
}

static void test_known_levels(void)
{
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x3C, amp_volume_code(0.5f), "-6.02 dB -> 12 steps");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x58, amp_volume_code(0.1f), "-20 dB -> 40 steps");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, amp_volume_code(gain_of_db(-40.0f)), "-40 dB -> 80 steps");
}

static void test_rounds_to_nearest_half_db(void)
{
    TEST_ASSERT_EQUAL_HEX8(0x30, amp_volume_code(gain_of_db(-0.24f)));
    TEST_ASSERT_EQUAL_HEX8(0x31, amp_volume_code(gain_of_db(-0.26f)));
    TEST_ASSERT_EQUAL_HEX8(0x31, amp_volume_code(gain_of_db(-0.74f)));
    TEST_ASSERT_EQUAL_HEX8(0x32, amp_volume_code(gain_of_db(-0.76f)));
}

static void test_tiny_gain_is_quietest_step_not_mute(void)
{
    TEST_ASSERT_EQUAL_HEX8(0xFE, amp_volume_code(gain_of_db(-103.0f)));
    TEST_ASSERT_EQUAL_HEX8(0xFE, amp_volume_code(gain_of_db(-150.0f)));
    TEST_ASSERT_EQUAL_HEX8(0xFE, amp_volume_code(1e-30f));
}

static void test_product_of_volume_and_fade(void)
{
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x48, amp_volume_code_for(0.5f, 0.5f), "-12.04 dB -> 24 steps");
    TEST_ASSERT_EQUAL_HEX8(0x30, amp_volume_code_for(1.0f, 1.0f));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, amp_volume_code_for(0.8f, 0.0f), "fade at 0 mutes");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, amp_volume_code_for(0.0f, 1.0f), "volume step 0 mutes");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, amp_volume_code_for(NAN, 1.0f), "invalid volume mutes");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, amp_volume_code_for(1.0f, -1.0f), "invalid fade mutes");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, amp_volume_code_for(-1.0f, -1.0f), "two negatives do not make a valid gain");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x30, amp_volume_code_for(2.0f, 0.9f), "product above 1 is still 0 dB");
}

static void test_product_that_underflows_is_quietest_step_not_mute(void)
{
    /* 1e-30 * 1e-30 is 0 in float, but both factors are valid and above zero */
    TEST_ASSERT_EQUAL_HEX8(0xFE, amp_volume_code_for(1e-30f, 1e-30f));
}

static void test_fade_out_is_monotonic_and_mutes_only_at_zero(void)
{
    uint8_t prev = amp_volume_code_for(0.6f, 1.0f);
    for (int i = 399; i >= 1; i--) {
        const uint8_t c = amp_volume_code_for(0.6f, (float)i / 400.0f);
        TEST_ASSERT_TRUE_MESSAGE(c >= prev, "fade-out codes must not decrease");
        TEST_ASSERT_TRUE_MESSAGE(c != 0xFF, "no mute while the fade is above zero");
        prev = c;
    }
    TEST_ASSERT_EQUAL_HEX8(0xFF, amp_volume_code_for(0.6f, 0.0f));
}

static void test_db_of_codes(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 24.0f, amp_volume_db(0x00));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, amp_volume_db(0x30));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.5f, amp_volume_db(0x31));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -103.0f, amp_volume_db(0xFE));
    const float mute = amp_volume_db(0xFF);
    TEST_ASSERT_TRUE_MESSAGE(isinf(mute) && mute < 0.0f, "mute is -INFINITY");
}

static void test_round_trip_for_every_attenuation_step(void)
{
    for (unsigned code = 0x30; code <= 0xFE; code++) {
        const float g = gain_of_db(amp_volume_db((uint8_t)code));
        TEST_ASSERT_EQUAL_HEX8(code, amp_volume_code(g));
    }
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unity_and_above_is_zero_db);
    RUN_TEST(test_zero_negative_and_non_finite_mute);
    RUN_TEST(test_known_levels);
    RUN_TEST(test_rounds_to_nearest_half_db);
    RUN_TEST(test_tiny_gain_is_quietest_step_not_mute);
    RUN_TEST(test_product_of_volume_and_fade);
    RUN_TEST(test_product_that_underflows_is_quietest_step_not_mute);
    RUN_TEST(test_fade_out_is_monotonic_and_mutes_only_at_zero);
    RUN_TEST(test_db_of_codes);
    RUN_TEST(test_round_trip_for_every_attenuation_step);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
