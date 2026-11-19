/*
 * Tests for lib/armed_alarm – requirements BED-10 ... BED-16.
 * The helper ticks once per second.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "armed_alarm.h"

#define UTC0 1795000000u

static armed_alarm_t *aa;
static uint32_t now;
static link_time_alarm_t copy;

void setUp(void)
{
    aa = NULL;
    now = 5000000u;
    memset(&copy, 0, sizeof(copy));
    copy.version = 10;
    copy.utc_now_s = UTC0;
    copy.alarm_utc_s = UTC0 + 3600;
    copy.sound = 3;
    copy.volume = 20;
    copy.fade_s = 60;
    copy.ring_max_min = 15;
    copy.game = false;
}

void tearDown(void)
{
    armed_alarm_destroy(aa);
    aa = NULL;
}

static void create(void)
{
    aa = armed_alarm_create(now);
    TEST_ASSERT_NOT_NULL_MESSAGE(aa, "armed_alarm_create returned NULL");
}

static void run_s(uint32_t seconds)
{
    for (uint32_t i = 0; i < seconds; i++) {
        now += 1000;
        armed_alarm_tick(aa, now);
    }
}

static void expect_ring(void)
{
    armed_event_t e;
    TEST_ASSERT_TRUE_MESSAGE(armed_alarm_take_event(aa, &e), "ring event missing");
    TEST_ASSERT_EQUAL_INT(ARMED_EV_RING, e.id);
    TEST_ASSERT_EQUAL_UINT8(copy.sound, e.sound);
    TEST_ASSERT_EQUAL_UINT8(copy.volume, e.volume);
    TEST_ASSERT_EQUAL_UINT16(copy.fade_s, e.fade_s);
    TEST_ASSERT_EQUAL_INT(copy.game, e.game);
}

static void expect_stop(armed_stop_t reason)
{
    armed_event_t e;
    TEST_ASSERT_TRUE_MESSAGE(armed_alarm_take_event(aa, &e), "stop event missing");
    TEST_ASSERT_EQUAL_INT(ARMED_EV_STOP, e.id);
    TEST_ASSERT_EQUAL_INT(reason, e.reason);
}

static void expect_no_event(void)
{
    armed_event_t e;
    TEST_ASSERT_FALSE(armed_alarm_take_event(aa, &e));
}

static void arm_and_ring(void)
{
    create();
    TEST_ASSERT_TRUE(armed_alarm_update(aa, &copy, now));
    run_s(3600);
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    expect_ring();
}

static void test_starts_idle(void)
{
    create();
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    TEST_ASSERT_EQUAL_UINT32(0, armed_alarm_version(aa));
    TEST_ASSERT_EQUAL_UINT32(0, armed_alarm_utc(aa, now));
    run_s(100);
    expect_no_event();
    armed_alarm_destroy(NULL);
}

static void test_copy_arms_and_the_clock_runs_on(void)
{
    create();
    TEST_ASSERT_TRUE(armed_alarm_update(aa, &copy, now));
    TEST_ASSERT_EQUAL_INT(ARMED_WAITING, armed_alarm_state(aa));
    TEST_ASSERT_EQUAL_UINT32(10, armed_alarm_version(aa));
    TEST_ASSERT_EQUAL_UINT32(UTC0, armed_alarm_utc(aa, now));
    run_s(125);
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 125, armed_alarm_utc(aa, now));
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 125, armed_alarm_utc(aa, now + 999));
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 126, armed_alarm_utc(aa, now + 1000));
}

static void test_rings_by_itself_at_the_alarm_time(void)
{
    create();
    armed_alarm_update(aa, &copy, now);
    run_s(3599);
    TEST_ASSERT_EQUAL_INT(ARMED_WAITING, armed_alarm_state(aa));
    expect_no_event();
    run_s(1); /* no further message from the bedside: the link may be down */
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    expect_ring();
    expect_no_event();
    run_s(30);
    TEST_ASSERT_EQUAL_UINT16(30, armed_alarm_ring_seconds(aa, now));
}

static void test_heartbeats_correct_the_clock(void)
{
    create();
    armed_alarm_update(aa, &copy, now);
    run_s(1800);
    /* the speaker's clock ran slow: the bedside says 40 s more have passed */
    copy.utc_now_s = UTC0 + 1840;
    TEST_ASSERT_TRUE_MESSAGE(armed_alarm_update(aa, &copy, now), "same version: accepted as a refresh");
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 1840, armed_alarm_utc(aa, now));
    run_s(1759);
    TEST_ASSERT_EQUAL_INT(ARMED_WAITING, armed_alarm_state(aa));
    run_s(1);
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
}

static void test_older_versions_are_discarded(void)
{
    create();
    armed_alarm_update(aa, &copy, now);
    link_time_alarm_t old = copy;
    old.version = 9;
    old.alarm_utc_s = UTC0 + 60;
    TEST_ASSERT_FALSE(armed_alarm_update(aa, &old, now));
    TEST_ASSERT_EQUAL_UINT32(10, armed_alarm_version(aa));
    run_s(120);
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARMED_WAITING, armed_alarm_state(aa), "the old alarm time was not taken");

    link_time_alarm_t newer = copy;
    newer.version = 11;
    newer.utc_now_s = UTC0 + 120;
    newer.alarm_utc_s = UTC0 + 180;
    TEST_ASSERT_TRUE(armed_alarm_update(aa, &newer, now));
    run_s(60);
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    TEST_ASSERT_FALSE(armed_alarm_update(aa, NULL, now));
}

static void test_no_alarm_or_past_alarm_is_not_armed(void)
{
    create();
    copy.alarm_utc_s = 0;
    armed_alarm_update(aa, &copy, now);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    copy.version = 11;
    copy.alarm_utc_s = UTC0; /* not later than now */
    armed_alarm_update(aa, &copy, now);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    copy.version = 12;
    copy.alarm_utc_s = UTC0 - 100;
    armed_alarm_update(aa, &copy, now);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    run_s(200);
    expect_no_event();
    /* a cancelled alarm: armed, then the bedside sends "none" */
    copy.version = 13;
    copy.utc_now_s = UTC0 + 200;
    copy.alarm_utc_s = UTC0 + 500;
    armed_alarm_update(aa, &copy, now);
    TEST_ASSERT_EQUAL_INT(ARMED_WAITING, armed_alarm_state(aa));
    copy.version = 14;
    copy.alarm_utc_s = 0;
    armed_alarm_update(aa, &copy, now);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    run_s(1000);
    expect_no_event();
}

static void test_start_command_from_the_bedside(void)
{
    create();
    copy.game = true;
    armed_alarm_update(aa, &copy, now);
    run_s(3590); /* the bedside clock is 10 s ahead and says start */
    TEST_ASSERT_TRUE(armed_alarm_ring(aa, LINK_RING_START, 0, now));
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    expect_ring();
    TEST_ASSERT_FALSE_MESSAGE(armed_alarm_ring(aa, LINK_RING_START, 0, now), "already ringing");
    run_s(20); /* its own alarm time passes while ringing: no second ring */
    expect_no_event();
    TEST_ASSERT_EQUAL_UINT16(20, armed_alarm_ring_seconds(aa, now));
}

static void test_start_without_any_copy_uses_safe_values(void)
{
    create();
    TEST_ASSERT_TRUE(armed_alarm_ring(aa, LINK_RING_START, 0, now));
    armed_event_t e;
    TEST_ASSERT_TRUE(armed_alarm_take_event(aa, &e));
    TEST_ASSERT_EQUAL_INT(ARMED_EV_RING, e.id);
    TEST_ASSERT_EQUAL_UINT8(0, e.sound);
    TEST_ASSERT_EQUAL_UINT8(16, e.volume);
    TEST_ASSERT_EQUAL_UINT16(0, e.fade_s);
    TEST_ASSERT_FALSE(e.game);
    run_s(15 * 60 - 1);
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    run_s(1);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    expect_stop(ARMED_STOP_TIMEOUT);
}

static void test_stop_from_the_bedside(void)
{
    arm_and_ring();
    run_s(5);
    TEST_ASSERT_TRUE(armed_alarm_ring(aa, LINK_RING_STOP, 0, now));
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    expect_stop(ARMED_STOP_REMOTE);
    TEST_ASSERT_FALSE(armed_alarm_ring(aa, LINK_RING_STOP, 0, now));
    /* the heartbeat still carries the alarm that just rang: it must not ring again */
    copy.utc_now_s = UTC0 + 3606;
    armed_alarm_update(aa, &copy, now);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    run_s(4000);
    expect_no_event();
}

static void test_emergency_button(void)
{
    create();
    TEST_ASSERT_FALSE(armed_alarm_local_stop(aa, now)); /* nothing rings */
    tearDown();
    arm_and_ring();
    TEST_ASSERT_TRUE(armed_alarm_local_stop(aa, now));
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    expect_stop(ARMED_STOP_LOCAL);
    TEST_ASSERT_FALSE(armed_alarm_local_stop(aa, now));
}

static void test_maximum_ring_time(void)
{
    copy.ring_max_min = 2;
    arm_and_ring();
    run_s(119);
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    run_s(1);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    expect_stop(ARMED_STOP_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT16(0, armed_alarm_ring_seconds(aa, now));
}

static void test_snooze(void)
{
    copy.ring_max_min = 2;
    arm_and_ring();
    run_s(100);
    TEST_ASSERT_FALSE(armed_alarm_ring(aa, LINK_RING_SNOOZE, 0, now));
    TEST_ASSERT_FALSE(armed_alarm_ring(aa, LINK_RING_SNOOZE, 61, now));
    TEST_ASSERT_TRUE(armed_alarm_ring(aa, LINK_RING_SNOOZE, 9, now));
    TEST_ASSERT_EQUAL_INT(ARMED_SNOOZED, armed_alarm_state(aa));
    armed_event_t e;
    TEST_ASSERT_TRUE(armed_alarm_take_event(aa, &e));
    TEST_ASSERT_EQUAL_INT(ARMED_EV_SNOOZE, e.id);
    TEST_ASSERT_FALSE_MESSAGE(armed_alarm_ring(aa, LINK_RING_SNOOZE, 9, now), "only while ringing");
    run_s(9 * 60 - 1);
    TEST_ASSERT_EQUAL_INT(ARMED_SNOOZED, armed_alarm_state(aa));
    expect_no_event();
    run_s(1); /* the bedside is silent: the speaker rings again on its own */
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    expect_ring();
    run_s(119); /* a fresh maximum ring time */
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    run_s(1);
    expect_stop(ARMED_STOP_TIMEOUT);

    /* a stop works during the snooze too */
    tearDown();
    setUp();
    arm_and_ring();
    armed_alarm_ring(aa, LINK_RING_SNOOZE, 5, now);
    run_s(60);
    TEST_ASSERT_TRUE(armed_alarm_local_stop(aa, now));
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
    run_s(600);
    TEST_ASSERT_EQUAL_INT(ARMED_IDLE, armed_alarm_state(aa));
}

static void test_update_during_the_ring_arms_the_next_alarm(void)
{
    arm_and_ring();
    link_time_alarm_t next = copy;
    next.version = 11;
    next.utc_now_s = UTC0 + 3600;
    next.alarm_utc_s = UTC0 + 3600 + 600; /* the next alarm, ten minutes later */
    next.sound = 7;
    TEST_ASSERT_TRUE(armed_alarm_update(aa, &next, now));
    TEST_ASSERT_EQUAL_INT_MESSAGE(ARMED_RINGING, armed_alarm_state(aa), "the running ring goes on");
    TEST_ASSERT_EQUAL_UINT32(11, armed_alarm_version(aa));
    run_s(60);
    armed_alarm_ring(aa, LINK_RING_STOP, 0, now);
    expect_stop(ARMED_STOP_REMOTE);
    TEST_ASSERT_EQUAL_INT(ARMED_WAITING, armed_alarm_state(aa));
    run_s(540);
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    copy = next;
    expect_ring();
}

static void test_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 1800u * 1000u; /* the counter wraps half way to the alarm */
    create();
    armed_alarm_update(aa, &copy, now);
    run_s(3599);
    TEST_ASSERT_EQUAL_INT(ARMED_WAITING, armed_alarm_state(aa));
    run_s(1);
    TEST_ASSERT_EQUAL_INT(ARMED_RINGING, armed_alarm_state(aa));
    TEST_ASSERT_EQUAL_UINT32(UTC0 + 3600, armed_alarm_utc(aa, now));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_idle);
    RUN_TEST(test_copy_arms_and_the_clock_runs_on);
    RUN_TEST(test_rings_by_itself_at_the_alarm_time);
    RUN_TEST(test_heartbeats_correct_the_clock);
    RUN_TEST(test_older_versions_are_discarded);
    RUN_TEST(test_no_alarm_or_past_alarm_is_not_armed);
    RUN_TEST(test_start_command_from_the_bedside);
    RUN_TEST(test_start_without_any_copy_uses_safe_values);
    RUN_TEST(test_stop_from_the_bedside);
    RUN_TEST(test_emergency_button);
    RUN_TEST(test_maximum_ring_time);
    RUN_TEST(test_snooze);
    RUN_TEST(test_update_during_the_ring_arms_the_next_alarm);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
