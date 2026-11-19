/*
 * Tests for lib/target_logic – requirements GAME-40 ... GAME-46.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "target_logic.h"

static target_config_t cfg;
static target_logic_t *t;
static uint32_t now;

void setUp(void)
{
    target_config_default(&cfg);
    cfg.flash_ms = 300;
    cfg.zone_min[0] = 150;
    cfg.zone_min[1] = 400;
    cfg.zone_min[2] = 700;
    t = NULL;
    now = 3000000u;
}

void tearDown(void)
{
    target_logic_destroy(t);
    t = NULL;
}

static void create(void)
{
    t = target_logic_create(&cfg, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "target_logic_create returned NULL for a valid config");
}

static bool command(uint8_t cmd, uint8_t round, uint16_t arg_ms)
{
    const link_target_cmd_t c = {cmd, round, arg_ms};
    return target_logic_command(t, &c, now);
}

static void run_ms(uint32_t ms)
{
    while (ms > 0) {
        const uint32_t step = ms > 10 ? 10 : ms;
        now += step;
        ms -= step;
        target_logic_tick(t, now);
    }
}

static void expect_event(uint8_t evt, uint8_t round, uint8_t gun, uint16_t value)
{
    link_target_evt_t e;
    TEST_ASSERT_TRUE_MESSAGE(target_logic_take_event(t, &e), "event missing");
    TEST_ASSERT_EQUAL_UINT8(evt, e.evt);
    TEST_ASSERT_EQUAL_UINT8(round, e.round);
    if (evt == LINK_TARGET_HIT) {
        TEST_ASSERT_EQUAL_UINT8(gun, e.gun_id);
        TEST_ASSERT_EQUAL_UINT16(value, e.value);
    }
}

static void expect_no_event(void)
{
    link_target_evt_t e;
    TEST_ASSERT_FALSE(target_logic_take_event(t, &e));
}

static void test_defaults_and_validation(void)
{
    target_config_t d;
    memset(&d, 0, sizeof(d));
    target_config_default(&d);
    TEST_ASSERT_EQUAL_UINT16(300, d.flash_ms);
    TEST_ASSERT_EQUAL_UINT16(150, d.zone_min[0]);
    TEST_ASSERT_EQUAL_UINT16(400, d.zone_min[1]);
    TEST_ASSERT_EQUAL_UINT16(700, d.zone_min[2]);
    create();
    TEST_ASSERT_EQUAL_INT(TARGET_LED_OFF, target_logic_led(t));
    expect_no_event();

    target_config_t bad = d;
    bad.flash_ms = 49;
    TEST_ASSERT_NULL(target_logic_create(&bad, now));
    bad.flash_ms = 2001;
    TEST_ASSERT_NULL(target_logic_create(&bad, now));
    bad = d;
    bad.zone_min[0] = 0;
    TEST_ASSERT_NULL(target_logic_create(&bad, now));
    bad = d;
    bad.zone_min[2] = 1001;
    TEST_ASSERT_NULL(target_logic_create(&bad, now));
    bad = d;
    bad.zone_min[1] = 100; /* below zone 1 */
    TEST_ASSERT_NULL(target_logic_create(&bad, now));
    TEST_ASSERT_NULL(target_logic_create(NULL, now));
    target_logic_destroy(NULL);

    TEST_ASSERT_TRUE(target_logic_setup(t, 15, 3));
    TEST_ASSERT_TRUE(target_logic_setup(t, TARGET_ANY_GUN, 1));
    TEST_ASSERT_FALSE(target_logic_setup(t, 16, 1));
    TEST_ASSERT_FALSE(target_logic_setup(t, 3, 0));
    TEST_ASSERT_FALSE(target_logic_setup(t, 3, 4));
}

static void test_lit_then_hit(void)
{
    create();
    TEST_ASSERT_TRUE(command(LINK_TARGET_LIGHT, 7, 8000));
    TEST_ASSERT_EQUAL_INT(TARGET_LED_LIT, target_logic_led(t));
    run_ms(2500);
    TEST_ASSERT_TRUE(target_logic_shot(t, 4, 820, now));
    TEST_ASSERT_EQUAL_INT(TARGET_LED_FLASH, target_logic_led(t));
    expect_event(LINK_TARGET_HIT, 7, 4, 820);
    expect_no_event();
    TEST_ASSERT_FALSE_MESSAGE(target_logic_shot(t, 4, 820, now), "one hit per lit phase");
    run_ms(290);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_FLASH, target_logic_led(t));
    run_ms(10);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_OFF, target_logic_led(t));
    run_ms(10000);
    expect_no_event(); /* no timeout after a hit */
}

static void test_timeout_without_a_hit(void)
{
    create();
    command(LINK_TARGET_LIGHT, 9, 4000);
    run_ms(3990);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_LIT, target_logic_led(t));
    expect_no_event();
    run_ms(10);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_OFF, target_logic_led(t));
    expect_event(LINK_TARGET_TIMEOUT, 9, 0, 0);
    expect_no_event();
    TEST_ASSERT_FALSE(target_logic_shot(t, 4, 900, now)); /* too late */
    expect_no_event();
}

static void test_shots_only_count_while_lit(void)
{
    create();
    TEST_ASSERT_FALSE(target_logic_shot(t, 4, 900, now));
    command(LINK_TARGET_IDENTIFY, 0, 2000);
    TEST_ASSERT_FALSE(target_logic_shot(t, 4, 900, now));
    expect_no_event();
    TEST_ASSERT_EQUAL_INT(TARGET_LED_IDENTIFY, target_logic_led(t));
}

static void test_hit_zone_is_a_strength_threshold(void)
{
    create();
    target_logic_setup(t, TARGET_ANY_GUN, 1);
    command(LINK_TARGET_LIGHT, 1, 8000);
    TEST_ASSERT_FALSE(target_logic_shot(t, 2, 149, now));
    TEST_ASSERT_EQUAL_INT_MESSAGE(TARGET_LED_LIT, target_logic_led(t), "a weak burst leaves the target lit");
    TEST_ASSERT_TRUE(target_logic_shot(t, 2, 150, now));
    expect_event(LINK_TARGET_HIT, 1, 2, 150);

    target_logic_setup(t, TARGET_ANY_GUN, 2);
    command(LINK_TARGET_LIGHT, 2, 8000);
    TEST_ASSERT_FALSE(target_logic_shot(t, 2, 399, now));
    TEST_ASSERT_TRUE(target_logic_shot(t, 2, 400, now));
    expect_event(LINK_TARGET_HIT, 2, 2, 400);

    target_logic_setup(t, TARGET_ANY_GUN, 3);
    command(LINK_TARGET_LIGHT, 3, 8000);
    TEST_ASSERT_FALSE(target_logic_shot(t, 2, 699, now));
    TEST_ASSERT_TRUE(target_logic_shot(t, 2, 1000, now));
    expect_event(LINK_TARGET_HIT, 3, 2, 1000);
    expect_no_event();
}

static void test_only_the_paired_gun_counts(void)
{
    create();
    target_logic_setup(t, 6, 1);
    command(LINK_TARGET_LIGHT, 1, 8000);
    TEST_ASSERT_FALSE(target_logic_shot(t, 7, 900, now));
    TEST_ASSERT_EQUAL_INT(TARGET_LED_LIT, target_logic_led(t));
    TEST_ASSERT_TRUE(target_logic_shot(t, 6, 900, now));
    expect_event(LINK_TARGET_HIT, 1, 6, 900);
    target_logic_setup(t, TARGET_ANY_GUN, 1);
    command(LINK_TARGET_LIGHT, 2, 8000);
    TEST_ASSERT_TRUE(target_logic_shot(t, 7, 900, now));
    expect_event(LINK_TARGET_HIT, 2, 7, 900);
}

static void test_off_and_identify(void)
{
    create();
    command(LINK_TARGET_LIGHT, 1, 8000);
    TEST_ASSERT_TRUE(command(LINK_TARGET_OFF, 0, 0));
    TEST_ASSERT_EQUAL_INT(TARGET_LED_OFF, target_logic_led(t));
    run_ms(9000);
    expect_no_event(); /* switched off: no timeout */
    TEST_ASSERT_TRUE(command(LINK_TARGET_OFF, 0, 0)); /* already dark: fine */

    command(LINK_TARGET_LIGHT, 2, 8000);
    TEST_ASSERT_TRUE(command(LINK_TARGET_IDENTIFY, 0, 1500));
    TEST_ASSERT_EQUAL_INT(TARGET_LED_IDENTIFY, target_logic_led(t));
    run_ms(1490);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_IDENTIFY, target_logic_led(t));
    run_ms(10);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_OFF, target_logic_led(t));
    run_ms(9000);
    expect_no_event();
}

static void test_light_again_starts_over(void)
{
    create();
    command(LINK_TARGET_LIGHT, 1, 4000);
    run_ms(3000);
    command(LINK_TARGET_LIGHT, 2, 4000);
    run_ms(3990);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_LIT, target_logic_led(t));
    expect_no_event();
    run_ms(10);
    expect_event(LINK_TARGET_TIMEOUT, 2, 0, 0);
    /* lighting during the hit flash works too */
    command(LINK_TARGET_LIGHT, 3, 4000);
    target_logic_shot(t, 1, 900, now);
    command(LINK_TARGET_LIGHT, 4, 4000);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_LIT, target_logic_led(t));
    TEST_ASSERT_TRUE(target_logic_shot(t, 1, 900, now));
    expect_event(LINK_TARGET_HIT, 3, 1, 900);
    expect_event(LINK_TARGET_HIT, 4, 1, 900);
}

static void test_bad_commands_are_refused(void)
{
    create();
    TEST_ASSERT_FALSE(command(LINK_TARGET_LIGHT, 1, 99));
    TEST_ASSERT_FALSE(command(LINK_TARGET_LIGHT, 1, 60001));
    TEST_ASSERT_FALSE(command(LINK_TARGET_IDENTIFY, 1, 0));
    TEST_ASSERT_FALSE(command(LINK_TARGET_CALIBRATE, 0, 0));
    TEST_ASSERT_FALSE(command(0, 0, 1000));
    TEST_ASSERT_FALSE(command(99, 0, 1000));
    TEST_ASSERT_FALSE(target_logic_command(t, NULL, now));
    TEST_ASSERT_EQUAL_INT(TARGET_LED_OFF, target_logic_led(t));
    TEST_ASSERT_TRUE(command(LINK_TARGET_LIGHT, 1, 100));
    TEST_ASSERT_TRUE(command(LINK_TARGET_LIGHT, 1, 60000));
}

static void test_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 2000u;
    create();
    command(LINK_TARGET_LIGHT, 5, 4000);
    run_ms(3990);
    TEST_ASSERT_EQUAL_INT(TARGET_LED_LIT, target_logic_led(t));
    run_ms(10);
    expect_event(LINK_TARGET_TIMEOUT, 5, 0, 0);
}

static void test_sleep_plan(void)
{
    TEST_ASSERT_EQUAL_UINT32(21600, target_sleep_seconds(false, 0, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(21600, target_sleep_seconds(false, 999999, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(0, target_sleep_seconds(true, 0, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(0, target_sleep_seconds(true, 60, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(1, target_sleep_seconds(true, 61, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(3540, target_sleep_seconds(true, 3600, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(21600, target_sleep_seconds(true, 21660, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(21600, target_sleep_seconds(true, 8 * 86400, 60, 21600));
    TEST_ASSERT_EQUAL_UINT32(21600, target_sleep_seconds(true, 0xFFFFFFFFu, 60, 21600));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_lit_then_hit);
    RUN_TEST(test_timeout_without_a_hit);
    RUN_TEST(test_shots_only_count_while_lit);
    RUN_TEST(test_hit_zone_is_a_strength_threshold);
    RUN_TEST(test_only_the_paired_gun_counts);
    RUN_TEST(test_off_and_identify);
    RUN_TEST(test_light_again_starts_over);
    RUN_TEST(test_bad_commands_are_refused);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_sleep_plan);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
