/*
 * Tests for lib/cold_session – requirements COLD-15 ... COLD-17.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "cold_session.h"

#define UTC0 1795000000u

static cold_session_t *cs;
static cold_profile_t plan;
static cold_record_t rec;
static uint32_t now;
static uint32_t t0;

void setUp(void)
{
    cs = NULL;
    plan.warm_s = 120;
    plan.cold_s = 90;
    plan.after_s = 300;
    plan.voice = true;
    plan.music = true;
    plan.calm = true;
    memset(&rec, 0xEE, sizeof(rec));
    now = 7000000u;
    t0 = now;
}

void tearDown(void)
{
    cold_session_destroy(cs);
    cs = NULL;
}

static void create(void)
{
    cs = cold_session_create();
    TEST_ASSERT_NOT_NULL_MESSAGE(cs, "cold_session_create returned NULL");
}

static void begin(uint32_t id, uint32_t utc)
{
    t0 = now;
    TEST_ASSERT_TRUE(cold_session_begin(cs, id, COLD_SESSION_HYPE, &plan, utc, now));
}

/* Move to `phase` at `rel` ms after begin. */
static void phase_at(cold_phase_t phase, uint32_t rel)
{
    now = t0 + rel;
    cold_session_phase(cs, phase, now);
}

static bool end_at(cold_end_t end, uint32_t rel, uint32_t utc)
{
    now = t0 + rel;
    return cold_session_end(cs, end, utc, now, &rec);
}

static void test_full_run(void)
{
    create();
    begin(17, UTC0);
    phase_at(COLD_COUNTDOWN, 0);
    phase_at(COLD_WARM, 3200);
    phase_at(COLD_COLD, 3200 + 120000);
    phase_at(COLD_AFTER, 3200 + 120000 + 90000);
    TEST_ASSERT_TRUE(end_at(COLD_END_FINISHED, 3200 + 120000 + 90000 + 300000, UTC0 + 513));
    TEST_ASSERT_EQUAL_UINT32(17, rec.id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(UTC0, rec.start_utc, "the start time given to begin() is kept");
    TEST_ASSERT_EQUAL_UINT8(COLD_SESSION_HYPE, rec.profile);
    TEST_ASSERT_EQUAL_UINT16(120, rec.planned_warm_s);
    TEST_ASSERT_EQUAL_UINT16(90, rec.planned_cold_s);
    TEST_ASSERT_EQUAL_UINT16(300, rec.planned_after_s);
    TEST_ASSERT_EQUAL_UINT32(120, rec.warm_s);
    TEST_ASSERT_EQUAL_UINT32(90, rec.cold_s);
    TEST_ASSERT_EQUAL_UINT32(300, rec.after_s);
    TEST_ASSERT_EQUAL_INT(COLD_END_FINISHED, rec.end);
    TEST_ASSERT_FALSE(rec.skipped);
}

static void test_skipped_and_aborted_run(void)
{
    create();
    plan.warm_s = 0;
    begin(18, UTC0);
    phase_at(COLD_COLD, 3200); /* no warm phase */
    cold_session_next(cs);     /* "next" pressed in the cold phase */
    phase_at(COLD_AFTER, 3200 + 41400);
    TEST_ASSERT_TRUE(end_at(COLD_END_ABORTED, 3200 + 41400 + 12600, 0));
    TEST_ASSERT_EQUAL_UINT32(0, rec.warm_s);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(41, rec.cold_s, "41.4 s rounds to 41");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(13, rec.after_s, "12.6 s rounds to 13");
    TEST_ASSERT_EQUAL_UINT16(0, rec.planned_warm_s);
    TEST_ASSERT_EQUAL_INT(COLD_END_ABORTED, rec.end);
    TEST_ASSERT_TRUE(rec.skipped);
}

static void test_rounding_half_up(void)
{
    create();
    begin(1, UTC0);
    phase_at(COLD_COLD, 1000);
    TEST_ASSERT_TRUE(end_at(COLD_END_ABORTED, 1000 + 30500, 0));
    TEST_ASSERT_EQUAL_UINT32(31, rec.cold_s);
    begin(2, UTC0);
    phase_at(COLD_COLD, 1000);
    TEST_ASSERT_TRUE(end_at(COLD_END_ABORTED, 1000 + 30499, 0));
    TEST_ASSERT_EQUAL_UINT32(30, rec.cold_s);
}

static void test_countdown_and_repeats_count_for_nothing(void)
{
    create();
    begin(3, UTC0);
    phase_at(COLD_COUNTDOWN, 1000); /* repeated: nothing */
    phase_at(COLD_WARM, 3200);
    phase_at(COLD_WARM, 60000); /* repeated: nothing */
    phase_at(COLD_COLD, 123200);
    phase_at(COLD_COLD, 150000);
    phase_at(COLD_IDLE, 213200); /* idle counts for nothing */
    TEST_ASSERT_TRUE(end_at(COLD_END_FINISHED, 400000, 0));
    TEST_ASSERT_EQUAL_UINT32(120, rec.warm_s);
    TEST_ASSERT_EQUAL_UINT32(90, rec.cold_s);
    TEST_ASSERT_EQUAL_UINT32(0, rec.after_s);
}

static void test_no_record_without_a_cold_phase(void)
{
    create();
    const cold_record_t untouched = rec;
    begin(4, UTC0);
    TEST_ASSERT_FALSE_MESSAGE(end_at(COLD_END_ABORTED, 2000, 0), "aborted in the countdown");
    begin(5, UTC0);
    phase_at(COLD_WARM, 3200);
    TEST_ASSERT_FALSE_MESSAGE(end_at(COLD_END_ABORTED, 60000, 0), "aborted in the warm phase");
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &rec, sizeof(rec));
    /* the session is closed either way */
    TEST_ASSERT_FALSE(end_at(COLD_END_ABORTED, 70000, 0));
    /* and a real one afterwards works */
    begin(6, UTC0);
    phase_at(COLD_COLD, 3200);
    TEST_ASSERT_TRUE(end_at(COLD_END_FINISHED, 93200, 0));
    TEST_ASSERT_EQUAL_UINT32(6, rec.id);
}

static void test_start_time_filled_in_later(void)
{
    create();
    begin(7, 0); /* SNTP not there yet */
    phase_at(COLD_COLD, 3200);
    TEST_ASSERT_TRUE(end_at(COLD_END_FINISHED, 93700, UTC0 + 1000));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(UTC0 + 1000 - 94, rec.start_utc, "93.7 s before the end, rounded to 94");
    begin(8, 0);
    phase_at(COLD_COLD, 3200);
    TEST_ASSERT_TRUE(end_at(COLD_END_FINISHED, 93200, 0));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rec.start_utc, "still unknown");
}

static void test_calls_outside_a_session_do_nothing(void)
{
    create();
    cold_session_phase(cs, COLD_COLD, now);
    cold_session_next(cs);
    TEST_ASSERT_FALSE(cold_session_end(cs, COLD_END_FINISHED, 0, now, &rec));
    TEST_ASSERT_FALSE(cold_session_begin(cs, 0, COLD_SESSION_HYPE, &plan, UTC0, now));
    TEST_ASSERT_FALSE(cold_session_begin(cs, 9, 2, &plan, UTC0, now));
    TEST_ASSERT_FALSE(cold_session_begin(cs, 9, COLD_SESSION_HYPE, NULL, UTC0, now));
    TEST_ASSERT_FALSE(cold_session_end(cs, COLD_END_FINISHED, 0, now, &rec));
    /* a new begin drops an open session */
    begin(10, UTC0);
    phase_at(COLD_COLD, 1000);
    cold_session_next(cs);
    begin(11, UTC0);
    cold_session_profile_t z = COLD_SESSION_ZEN;
    TEST_ASSERT_TRUE(cold_session_begin(cs, 11, (uint8_t)z, &plan, UTC0, now));
    t0 = now;
    phase_at(COLD_COLD, 2000);
    TEST_ASSERT_TRUE(end_at(COLD_END_FINISHED, 12000, 0));
    TEST_ASSERT_EQUAL_UINT32(11, rec.id);
    TEST_ASSERT_EQUAL_UINT8(COLD_SESSION_ZEN, rec.profile);
    TEST_ASSERT_EQUAL_UINT32(10, rec.cold_s);
    TEST_ASSERT_FALSE_MESSAGE(rec.skipped, "the skip belonged to the dropped session");
    cold_session_destroy(NULL);
}

static void test_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 50000u;
    create();
    begin(12, UTC0);
    phase_at(COLD_COLD, 3200);
    TEST_ASSERT_TRUE(end_at(COLD_END_FINISHED, 3200 + 90000, 0));
    TEST_ASSERT_EQUAL_UINT32(90, rec.cold_s);
}

static void test_json(void)
{
    cold_record_t r;
    memset(&r, 0, sizeof(r));
    r.id = 17;
    r.start_utc = 1795000000u;
    r.profile = COLD_SESSION_HYPE;
    r.planned_warm_s = 120;
    r.planned_cold_s = 90;
    r.planned_after_s = 300;
    r.warm_s = 118;
    r.cold_s = 90;
    r.after_s = 12;
    r.end = COLD_END_ABORTED;
    r.skipped = true;
    char out[COLD_SESSION_JSON_MAX];
    const char *expected = "{\"type\":\"cold_session\",\"id\":17,\"start\":1795000000,\"profile\":\"hype\","
                           "\"planned\":{\"warm\":120,\"cold\":90,\"after\":300},"
                           "\"actual\":{\"warm\":118,\"cold\":90,\"after\":12},\"end\":\"aborted\",\"skipped\":true}";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected), cold_session_to_json(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(expected, out);

    r.start_utc = 0;
    r.profile = COLD_SESSION_ZEN;
    r.end = COLD_END_FINISHED;
    r.skipped = false;
    const char *expected2 = "{\"type\":\"cold_session\",\"id\":17,\"start\":null,\"profile\":\"zen\","
                            "\"planned\":{\"warm\":120,\"cold\":90,\"after\":300},"
                            "\"actual\":{\"warm\":118,\"cold\":90,\"after\":12},\"end\":\"finished\",\"skipped\":false}";
    TEST_ASSERT_EQUAL_INT((int)strlen(expected2), cold_session_to_json(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(expected2, out);
}

static void test_json_limits(void)
{
    cold_record_t r;
    memset(&r, 0, sizeof(r));
    r.id = 4294967295u;
    r.start_utc = 4294967295u;
    r.planned_warm_s = 65535;
    r.planned_cold_s = 65535;
    r.planned_after_s = 65535;
    r.warm_s = 4294967295u;
    r.cold_s = 4294967295u;
    r.after_s = 4294967295u;
    r.end = COLD_END_ABORTED;
    r.skipped = false;
    char out[COLD_SESSION_JSON_MAX];
    const int len = cold_session_to_json(&r, out, sizeof(out));
    TEST_ASSERT_TRUE_MESSAGE(len > 0 && len < COLD_SESSION_JSON_MAX, "the longest record fits");
    char small[64];
    memset(small, 'Z', sizeof(small));
    TEST_ASSERT_EQUAL_INT(-1, cold_session_to_json(&r, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL_INT(-1, cold_session_to_json(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, cold_session_to_json(&r, NULL, 100));
    char exact[COLD_SESSION_JSON_MAX];
    TEST_ASSERT_EQUAL_INT(len, cold_session_to_json(&r, exact, (size_t)len + 1));
    TEST_ASSERT_EQUAL_INT(-1, cold_session_to_json(&r, exact, (size_t)len));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_full_run);
    RUN_TEST(test_skipped_and_aborted_run);
    RUN_TEST(test_rounding_half_up);
    RUN_TEST(test_countdown_and_repeats_count_for_nothing);
    RUN_TEST(test_no_record_without_a_cold_phase);
    RUN_TEST(test_start_time_filled_in_later);
    RUN_TEST(test_calls_outside_a_session_do_nothing);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_json);
    RUN_TEST(test_json_limits);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
