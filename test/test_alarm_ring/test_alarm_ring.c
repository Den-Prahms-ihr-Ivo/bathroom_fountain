/*
 * Tests for lib/alarm_ring – requirements BED-20 ... BED-28, GAME-01, GAME-08.
 * The helper ticks once per second; times are relative to the arm() call.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "alarm_ring.h"

#define MIN 60000u

static alarm_ring_config_t cfg;
static alarm_ring_t *ar;
static uint32_t now;

void setUp(void)
{
    alarm_ring_config_default(&cfg);
    cfg.sunrise_ms = 20 * MIN;
    cfg.snooze_ms = 9 * MIN;
    cfg.ring_max_ms = 15 * MIN;
    cfg.game = false;
    ar = NULL;
    now = 123456789u;
}

void tearDown(void)
{
    alarm_ring_destroy(ar);
    ar = NULL;
}

static void create(void)
{
    ar = alarm_ring_create(&cfg, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(ar, "alarm_ring_create returned NULL for a valid config");
}

static void run_ms(uint32_t ms)
{
    while (ms > 0) {
        const uint32_t step = ms > 1000 ? 1000 : ms;
        now += step;
        ms -= step;
        alarm_ring_tick(ar, now);
    }
}

static alarm_state_t state(void)
{
    return alarm_ring_outputs(ar)->state;
}

static void expect_event(alarm_event_id_t id, uint32_t value)
{
    alarm_event_t e;
    TEST_ASSERT_TRUE_MESSAGE(alarm_ring_take_event(ar, &e), "event missing");
    TEST_ASSERT_EQUAL_INT(id, e.id);
    TEST_ASSERT_EQUAL_UINT32(value, e.value);
}

static void expect_no_event(void)
{
    alarm_event_t e;
    TEST_ASSERT_FALSE(alarm_ring_take_event(ar, &e));
}

/* arm one hour ahead and run until it rings; events up to RING are taken */
static void ring_now(void)
{
    create();
    TEST_ASSERT_TRUE(alarm_ring_arm(ar, 60 * MIN, now));
    run_ms(60 * MIN);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    expect_event(ALARM_EV_SUNRISE, 20 * MIN);
    expect_event(ALARM_EV_RING, cfg.game ? 1 : 0);
}

static void test_defaults_and_validation(void)
{
    alarm_ring_config_t d;
    memset(&d, 0xEE, sizeof(d));
    alarm_ring_config_default(&d);
    TEST_ASSERT_EQUAL_UINT32(1200000, d.sunrise_ms);
    TEST_ASSERT_EQUAL_UINT32(540000, d.snooze_ms);
    TEST_ASSERT_EQUAL_UINT32(900000, d.ring_max_ms);
    TEST_ASSERT_FALSE(d.game);
    create();
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    TEST_ASSERT_EQUAL_UINT16(0, alarm_ring_outputs(ar)->light_permille);
    TEST_ASSERT_FALSE(alarm_ring_outputs(ar)->game_required);

    alarm_ring_config_t bad = d;
    bad.sunrise_ms = 3600001;
    TEST_ASSERT_NULL(alarm_ring_create(&bad, now));
    TEST_ASSERT_FALSE(alarm_ring_set_config(ar, &bad));
    bad = d;
    bad.snooze_ms = 59999;
    TEST_ASSERT_NULL(alarm_ring_create(&bad, now));
    bad.snooze_ms = 1800001;
    TEST_ASSERT_NULL(alarm_ring_create(&bad, now));
    bad = d;
    bad.ring_max_ms = 59999;
    TEST_ASSERT_NULL(alarm_ring_create(&bad, now));
    bad.ring_max_ms = 3600001;
    TEST_ASSERT_NULL(alarm_ring_create(&bad, now));
    TEST_ASSERT_NULL(alarm_ring_create(NULL, now));
    TEST_ASSERT_FALSE(alarm_ring_set_config(ar, NULL));
    TEST_ASSERT_FALSE(alarm_ring_arm(ar, 2000000001u, now));
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    alarm_ring_destroy(NULL);
}

static void test_sunrise_then_ring(void)
{
    create();
    TEST_ASSERT_TRUE(alarm_ring_arm(ar, 60 * MIN, now));
    TEST_ASSERT_EQUAL_INT(ALARM_ARMED, state());
    run_ms(40 * MIN - 1000);
    TEST_ASSERT_EQUAL_INT(ALARM_ARMED, state());
    TEST_ASSERT_EQUAL_UINT16(0, alarm_ring_outputs(ar)->light_permille);
    expect_no_event();
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT(ALARM_SUNRISE, state());
    expect_event(ALARM_EV_SUNRISE, 20 * MIN);
    TEST_ASSERT_EQUAL_UINT16(0, alarm_ring_outputs(ar)->light_permille);
    run_ms(5 * MIN);
    TEST_ASSERT_UINT16_WITHIN(1, 250, alarm_ring_outputs(ar)->light_permille);
    run_ms(5 * MIN);
    TEST_ASSERT_UINT16_WITHIN(1, 500, alarm_ring_outputs(ar)->light_permille);
    run_ms(10 * MIN - 1000);
    TEST_ASSERT_EQUAL_INT(ALARM_SUNRISE, state());
    TEST_ASSERT_UINT16_WITHIN(1, 999, alarm_ring_outputs(ar)->light_permille);
    expect_no_event();
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    TEST_ASSERT_EQUAL_UINT16(1000, alarm_ring_outputs(ar)->light_permille);
    expect_event(ALARM_EV_RING, 0);
    expect_no_event();
}

static void test_short_notice_shortens_the_sunrise(void)
{
    create();
    alarm_ring_arm(ar, 5 * MIN, now); /* less than the 20 min sunrise */
    TEST_ASSERT_EQUAL_INT(ALARM_SUNRISE, state());
    expect_event(ALARM_EV_SUNRISE, 5 * MIN);
    run_ms(150000);
    TEST_ASSERT_UINT16_WITHIN(1, 500, alarm_ring_outputs(ar)->light_permille);
    run_ms(150000);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    TEST_ASSERT_EQUAL_UINT16(1000, alarm_ring_outputs(ar)->light_permille);
}

static void test_no_sunrise_when_switched_off(void)
{
    cfg.sunrise_ms = 0;
    create();
    alarm_ring_arm(ar, 10 * MIN, now);
    run_ms(10 * MIN - 1000);
    TEST_ASSERT_EQUAL_INT(ALARM_ARMED, state());
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    expect_event(ALARM_EV_RING, 0);
    expect_no_event();
    /* in_ms 0 rings at once */
    tearDown();
    create();
    alarm_ring_arm(ar, 0, now);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
}

static void test_snooze_and_off(void)
{
    ring_now();
    run_ms(30000);
    TEST_ASSERT_TRUE(alarm_ring_button_short(ar, now));
    TEST_ASSERT_EQUAL_INT(ALARM_SNOOZED, state());
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1000, alarm_ring_outputs(ar)->light_permille, "the light stays on");
    expect_event(ALARM_EV_SNOOZE, 9 * MIN);
    TEST_ASSERT_FALSE_MESSAGE(alarm_ring_button_short(ar, now), "already snoozing");
    run_ms(9 * MIN - 1000);
    TEST_ASSERT_EQUAL_INT(ALARM_SNOOZED, state());
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    expect_event(ALARM_EV_RING, 0);
    TEST_ASSERT_TRUE(alarm_ring_button_short(ar, now)); /* snooze again */
    expect_event(ALARM_EV_SNOOZE, 9 * MIN);
    run_ms(MIN);
    TEST_ASSERT_TRUE(alarm_ring_button_long(ar, now)); /* off during the snooze */
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    TEST_ASSERT_EQUAL_UINT16(0, alarm_ring_outputs(ar)->light_permille);
    expect_event(ALARM_EV_STOP, ALARM_STOP_BUTTON);
    run_ms(30 * MIN);
    expect_no_event();
    TEST_ASSERT_FALSE(alarm_ring_button_long(ar, now));
    TEST_ASSERT_FALSE(alarm_ring_button_short(ar, now));
}

static void test_long_press_while_ringing(void)
{
    ring_now();
    TEST_ASSERT_TRUE(alarm_ring_button_long(ar, now));
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    expect_event(ALARM_EV_STOP, ALARM_STOP_BUTTON);
}

static void test_buttons_before_the_ring(void)
{
    create();
    alarm_ring_arm(ar, 60 * MIN, now);
    TEST_ASSERT_FALSE(alarm_ring_button_short(ar, now));
    TEST_ASSERT_FALSE_MESSAGE(alarm_ring_button_long(ar, now), "ARMED: the button does nothing");
    TEST_ASSERT_EQUAL_INT(ALARM_ARMED, state());
    run_ms(45 * MIN);
    TEST_ASSERT_EQUAL_INT(ALARM_SUNRISE, state());
    TEST_ASSERT_FALSE(alarm_ring_button_short(ar, now));
    TEST_ASSERT_TRUE_MESSAGE(alarm_ring_button_long(ar, now), "awake already: cancel this alarm");
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    TEST_ASSERT_EQUAL_UINT16(0, alarm_ring_outputs(ar)->light_permille);
    expect_event(ALARM_EV_SUNRISE, 20 * MIN);
    expect_event(ALARM_EV_STOP, ALARM_STOP_BUTTON);
    run_ms(30 * MIN);
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    expect_no_event();
}

static void test_maximum_ring_time(void)
{
    ring_now();
    run_ms(15 * MIN - 1000);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    expect_event(ALARM_EV_STOP, ALARM_STOP_TIMEOUT);
    /* a snooze gives a fresh maximum */
    tearDown();
    ring_now();
    run_ms(14 * MIN);
    alarm_ring_button_short(ar, now);
    run_ms(9 * MIN + 14 * MIN);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    run_ms(MIN);
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
}

static void test_game_blocks_the_buttons(void)
{
    cfg.game = true;
    ring_now();
    TEST_ASSERT_TRUE(alarm_ring_outputs(ar)->game_required);
    TEST_ASSERT_FALSE(alarm_ring_button_short(ar, now));
    TEST_ASSERT_FALSE(alarm_ring_button_long(ar, now));
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    expect_no_event();
    run_ms(2 * MIN);
    alarm_ring_game_won(ar, now);
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    TEST_ASSERT_FALSE(alarm_ring_outputs(ar)->game_required);
    expect_event(ALARM_EV_STOP, ALARM_STOP_GAME);
    alarm_ring_game_won(ar, now); /* nothing rings: no effect */
    expect_no_event();
}

static void test_game_won_needs_a_game(void)
{
    ring_now(); /* no game for this alarm */
    alarm_ring_game_won(ar, now);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_RINGING, state(), "a demo game must not stop a normal alarm");
    expect_no_event();
}

static void test_game_unavailable_gives_the_buttons_back(void)
{
    cfg.game = true;
    ring_now();
    alarm_ring_game_unavailable(ar, now); /* a target went flat during the ring */
    TEST_ASSERT_FALSE(alarm_ring_outputs(ar)->game_required);
    TEST_ASSERT_TRUE(alarm_ring_button_short(ar, now));
    expect_event(ALARM_EV_SNOOZE, 9 * MIN);
    run_ms(9 * MIN);
    expect_event(ALARM_EV_RING, 0); /* still without the game after the snooze */
    TEST_ASSERT_TRUE(alarm_ring_button_long(ar, now));
    expect_event(ALARM_EV_STOP, ALARM_STOP_BUTTON);

    /* known before the ring: the alarm starts without the game */
    alarm_ring_arm(ar, 30 * MIN, now);
    alarm_ring_game_unavailable(ar, now);
    run_ms(30 * MIN);
    expect_event(ALARM_EV_SUNRISE, 20 * MIN);
    expect_event(ALARM_EV_RING, 0);
    TEST_ASSERT_TRUE(alarm_ring_button_long(ar, now));
    expect_event(ALARM_EV_STOP, ALARM_STOP_BUTTON);

    /* the next alarm requires the game again */
    alarm_ring_arm(ar, 30 * MIN, now);
    run_ms(30 * MIN);
    expect_event(ALARM_EV_SUNRISE, 20 * MIN);
    expect_event(ALARM_EV_RING, 1);
    TEST_ASSERT_TRUE(alarm_ring_outputs(ar)->game_required);
}

static void test_safety_nets_work_with_the_game(void)
{
    cfg.game = true;
    ring_now();
    alarm_ring_remote_stop(ar, now); /* emergency button on the speaker */
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    expect_event(ALARM_EV_STOP, ALARM_STOP_REMOTE);
    tearDown();
    ring_now();
    run_ms(15 * MIN);
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    expect_event(ALARM_EV_STOP, ALARM_STOP_TIMEOUT);
    alarm_ring_remote_stop(ar, now); /* idle: nothing */
    expect_no_event();
    /* remote stop also ends a snooze */
    tearDown();
    cfg.game = false;
    ring_now();
    alarm_ring_button_short(ar, now);
    expect_event(ALARM_EV_SNOOZE, 9 * MIN);
    alarm_ring_remote_stop(ar, now);
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    expect_event(ALARM_EV_STOP, ALARM_STOP_REMOTE);
}

static void test_arm_and_disarm_rules(void)
{
    create();
    alarm_ring_arm(ar, 60 * MIN, now);
    run_ms(10 * MIN);
    TEST_ASSERT_TRUE(alarm_ring_arm(ar, 100 * MIN, now)); /* the schedule changed */
    run_ms(79 * MIN);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_ARMED, state(), "the old time was replaced");
    alarm_ring_disarm(ar, now); /* armed switch off */
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    run_ms(60 * MIN);
    expect_no_event();

    alarm_ring_arm(ar, 30 * MIN, now);
    run_ms(15 * MIN);
    expect_event(ALARM_EV_SUNRISE, 20 * MIN);
    alarm_ring_disarm(ar, now); /* during the sunrise */
    TEST_ASSERT_EQUAL_INT(ALARM_IDLE, state());
    TEST_ASSERT_EQUAL_UINT16(0, alarm_ring_outputs(ar)->light_permille);
    expect_event(ALARM_EV_STOP, ALARM_STOP_DISARMED);

    alarm_ring_arm(ar, 0, now);
    expect_event(ALARM_EV_RING, 0);
    alarm_ring_disarm(ar, now);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_RINGING, state(), "the switch cannot silence a ringing alarm");
    TEST_ASSERT_FALSE_MESSAGE(alarm_ring_arm(ar, 60 * MIN, now), "not while ringing");
    alarm_ring_button_short(ar, now);
    alarm_ring_disarm(ar, now);
    TEST_ASSERT_EQUAL_INT(ALARM_SNOOZED, state());
    TEST_ASSERT_FALSE(alarm_ring_arm(ar, 60 * MIN, now));
}

static void test_new_config_applies_from_the_next_arm(void)
{
    create();
    alarm_ring_arm(ar, 60 * MIN, now);
    alarm_ring_config_t c = cfg;
    c.sunrise_ms = 0;
    c.snooze_ms = 2 * MIN;
    c.game = true;
    TEST_ASSERT_TRUE(alarm_ring_set_config(ar, &c));
    run_ms(60 * MIN);
    expect_event(ALARM_EV_SUNRISE, 20 * MIN); /* the armed alarm kept its plan */
    expect_event(ALARM_EV_RING, 0);
    alarm_ring_button_long(ar, now);
    expect_event(ALARM_EV_STOP, ALARM_STOP_BUTTON);
    alarm_ring_arm(ar, 5 * MIN, now);
    run_ms(5 * MIN);
    expect_event(ALARM_EV_RING, 1);
}

static void test_late_tick_and_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 30u * MIN;
    create();
    alarm_ring_arm(ar, 60 * MIN, now);
    now += 61 * MIN; /* nothing called for an hour; the counter wrapped */
    alarm_ring_tick(ar, now);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    expect_event(ALARM_EV_SUNRISE, 20 * MIN);
    expect_event(ALARM_EV_RING, 0);
    run_ms(14 * MIN - 1000);
    TEST_ASSERT_EQUAL_INT(ALARM_RINGING, state());
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ALARM_IDLE, state(), "ring time counts from the planned ring");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_sunrise_then_ring);
    RUN_TEST(test_short_notice_shortens_the_sunrise);
    RUN_TEST(test_no_sunrise_when_switched_off);
    RUN_TEST(test_snooze_and_off);
    RUN_TEST(test_long_press_while_ringing);
    RUN_TEST(test_buttons_before_the_ring);
    RUN_TEST(test_maximum_ring_time);
    RUN_TEST(test_game_blocks_the_buttons);
    RUN_TEST(test_game_won_needs_a_game);
    RUN_TEST(test_game_unavailable_gives_the_buttons_back);
    RUN_TEST(test_safety_nets_work_with_the_game);
    RUN_TEST(test_arm_and_disarm_rules);
    RUN_TEST(test_new_config_applies_from_the_next_arm);
    RUN_TEST(test_late_tick_and_clock_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
