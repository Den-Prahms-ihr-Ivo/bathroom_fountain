/*
 * Tests for lib/button – requirements BEH-03, BEH-04.
 * The helper feeds one update per millisecond.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "button.h"

static button_config_t cfg;
static button_t *b;
static uint32_t now;

typedef struct {
    int shorts;
    int longs;
    uint32_t last_time;
    button_event_t last;
} tally_t;

static tally_t tally;

void setUp(void)
{
    button_config_default(&cfg);
    cfg.debounce_ms = 30;
    cfg.long_press_ms = 3000;
    b = NULL;
    now = 50000u;
    memset(&tally, 0, sizeof(tally));
}

void tearDown(void)
{
    button_destroy(b);
    b = NULL;
}

static void create(void)
{
    b = button_create(&cfg, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(b, "button_create returned NULL for a valid config");
}

/* Feed `level` for `ms` milliseconds; returns the time of the first update. */
static uint32_t feed(bool level, uint32_t ms)
{
    const uint32_t start = now;
    for (uint32_t i = 0; i < ms; i++) {
        const button_event_t e = button_update(b, level, now);
        if (e == BUTTON_SHORT) {
            tally.shorts++;
        } else if (e == BUTTON_LONG) {
            tally.longs++;
        }
        if (e != BUTTON_NONE) {
            tally.last = e;
            tally.last_time = now;
        }
        now++;
    }
    return start;
}

/* ------------------------------------------------------------------------ */

static void test_defaults(void)
{
    button_config_t d;
    memset(&d, 0xA5, sizeof(d));
    button_config_default(&d);
    TEST_ASSERT_EQUAL_UINT16(30, d.debounce_ms);
    TEST_ASSERT_EQUAL_UINT16(3000, d.long_press_ms);
}

static void test_create_rejects_invalid_config(void)
{
    create();
    TEST_ASSERT_NULL(button_create(NULL, now));
    button_config_t c = {30, 30};
    TEST_ASSERT_NULL_MESSAGE(button_create(&c, now), "long press must exceed debounce");
    c.long_press_ms = 10;
    TEST_ASSERT_NULL_MESSAGE(button_create(&c, now), "long press shorter than debounce");

    c.long_press_ms = 0;
    button_t *ok = button_create(&c, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(ok, "long press disabled is valid");
    button_destroy(ok);
    c.debounce_ms = 0;
    ok = button_create(&c, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(ok, "no debounce is valid");
    button_destroy(ok);
}

static void test_clean_short_press(void)
{
    create();
    feed(false, 100);
    feed(true, 200);
    const uint32_t release = feed(false, 100);
    TEST_ASSERT_EQUAL_INT(1, tally.shorts);
    TEST_ASSERT_EQUAL_INT(0, tally.longs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(release + 30, tally.last_time, "SHORT when the release has been stable 30 ms");
}

static void test_debounced_state_follows_after_debounce_time(void)
{
    create();
    feed(false, 10);
    const uint32_t press = now;
    feed(true, 30); /* updates at press … press+29 */
    TEST_ASSERT_FALSE_MESSAGE(button_is_pressed(b), "29 ms is not enough");
    feed(true, 1); /* update at press+30 */
    TEST_ASSERT_TRUE(button_is_pressed(b));
    TEST_ASSERT_EQUAL_UINT32(press + 31, now);
}

static void test_short_glitch_is_ignored(void)
{
    create();
    feed(false, 50);
    feed(true, 20);
    feed(false, 200);
    TEST_ASSERT_EQUAL_INT(0, tally.shorts + tally.longs);
    TEST_ASSERT_FALSE(button_is_pressed(b));
}

static void test_bouncing_contacts_give_one_press(void)
{
    create();
    feed(false, 50);
    feed(true, 3);
    feed(false, 2);
    feed(true, 4);
    feed(false, 1);
    feed(true, 200);
    feed(false, 2);
    feed(true, 3);
    feed(false, 100);
    TEST_ASSERT_EQUAL_INT(1, tally.shorts);
    TEST_ASSERT_EQUAL_INT(0, tally.longs);
}

static void test_long_press_fires_once_while_held(void)
{
    create();
    feed(false, 50);
    const uint32_t press = now;
    feed(true, 3001);
    TEST_ASSERT_EQUAL_INT(1, tally.longs);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(press + 3000, tally.last_time, "LONG at 3000 ms after the press transition");
    TEST_ASSERT_TRUE(button_is_pressed(b));
    feed(true, 2000);
    feed(false, 100);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, tally.longs, "only one LONG per press");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, tally.shorts, "no SHORT after a LONG");
}

static void test_press_just_below_long_is_short(void)
{
    create();
    feed(false, 50);
    feed(true, 2990);
    feed(false, 100);
    TEST_ASSERT_EQUAL_INT(1, tally.shorts);
    TEST_ASSERT_EQUAL_INT(0, tally.longs);
}

static void test_long_press_disabled(void)
{
    cfg.long_press_ms = 0;
    create();
    feed(false, 50);
    feed(true, 10000);
    TEST_ASSERT_EQUAL_INT(0, tally.longs);
    feed(false, 100);
    TEST_ASSERT_EQUAL_INT(1, tally.shorts);
}

static void test_no_debounce_reacts_immediately(void)
{
    cfg.debounce_ms = 0;
    create();
    feed(false, 5);
    feed(true, 1);
    TEST_ASSERT_TRUE(button_is_pressed(b));
    const uint32_t release = feed(false, 1);
    TEST_ASSERT_EQUAL_INT(1, tally.shorts);
    TEST_ASSERT_EQUAL_UINT32(release, tally.last_time);
}

static void test_repeated_presses(void)
{
    create();
    for (int i = 0; i < 5; i++) {
        feed(false, 80);
        feed(true, 120);
    }
    feed(false, 80);
    TEST_ASSERT_EQUAL_INT(5, tally.shorts);
}

static void test_long_press_across_counter_wrap(void)
{
    now = UINT32_MAX - 1000u;
    create();
    feed(false, 10);
    const uint32_t press = now;
    feed(true, 3500);
    TEST_ASSERT_EQUAL_INT(1, tally.longs);
    TEST_ASSERT_EQUAL_UINT32(press + 3000u, tally.last_time);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_create_rejects_invalid_config);
    RUN_TEST(test_clean_short_press);
    RUN_TEST(test_debounced_state_follows_after_debounce_time);
    RUN_TEST(test_short_glitch_is_ignored);
    RUN_TEST(test_bouncing_contacts_give_one_press);
    RUN_TEST(test_long_press_fires_once_while_held);
    RUN_TEST(test_press_just_below_long_is_short);
    RUN_TEST(test_long_press_disabled);
    RUN_TEST(test_no_debounce_reacts_immediately);
    RUN_TEST(test_repeated_presses);
    RUN_TEST(test_long_press_across_counter_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
