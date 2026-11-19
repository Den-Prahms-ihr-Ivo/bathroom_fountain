/*
 * Tests for lib/alarm_schedule – requirements BED-01 ... BED-04.
 * Local seconds are built from a calendar date with days_from_civil().
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "alarm_schedule.h"

static alarm_t alarms[ALARM_SLOTS];
static alarm_next_t next;

void setUp(void)
{
    memset(alarms, 0, sizeof(alarms));
    memset(&next, 0xEE, sizeof(next));
}

void tearDown(void) {}

/* Days since 1970-01-01 for a proleptic Gregorian date. */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int64_t at(int y, int mo, int d, int h, int mi, int s)
{
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

static void put(int slot, const char *text)
{
    TEST_ASSERT_TRUE_MESSAGE(alarm_parse(text, &alarms[slot]), text);
}

static void expect_next(int64_t after, int64_t expected, int slot)
{
    memset(&next, 0xEE, sizeof(next));
    TEST_ASSERT_TRUE(alarm_next(alarms, ALARM_SLOTS, true, after, &next));
    TEST_ASSERT_EQUAL_INT64(expected, next.local_s);
    TEST_ASSERT_EQUAL_UINT8(slot, next.slot);
}

static void test_parse(void)
{
    alarm_t a;
    memset(&a, 0xEE, sizeof(a));
    TEST_ASSERT_TRUE(alarm_parse("06:45,mtwtf--,on", &a));
    TEST_ASSERT_TRUE(a.used);
    TEST_ASSERT_TRUE(a.enabled);
    TEST_ASSERT_EQUAL_UINT8(6, a.hour);
    TEST_ASSERT_EQUAL_UINT8(45, a.minute);
    TEST_ASSERT_EQUAL_HEX8(0x1F, a.days);

    TEST_ASSERT_TRUE(alarm_parse("23:59,-----ss,off", &a));
    TEST_ASSERT_FALSE(a.enabled);
    TEST_ASSERT_EQUAL_UINT8(23, a.hour);
    TEST_ASSERT_EQUAL_UINT8(59, a.minute);
    TEST_ASSERT_EQUAL_HEX8(0x60, a.days);

    TEST_ASSERT_TRUE(alarm_parse("00:00,-------,on", &a));
    TEST_ASSERT_EQUAL_HEX8(0x00, a.days);
    TEST_ASSERT_TRUE(alarm_parse("12:30,m-w-f-s,on", &a));
    TEST_ASSERT_EQUAL_HEX8(0x55, a.days);
    TEST_ASSERT_TRUE(alarm_parse("12:30,-t-t-s-,on", &a));
    TEST_ASSERT_EQUAL_HEX8(0x2A, a.days);
}

static void test_parse_rejects_everything_else(void)
{
    const char *bad[] = {
        "",                    "6:45,mtwtf--,on",    "06:45,mtwtf--,on ",  " 06:45,mtwtf--,on", "24:00,mtwtf--,on",
        "06:60,mtwtf--,on",    "06-45,mtwtf--,on",   "06:45;mtwtf--,on",   "06:45,mtwtf-,on",   "06:45,mtwtf---,on",
        "06:45,MTWTF--,on",    "06:45,tmwtf--,on",   "06:45,mtwtf--,ON",   "06:45,mtwtf--,",    "06:45,mtwtf--,yes",
        "06:45,mtwtf--,offf",  "06:45,mtwtf--",      "0a:45,mtwtf--,on",   "06:4b,mtwtf--,on",  "06:45,xxxxxxx,on",
        "06:45,mtwtf--,on,x",  "+6:45,mtwtf--,on",
    };
    alarm_t a;
    memset(&a, 0x33, sizeof(a));
    const alarm_t untouched = a;
    TEST_ASSERT_TRUE_MESSAGE(alarm_parse("06:45,mtwtf--,on", &a), "a valid text must parse");
    a = untouched;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(alarm_parse(bad[i], &a), bad[i]);
        TEST_ASSERT_EQUAL_MEMORY(&untouched, &a, sizeof(a));
        if (bad[i][0] != '\0') {
            TEST_ASSERT_FALSE_MESSAGE(alarm_text_valid(bad[i]), bad[i]);
        }
    }
    TEST_ASSERT_FALSE(alarm_parse(NULL, &a));
    TEST_ASSERT_FALSE(alarm_parse("06:45,mtwtf--,on", NULL));
    TEST_ASSERT_TRUE_MESSAGE(alarm_text_valid(""), "an empty slot is a valid setting");
    TEST_ASSERT_TRUE(alarm_text_valid("06:45,mtwtf--,on"));
    TEST_ASSERT_FALSE(alarm_text_valid(NULL));
}

static void test_format(void)
{
    char out[ALARM_TEXT_MAX + 1];
    alarm_t a = {.used = true, .enabled = true, .hour = 6, .minute = 5, .days = 0x1F};
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_TRUE(alarm_format(&a, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("06:05,mtwtf--,on", out);
    a.enabled = false;
    a.days = 0x41;
    a.hour = 23;
    TEST_ASSERT_TRUE(alarm_format(&a, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("23:05,m-----s,off", out);
    a.used = false;
    TEST_ASSERT_TRUE(alarm_format(&a, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);

    a.used = true;
    char small[17]; /* "..,off" needs 18 */
    memset(small, 'Z', sizeof(small));
    TEST_ASSERT_FALSE(alarm_format(&a, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    a.enabled = true;
    TEST_ASSERT_TRUE(alarm_format(&a, small, sizeof(small)));
    a.hour = 24;
    TEST_ASSERT_FALSE(alarm_format(&a, out, sizeof(out)));
    a.hour = 7;
    a.minute = 60;
    TEST_ASSERT_FALSE(alarm_format(&a, out, sizeof(out)));
    a.minute = 0;
    a.days = 0x80;
    TEST_ASSERT_FALSE(alarm_format(&a, out, sizeof(out)));
    TEST_ASSERT_FALSE(alarm_format(NULL, out, sizeof(out)));
    TEST_ASSERT_FALSE(alarm_format(&a, NULL, 20));
}

static void test_text_round_trip(void)
{
    const char *texts[] = {"06:45,mtwtf--,on", "00:00,-------,off", "23:59,mtwtfss,on", "12:07,--w---s,off"};
    for (size_t i = 0; i < 4; i++) {
        alarm_t a;
        char out[ALARM_TEXT_MAX + 1];
        TEST_ASSERT_TRUE(alarm_parse(texts[i], &a));
        TEST_ASSERT_TRUE(alarm_format(&a, out, sizeof(out)));
        TEST_ASSERT_EQUAL_STRING(texts[i], out);
    }
}

static void test_weekday_alarm(void)
{
    put(0, "06:45,mtwtf--,on");
    /* 2026-11-16 is a Monday */
    expect_next(at(2026, 11, 16, 5, 0, 0), at(2026, 11, 16, 6, 45, 0), 0);
    expect_next(at(2026, 11, 16, 6, 44, 59), at(2026, 11, 16, 6, 45, 0), 0);
    expect_next(at(2026, 11, 16, 6, 45, 0), at(2026, 11, 17, 6, 45, 0), 0); /* strictly after */
    expect_next(at(2026, 11, 20, 6, 45, 0), at(2026, 11, 23, 6, 45, 0), 0); /* Friday -> Monday */
    expect_next(at(2026, 11, 21, 12, 0, 0), at(2026, 11, 23, 6, 45, 0), 0); /* Saturday */
    expect_next(at(2026, 11, 22, 23, 59, 59), at(2026, 11, 23, 6, 45, 0), 0);
}

static void test_every_single_weekday(void)
{
    const char *texts[7] = {"08:00,m------,on", "08:00,-t-----,on", "08:00,--w----,on", "08:00,---t---,on",
                            "08:00,----f--,on", "08:00,-----s-,on", "08:00,------s,on"};
    for (int d = 0; d < 7; d++) {
        memset(alarms, 0, sizeof(alarms));
        put(0, texts[d]);
        /* from Monday 2026-11-16 00:00 the next one is on 16 + d */
        expect_next(at(2026, 11, 16, 0, 0, 0), at(2026, 11, 16 + d, 8, 0, 0), 0);
        /* and a week later after that */
        expect_next(at(2026, 11, 16 + d, 8, 0, 0), at(2026, 11, 23 + d, 8, 0, 0), 0);
    }
    /* day 0 of the count was a Thursday */
    memset(alarms, 0, sizeof(alarms));
    put(0, "00:01,---t---,on");
    expect_next(0, 60, 0);
}

static void test_one_time_alarm(void)
{
    put(2, "07:30,-------,on");
    expect_next(at(2026, 11, 21, 7, 0, 0), at(2026, 11, 21, 7, 30, 0), 2);  /* later today (a Saturday) */
    expect_next(at(2026, 11, 21, 7, 30, 0), at(2026, 11, 22, 7, 30, 0), 2); /* otherwise tomorrow */
    expect_next(at(2026, 12, 31, 22, 0, 0), at(2027, 1, 1, 7, 30, 0), 2);   /* across the year */
}

static void test_earliest_of_several_and_ties(void)
{
    put(0, "09:00,-----ss,on");
    put(1, "06:45,mtwtf--,on");
    put(3, "06:45,mtwtfss,on");
    put(4, "05:00,mtwtfss,off");
    put(5, "06:45,-------,on");
    expect_next(at(2026, 11, 16, 0, 0, 0), at(2026, 11, 16, 6, 45, 0), 1); /* Monday: slots 1, 3 and 5 tie */
    expect_next(at(2026, 11, 21, 0, 0, 0), at(2026, 11, 21, 6, 45, 0), 3); /* Saturday: slots 3 and 5 tie */
    expect_next(at(2026, 11, 21, 6, 45, 0), at(2026, 11, 21, 9, 0, 0), 0);
    alarms[3].enabled = false;
    alarms[5].used = false;
    expect_next(at(2026, 11, 21, 0, 0, 0), at(2026, 11, 21, 9, 0, 0), 0);
}

static void test_nothing_to_ring(void)
{
    const alarm_next_t untouched = next;
    TEST_ASSERT_FALSE(alarm_next(alarms, ALARM_SLOTS, true, at(2026, 11, 16, 0, 0, 0), &next)); /* all unused */
    put(0, "06:45,mtwtf--,off");
    TEST_ASSERT_FALSE(alarm_next(alarms, ALARM_SLOTS, true, at(2026, 11, 16, 0, 0, 0), &next));
    put(1, "06:45,mtwtf--,on");
    TEST_ASSERT_FALSE_MESSAGE(alarm_next(alarms, ALARM_SLOTS, false, at(2026, 11, 16, 0, 0, 0), &next),
                              "the armed switch is off");
    TEST_ASSERT_FALSE(alarm_next(alarms, 1, true, at(2026, 11, 16, 0, 0, 0), &next)); /* count limits the slots */
    TEST_ASSERT_FALSE(alarm_next(NULL, ALARM_SLOTS, true, 0, &next));
    TEST_ASSERT_FALSE(alarm_next(alarms, ALARM_SLOTS, true, 0, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &next, sizeof(next));
    expect_next(at(2026, 11, 16, 0, 0, 0), at(2026, 11, 16, 6, 45, 0), 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse);
    RUN_TEST(test_parse_rejects_everything_else);
    RUN_TEST(test_format);
    RUN_TEST(test_text_round_trip);
    RUN_TEST(test_weekday_alarm);
    RUN_TEST(test_every_single_weekday);
    RUN_TEST(test_one_time_alarm);
    RUN_TEST(test_earliest_of_several_and_ties);
    RUN_TEST(test_nothing_to_ring);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
