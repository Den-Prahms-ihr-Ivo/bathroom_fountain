/*
 * Tests for lib/bed_display – the bedside display, decided 2026-11-23.
 * Needs only alarm_ring.h for alarm_state_t (no alarm_ring code runs).
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unity.h>

#include "bed_display.h"

#define SEC 1000u
#define MIN 60000u
#define AUTO BED_DISPLAY_AUTO_MS

static bed_display_t d;
static bed_display_cfg_t cfg;
static uint32_t t0;

void setUp(void)
{
    cfg.press_ms = 10 * SEC;
    cfg.sunrise_auto = true;
    memset(&d, 0x5A, sizeof(d));
    bed_display_init(&d, &cfg);
    t0 = 500000000u;
}

void tearDown(void) {}

static bool on(uint32_t at, alarm_state_t a)
{
    bool r = bed_display_update(&d, at, a);
    TEST_ASSERT_EQUAL(r, bed_display_on(&d, at));
    return r;
}

/* ---- on / off ------------------------------------------------------------------ */

static void test_dark_after_init(void)
{
    TEST_ASSERT_FALSE(bed_display_on(&d, t0));
    TEST_ASSERT_FALSE(on(t0, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 1 * MIN, ALARM_ARMED));
    TEST_ASSERT_FALSE(on(t0 + 60 * MIN, ALARM_ARMED));
}

static void test_press_opens_for_press_ms(void)
{
    on(t0, ALARM_IDLE);
    bed_display_press(&d, t0 + 1000);
    TEST_ASSERT_TRUE(bed_display_on(&d, t0 + 1000));
    TEST_ASSERT_TRUE(on(t0 + 1000, ALARM_IDLE));
    TEST_ASSERT_TRUE(on(t0 + 1000 + 10 * SEC - 1, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 1000 + 10 * SEC, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 1000 + 20 * SEC, ALARM_IDLE));
}

static void test_press_restarts_window(void)
{
    on(t0, ALARM_IDLE);
    bed_display_press(&d, t0);
    bed_display_press(&d, t0 + 8 * SEC);
    TEST_ASSERT_TRUE(on(t0 + 17 * SEC, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 18 * SEC, ALARM_IDLE));
}

static void test_press_works_in_every_state(void)
{
    const alarm_state_t states[] = {ALARM_IDLE, ALARM_ARMED, ALARM_SUNRISE, ALARM_RINGING, ALARM_SNOOZED};
    cfg.sunrise_auto = false;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        bed_display_init(&d, &cfg);
        uint32_t t = t0 + (uint32_t)i * 100 * MIN;
        on(t, states[i]);
        on(t + AUTO + 1, states[i]); /* any auto window has run out */
        TEST_ASSERT_FALSE(on(t + AUTO + 2, states[i]));
        bed_display_press(&d, t + AUTO + 3);
        TEST_ASSERT_TRUE(on(t + AUTO + 3, states[i]));
        TEST_ASSERT_FALSE(on(t + AUTO + 3 + 10 * SEC, states[i]));
    }
}

static void test_sunrise_opens_auto_window(void)
{
    on(t0, ALARM_ARMED);
    TEST_ASSERT_TRUE(on(t0 + 1 * MIN, ALARM_SUNRISE));
    TEST_ASSERT_TRUE(on(t0 + 1 * MIN + AUTO - 1, ALARM_SUNRISE));
    TEST_ASSERT_FALSE(on(t0 + 1 * MIN + AUTO, ALARM_SUNRISE)); /* sunrise still runs, display dark */
    TEST_ASSERT_FALSE(on(t0 + 1 * MIN + AUTO + 5 * MIN, ALARM_SUNRISE));
}

static void test_sunrise_auto_off(void)
{
    cfg.sunrise_auto = false;
    bed_display_init(&d, &cfg);
    on(t0, ALARM_ARMED);
    TEST_ASSERT_FALSE(on(t0 + 1 * MIN, ALARM_SUNRISE));
    TEST_ASSERT_FALSE(on(t0 + 5 * MIN, ALARM_SUNRISE));
    bed_display_press(&d, t0 + 6 * MIN); /* a press always shows it */
    TEST_ASSERT_TRUE(on(t0 + 6 * MIN, ALARM_SUNRISE));
    TEST_ASSERT_FALSE(on(t0 + 6 * MIN + 10 * SEC, ALARM_SUNRISE));
    /* the ring switches it on regardless */
    TEST_ASSERT_TRUE(on(t0 + 20 * MIN, ALARM_RINGING));
    TEST_ASSERT_TRUE(on(t0 + 20 * MIN + AUTO - 1, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 20 * MIN + AUTO, ALARM_RINGING));
}

static void test_ring_without_sunrise(void)
{
    on(t0, ALARM_ARMED);
    TEST_ASSERT_TRUE(on(t0 + 1, ALARM_RINGING));
    TEST_ASSERT_TRUE(on(t0 + 1 + AUTO - 1, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 1 + AUTO, ALARM_RINGING));
}

static void test_ring_opens_fresh_window_after_sunrise(void)
{
    on(t0, ALARM_ARMED);
    on(t0 + 1, ALARM_SUNRISE);                          /* window until t0+1+10 min */
    TEST_ASSERT_FALSE(on(t0 + 15 * MIN, ALARM_SUNRISE)); /* ran out */
    TEST_ASSERT_TRUE(on(t0 + 20 * MIN, ALARM_RINGING));
    TEST_ASSERT_TRUE(on(t0 + 30 * MIN - 1, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 30 * MIN, ALARM_RINGING));
}

static void test_ring_restarts_window_still_open(void)
{
    on(t0, ALARM_ARMED);
    on(t0, ALARM_SUNRISE);
    TEST_ASSERT_TRUE(on(t0 + 5 * MIN, ALARM_RINGING)); /* shortened sunrise */
    TEST_ASSERT_TRUE(on(t0 + 15 * MIN - 1, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 15 * MIN, ALARM_RINGING));
}

static void test_staying_in_state_does_not_reopen(void)
{
    on(t0, ALARM_ARMED);
    for (uint32_t t = 0; t <= 20 * MIN; t += 20) {
        bool r = on(t0 + 1 + t, ALARM_RINGING);
        TEST_ASSERT_EQUAL(t < AUTO, r);
    }
}

static void test_snooze_keeps_window(void)
{
    on(t0, ALARM_ARMED);
    on(t0, ALARM_RINGING);                              /* window until t0+10 min */
    bed_display_press(&d, t0 + 2 * MIN);                /* the snooze press */
    TEST_ASSERT_TRUE(on(t0 + 2 * MIN, ALARM_SNOOZED));
    TEST_ASSERT_TRUE(on(t0 + 10 * MIN - 1, ALARM_SNOOZED));
    TEST_ASSERT_FALSE(on(t0 + 10 * MIN, ALARM_SNOOZED)); /* snooze still runs */
    /* back from the snooze: fresh window */
    TEST_ASSERT_TRUE(on(t0 + 11 * MIN, ALARM_RINGING));
    TEST_ASSERT_TRUE(on(t0 + 21 * MIN - 1, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 21 * MIN, ALARM_RINGING));
}

static void test_snooze_after_window_ran_out(void)
{
    on(t0, ALARM_ARMED);
    on(t0, ALARM_RINGING);
    TEST_ASSERT_FALSE(on(t0 + 12 * MIN, ALARM_RINGING));
    bed_display_press(&d, t0 + 12 * MIN);
    TEST_ASSERT_TRUE(on(t0 + 12 * MIN, ALARM_SNOOZED));
    TEST_ASSERT_TRUE(on(t0 + 12 * MIN + 10 * SEC - 1, ALARM_SNOOZED));
    TEST_ASSERT_FALSE(on(t0 + 12 * MIN + 10 * SEC, ALARM_SNOOZED));
}

static void test_press_after_window_ran_out_while_ringing(void)
{
    on(t0, ALARM_ARMED);
    on(t0, ALARM_RINGING);
    TEST_ASSERT_FALSE(on(t0 + 11 * MIN, ALARM_RINGING));
    bed_display_press(&d, t0 + 11 * MIN);
    TEST_ASSERT_TRUE(on(t0 + 11 * MIN + 9 * SEC, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 11 * MIN + 10 * SEC, ALARM_RINGING));
}

static void test_press_inside_auto_window(void)
{
    on(t0, ALARM_ARMED);
    on(t0, ALARM_RINGING);
    bed_display_press(&d, t0 + 1 * MIN);
    /* the press window ends first, the auto window keeps it on */
    TEST_ASSERT_TRUE(on(t0 + 1 * MIN + 10 * SEC, ALARM_RINGING));
    TEST_ASSERT_TRUE(on(t0 + 10 * MIN - 1, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 10 * MIN, ALARM_RINGING));
    /* a press near the end outlives the auto window */
    bed_display_init(&d, &cfg);
    on(t0, ALARM_ARMED);
    on(t0, ALARM_RINGING);
    bed_display_press(&d, t0 + 10 * MIN - 1 * SEC);
    TEST_ASSERT_TRUE(on(t0 + 10 * MIN + 8 * SEC, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + 10 * MIN + 9 * SEC, ALARM_RINGING));
}

static void test_alarm_end_closes_auto_window(void)
{
    const alarm_state_t ends[] = {ALARM_IDLE, ALARM_ARMED};
    const alarm_state_t from[] = {ALARM_SUNRISE, ALARM_RINGING, ALARM_SNOOZED};
    for (size_t e = 0; e < 2; e++) {
        for (size_t f = 0; f < 3; f++) {
            bed_display_init(&d, &cfg);
            on(t0, ALARM_ARMED);
            on(t0 + 1, ALARM_RINGING);
            on(t0 + 2, from[f]);
            TEST_ASSERT_TRUE(on(t0 + 3, from[f]));
            TEST_ASSERT_FALSE(on(t0 + 4, ends[e]));
            TEST_ASSERT_FALSE(on(t0 + 5 * MIN, ends[e]));
        }
    }
}

static void test_stop_press_keeps_press_window(void)
{
    on(t0, ALARM_ARMED);
    on(t0, ALARM_RINGING);
    bed_display_press(&d, t0 + 3 * MIN);               /* the long press that stops it */
    TEST_ASSERT_TRUE(on(t0 + 3 * MIN, ALARM_IDLE));
    TEST_ASSERT_TRUE(on(t0 + 3 * MIN + 10 * SEC - 1, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 3 * MIN + 10 * SEC, ALARM_IDLE));
}

static void test_press_and_update_any_order(void)
{
    /* update first, then press */
    on(t0, ALARM_ARMED);
    on(t0 + 1 * MIN, ALARM_RINGING);
    TEST_ASSERT_FALSE(on(t0 + 12 * MIN, ALARM_RINGING));
    on(t0 + 13 * MIN, ALARM_IDLE);
    bed_display_press(&d, t0 + 13 * MIN);
    TEST_ASSERT_TRUE(bed_display_on(&d, t0 + 13 * MIN));
    /* press first, then update */
    bed_display_init(&d, &cfg);
    on(t0, ALARM_ARMED);
    on(t0 + 1 * MIN, ALARM_RINGING);
    TEST_ASSERT_FALSE(on(t0 + 12 * MIN, ALARM_RINGING));
    bed_display_press(&d, t0 + 13 * MIN);
    TEST_ASSERT_TRUE(on(t0 + 13 * MIN, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 13 * MIN + 10 * SEC, ALARM_IDLE));
}

static void test_boot_while_ringing(void)
{
    TEST_ASSERT_TRUE(on(t0, ALARM_RINGING)); /* first update after init */
    TEST_ASSERT_FALSE(on(t0 + AUTO, ALARM_RINGING));
}

static void test_boot_while_snoozed_stays_dark(void)
{
    TEST_ASSERT_FALSE(on(t0, ALARM_SNOOZED)); /* only sunrise and ring open it */
}

static void test_wraparound(void)
{
    t0 = 0xFFFFFFFFu - 5 * SEC;
    on(t0, ALARM_IDLE);
    bed_display_press(&d, t0);
    TEST_ASSERT_TRUE(on(t0 + 9 * SEC, ALARM_IDLE)); /* past the wrap */
    TEST_ASSERT_FALSE(on(t0 + 10 * SEC, ALARM_IDLE));

    bed_display_init(&d, &cfg);
    t0 = 0xFFFFFFFFu - 1 * MIN;
    on(t0, ALARM_ARMED);
    on(t0, ALARM_RINGING);
    TEST_ASSERT_TRUE(on(t0 + AUTO - 1, ALARM_RINGING));
    TEST_ASSERT_FALSE(on(t0 + AUTO, ALARM_RINGING));
}

static void test_long_quiet_time_stays_dark(void)
{
    on(t0, ALARM_IDLE);
    bed_display_press(&d, t0);
    on(t0 + 10 * SEC, ALARM_IDLE);
    /* 2^31 ms later the old window must not look open again */
    TEST_ASSERT_FALSE(on(t0 + 0x80000000u, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 0xFFFFF000u, ALARM_IDLE));
}

static void test_cfg_change_applies_to_new_windows(void)
{
    on(t0, ALARM_IDLE);
    bed_display_press(&d, t0);
    bed_display_cfg_t c2 = {.press_ms = 60 * SEC, .sunrise_auto = false};
    bed_display_set_cfg(&d, &c2);
    TEST_ASSERT_TRUE(on(t0 + 9 * SEC, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 10 * SEC, ALARM_IDLE)); /* old window keeps its end */
    bed_display_press(&d, t0 + 1 * MIN);
    TEST_ASSERT_TRUE(on(t0 + 1 * MIN + 59 * SEC, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 2 * MIN, ALARM_IDLE));
    on(t0 + 3 * MIN, ALARM_ARMED);
    TEST_ASSERT_FALSE(on(t0 + 4 * MIN, ALARM_SUNRISE)); /* sunrise_auto now off */
}

static void test_settings_limits(void)
{
    bed_display_cfg_t c = {.press_ms = 3 * SEC, .sunrise_auto = true};
    bed_display_init(&d, &c);
    bed_display_press(&d, t0);
    TEST_ASSERT_TRUE(on(t0 + 3 * SEC - 1, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 3 * SEC, ALARM_IDLE));
    c.press_ms = 120 * SEC;
    bed_display_init(&d, &c);
    bed_display_press(&d, t0);
    TEST_ASSERT_TRUE(on(t0 + 120 * SEC - 1, ALARM_IDLE));
    TEST_ASSERT_FALSE(on(t0 + 120 * SEC, ALARM_IDLE));
}

/* ---- content ------------------------------------------------------------------- */

static struct tm tm_at(int wday, int mday, int mon, int hour, int min)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 126;
    t.tm_wday = wday;
    t.tm_mday = mday;
    t.tm_mon = mon;
    t.tm_hour = hour;
    t.tm_min = min;
    return t;
}

static bed_display_text_t txt;

static void text(const struct tm *now, const struct tm *alarm, alarm_state_t st, uint32_t left)
{
    memset(&txt, 0x77, sizeof(txt));
    bed_display_text(now, alarm, st, left, &txt);
}

static void test_text_time(void)
{
    struct tm n = tm_at(1, 23, 10, 6, 5);
    text(&n, NULL, ALARM_IDLE, 0);
    TEST_ASSERT_EQUAL_STRING("6:05", txt.big);
    n = tm_at(1, 23, 10, 23, 59);
    text(&n, NULL, ALARM_ARMED, 0);
    TEST_ASSERT_EQUAL_STRING("23:59", txt.big);
    n = tm_at(1, 23, 10, 0, 0);
    text(&n, NULL, ALARM_RINGING, 0);
    TEST_ASSERT_EQUAL_STRING("0:00", txt.big);
    n = tm_at(1, 23, 10, 12, 30);
    text(&n, NULL, ALARM_SUNRISE, 5000); /* snooze_left ignored unless snoozed */
    TEST_ASSERT_EQUAL_STRING("12:30", txt.big);
}

static void test_text_time_unknown(void)
{
    text(NULL, NULL, ALARM_IDLE, 0);
    TEST_ASSERT_EQUAL_STRING("--:--", txt.big);
    TEST_ASSERT_EQUAL_STRING("", txt.date);
    TEST_ASSERT_EQUAL_STRING("", txt.alarm);
    TEST_ASSERT_FALSE(txt.alarm_icon);
}

static void test_text_snooze_countdown(void)
{
    struct tm n = tm_at(1, 23, 10, 6, 9);
    text(&n, NULL, ALARM_SNOOZED, 522000);
    TEST_ASSERT_EQUAL_STRING("8:42", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 521001); /* rounded up */
    TEST_ASSERT_EQUAL_STRING("8:42", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 522001);
    TEST_ASSERT_EQUAL_STRING("8:43", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 4100);
    TEST_ASSERT_EQUAL_STRING("0:05", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 1);
    TEST_ASSERT_EQUAL_STRING("0:01", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 0);
    TEST_ASSERT_EQUAL_STRING("0:00", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 12 * MIN);
    TEST_ASSERT_EQUAL_STRING("12:00", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 30 * MIN);
    TEST_ASSERT_EQUAL_STRING("30:00", txt.big);
    TEST_ASSERT_EQUAL_STRING("23 Nov", txt.date); /* the bottom line stays */
    text(&n, NULL, ALARM_SNOOZED, 100 * MIN);
    TEST_ASSERT_EQUAL_STRING("99:59", txt.big);
    text(&n, NULL, ALARM_SNOOZED, 0xFFFFFFFFu);
    TEST_ASSERT_EQUAL_STRING("99:59", txt.big);
}

static void test_text_snooze_without_clock(void)
{
    text(NULL, NULL, ALARM_SNOOZED, 90 * SEC);
    TEST_ASSERT_EQUAL_STRING("1:30", txt.big);
    TEST_ASSERT_EQUAL_STRING("", txt.date);
}

static void test_text_next_alarm(void)
{
    struct tm n = tm_at(0, 22, 10, 23, 0);
    struct tm a = tm_at(1, 23, 10, 6, 0);
    text(&n, &a, ALARM_ARMED, 0);
    TEST_ASSERT_TRUE(txt.alarm_icon);
    TEST_ASSERT_EQUAL_STRING("Mon 6:00", txt.alarm);
    a = tm_at(0, 29, 10, 23, 30);
    text(&n, &a, ALARM_ARMED, 0);
    TEST_ASSERT_EQUAL_STRING("Sun 23:30", txt.alarm);
    const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    char want[12];
    for (int w = 0; w < 7; w++) {
        a = tm_at(w, 23, 10, 7, 15);
        text(&n, &a, ALARM_IDLE, 0);
        strcpy(want, days[w]);
        strcat(want, " 7:15");
        TEST_ASSERT_EQUAL_STRING(want, txt.alarm);
    }
    /* the next alarm shows while another one rings, too */
    text(&n, &a, ALARM_RINGING, 0);
    TEST_ASSERT_TRUE(txt.alarm_icon);
}

static void test_text_no_alarm(void)
{
    struct tm n = tm_at(1, 23, 10, 6, 5);
    text(&n, NULL, ALARM_IDLE, 0);
    TEST_ASSERT_FALSE(txt.alarm_icon);
    TEST_ASSERT_EQUAL_STRING("", txt.alarm);
}

static void test_text_date(void)
{
    const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char want[8];
    for (int m = 0; m < 12; m++) {
        struct tm n = tm_at(3, 1, m, 9, 0);
        text(&n, NULL, ALARM_IDLE, 0);
        strcpy(want, "1 ");
        strcat(want, months[m]);
        TEST_ASSERT_EQUAL_STRING(want, txt.date);
    }
    struct tm n = tm_at(1, 23, 10, 9, 0);
    text(&n, NULL, ALARM_IDLE, 0);
    TEST_ASSERT_EQUAL_STRING("23 Nov", txt.date);
    n = tm_at(4, 31, 11, 9, 0);
    text(&n, NULL, ALARM_IDLE, 0);
    TEST_ASSERT_EQUAL_STRING("31 Dec", txt.date);
}

static void test_text_out_of_range(void)
{
    const struct tm good = tm_at(1, 23, 10, 6, 5);
    struct tm bad[8];
    for (int i = 0; i < 8; i++) {
        bad[i] = good;
    }
    bad[0].tm_wday = 7;
    bad[1].tm_wday = -1;
    bad[2].tm_mon = 12;
    bad[3].tm_mday = 0;
    bad[4].tm_mday = 32;
    bad[5].tm_hour = 24;
    bad[6].tm_min = 60;
    bad[7].tm_min = -1;
    for (int i = 0; i < 8; i++) {
        text(&bad[i], &good, ALARM_IDLE, 0);
        TEST_ASSERT_EQUAL_STRING("--:--", txt.big);
        TEST_ASSERT_EQUAL_STRING("", txt.date);
        TEST_ASSERT_EQUAL_STRING("Mon 6:05", txt.alarm);
        text(&good, &bad[i], ALARM_IDLE, 0);
        TEST_ASSERT_EQUAL_STRING("6:05", txt.big);
        TEST_ASSERT_EQUAL_STRING("", txt.alarm);
        TEST_ASSERT_FALSE(txt.alarm_icon);
    }
}

static void test_text_strings_terminated(void)
{
    struct tm n = tm_at(3, 30, 8, 23, 59);
    struct tm a = tm_at(3, 30, 8, 23, 59);
    text(&n, &a, ALARM_SNOOZED, 30 * MIN);
    TEST_ASSERT_TRUE(memchr(txt.big, '\0', sizeof(txt.big)) != NULL);
    TEST_ASSERT_TRUE(memchr(txt.alarm, '\0', sizeof(txt.alarm)) != NULL);
    TEST_ASSERT_TRUE(memchr(txt.date, '\0', sizeof(txt.date)) != NULL);
    TEST_ASSERT_EQUAL_STRING("Wed 23:59", txt.alarm);
    TEST_ASSERT_EQUAL_STRING("30 Sep", txt.date);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dark_after_init);
    RUN_TEST(test_press_opens_for_press_ms);
    RUN_TEST(test_press_restarts_window);
    RUN_TEST(test_press_works_in_every_state);
    RUN_TEST(test_sunrise_opens_auto_window);
    RUN_TEST(test_sunrise_auto_off);
    RUN_TEST(test_ring_without_sunrise);
    RUN_TEST(test_ring_opens_fresh_window_after_sunrise);
    RUN_TEST(test_ring_restarts_window_still_open);
    RUN_TEST(test_staying_in_state_does_not_reopen);
    RUN_TEST(test_snooze_keeps_window);
    RUN_TEST(test_snooze_after_window_ran_out);
    RUN_TEST(test_press_after_window_ran_out_while_ringing);
    RUN_TEST(test_press_inside_auto_window);
    RUN_TEST(test_alarm_end_closes_auto_window);
    RUN_TEST(test_stop_press_keeps_press_window);
    RUN_TEST(test_press_and_update_any_order);
    RUN_TEST(test_boot_while_ringing);
    RUN_TEST(test_boot_while_snoozed_stays_dark);
    RUN_TEST(test_wraparound);
    RUN_TEST(test_long_quiet_time_stays_dark);
    RUN_TEST(test_cfg_change_applies_to_new_windows);
    RUN_TEST(test_settings_limits);
    RUN_TEST(test_text_time);
    RUN_TEST(test_text_time_unknown);
    RUN_TEST(test_text_snooze_countdown);
    RUN_TEST(test_text_snooze_without_clock);
    RUN_TEST(test_text_next_alarm);
    RUN_TEST(test_text_no_alarm);
    RUN_TEST(test_text_date);
    RUN_TEST(test_text_out_of_range);
    RUN_TEST(test_text_strings_terminated);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
