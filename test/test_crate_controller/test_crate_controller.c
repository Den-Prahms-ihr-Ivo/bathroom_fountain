/*
 * Tests for lib/crate_controller – requirements CRATE-01 ... CRATE-10.
 * The helper polls the lid every 10 ms with the level the test set, like the
 * crate app does.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "crate_controller.h"

static crate_config_t cfg;
static crate_controller_t *cc;
static uint32_t now;
static bool lid; /* raw level the "door contact" reports */

void setUp(void)
{
    crate_config_default(&cfg);
    cfg.debounce_ms = 50;
    cfg.open_level = 600;
    cfg.dim_level = 150;
    cfg.open_fade_ms = 1500;
    cfg.close_fade_ms = 3000;
    cfg.volume = 500;
    cfg.max_open_ms = 900000;
    cfg.cutoff_mv = 12400;
    cfg.warn_mv = 12800;
    cfg.battery_hold_ms = 10000;
    cfg.sleep_delay_ms = 2000;
    cfg.busy_max_ms = 600000;
    cc = NULL;
    now = 5000000u;
    lid = false;
}

void tearDown(void)
{
    crate_controller_destroy(cc);
    cc = NULL;
}

static void create(bool open)
{
    lid = open;
    cc = crate_controller_create(&cfg, open, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(cc, "crate_controller_create returned NULL for a valid config");
}

static const crate_outputs_t *out(void)
{
    return crate_controller_outputs(cc);
}

/* Poll the lid every 10 ms for `ms` milliseconds. */
static void run_ms(uint32_t ms)
{
    while (ms > 0) {
        const uint32_t step = ms > 10 ? 10 : ms;
        now += step;
        ms -= step;
        crate_controller_lid(cc, lid, now);
        crate_controller_tick(cc, now);
    }
}

/* Change the raw level and poll until it has counted (debounce + one poll). */
static void set_lid(bool open)
{
    lid = open;
    crate_controller_lid(cc, lid, now);
    run_ms(cfg.debounce_ms);
}

static void expect_event(crate_event_t e)
{
    crate_event_t got;
    TEST_ASSERT_TRUE_MESSAGE(crate_controller_take_event(cc, &got), "event missing");
    TEST_ASSERT_EQUAL_INT(e, got);
}

static void expect_no_event(void)
{
    crate_event_t got;
    TEST_ASSERT_FALSE(crate_controller_take_event(cc, &got));
}

static void expect_open_outputs(void)
{
    TEST_ASSERT_EQUAL_INT(CRATE_OPEN, out()->state);
    TEST_ASSERT_EQUAL_UINT16(600, out()->light_permille);
    TEST_ASSERT_EQUAL_UINT16(1500, out()->light_fade_ms);
    TEST_ASSERT_EQUAL_UINT16(500, out()->volume_permille);
    TEST_ASSERT_EQUAL_UINT16(1500, out()->volume_fade_ms);
    TEST_ASSERT_TRUE(out()->amp_on);
    TEST_ASSERT_TRUE(out()->music_playing);
    TEST_ASSERT_FALSE(out()->sleep_ok);
}

/* Feed one battery sample every second for `seconds`. */
static void battery_for(uint16_t mv, uint32_t seconds)
{
    for (uint32_t i = 0; i < seconds; i++) {
        crate_controller_battery(cc, mv, now);
        run_ms(1000);
    }
    crate_controller_battery(cc, mv, now);
}

static void test_defaults_and_validation(void)
{
    crate_config_t d;
    memset(&d, 0, sizeof(d));
    crate_config_default(&d);
    TEST_ASSERT_EQUAL_UINT16(50, d.debounce_ms);
    TEST_ASSERT_EQUAL_UINT16(600, d.open_level);
    TEST_ASSERT_EQUAL_UINT16(150, d.dim_level);
    TEST_ASSERT_EQUAL_UINT16(1500, d.open_fade_ms);
    TEST_ASSERT_EQUAL_UINT16(3000, d.close_fade_ms);
    TEST_ASSERT_EQUAL_UINT16(500, d.volume);
    TEST_ASSERT_EQUAL_UINT32(900000, d.max_open_ms);
    TEST_ASSERT_EQUAL_UINT16(12400, d.cutoff_mv);
    TEST_ASSERT_EQUAL_UINT16(12800, d.warn_mv);
    TEST_ASSERT_EQUAL_UINT16(10000, d.battery_hold_ms);
    TEST_ASSERT_EQUAL_UINT16(2000, d.sleep_delay_ms);
    TEST_ASSERT_EQUAL_UINT32(600000, d.busy_max_ms);

    crate_config_t b;
#define BAD(field, value)                                                   \
    do {                                                                    \
        b = d;                                                              \
        b.field = value;                                                    \
        TEST_ASSERT_NULL_MESSAGE(crate_controller_create(&b, false, now), #field " = " #value); \
    } while (0)
    BAD(busy_max_ms, 3600001);
    BAD(debounce_ms, 9);
    BAD(debounce_ms, 501);
    BAD(open_level, 0);
    BAD(open_level, 1001);
    BAD(dim_level, 601); /* above open_level */
    BAD(open_fade_ms, 10001);
    BAD(close_fade_ms, 20001);
    BAD(volume, 0);
    BAD(volume, 1001);
    BAD(max_open_ms, 59999);
    BAD(max_open_ms, 3600001);
    BAD(cutoff_mv, 9999);
    BAD(cutoff_mv, 13501);
    BAD(warn_mv, 12399); /* below cutoff_mv */
    BAD(warn_mv, 14001);
    BAD(battery_hold_ms, 999);
    BAD(battery_hold_ms, 60001);
    BAD(sleep_delay_ms, 60001);
#undef BAD
    TEST_ASSERT_NULL(crate_controller_create(NULL, false, now));
    b = d;
    b.max_open_ms = 0; /* off is allowed */
    b.dim_level = 0;
    b.open_fade_ms = 0;
    b.close_fade_ms = 0;
    b.sleep_delay_ms = 0;
    cc = crate_controller_create(&b, false, now);
    TEST_ASSERT_NOT_NULL(cc);
    crate_controller_destroy(NULL);
}

static void test_created_closed_is_dark_and_may_sleep(void)
{
    create(false);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    TEST_ASSERT_EQUAL_UINT16(0, out()->light_permille);
    TEST_ASSERT_EQUAL_UINT16(0, out()->volume_permille);
    TEST_ASSERT_FALSE(out()->amp_on);
    TEST_ASSERT_FALSE(out()->music_playing);
    TEST_ASSERT_FALSE(out()->lid_open);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_OK, out()->battery);
    TEST_ASSERT_FALSE(out()->sleep_ok);
    run_ms(1990);
    TEST_ASSERT_FALSE(out()->sleep_ok);
    run_ms(10);
    TEST_ASSERT_TRUE_MESSAGE(out()->sleep_ok, "switched on with the lid closed: back to sleep after sleep_delay_ms");
    expect_no_event();
}

static void test_wake_by_the_lid_opens_at_once(void)
{
    create(true);
    TEST_ASSERT_TRUE(out()->lid_open);
    expect_open_outputs();
    expect_event(CRATE_EV_MUSIC_START);
    expect_no_event();
    run_ms(60000);
    expect_open_outputs();
    expect_no_event();
}

static void test_debounce(void)
{
    create(false);
    for (int i = 0; i < 20; i++) { /* a bouncing contact: 20 ms open, 20 ms closed */
        lid = (i % 2) == 0;
        run_ms(20);
    }
    lid = false;
    run_ms(200);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    expect_no_event();

    lid = true;
    crate_controller_lid(cc, lid, now);
    run_ms(40);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_CLOSED, out()->state, "40 ms is not yet 50 ms");
    run_ms(10);
    TEST_ASSERT_EQUAL_INT(CRATE_OPEN, out()->state);
    TEST_ASSERT_TRUE(out()->lid_open);
    expect_event(CRATE_EV_MUSIC_START);
}

static void test_open_then_close(void)
{
    create(false);
    run_ms(5000);
    TEST_ASSERT_TRUE(out()->sleep_ok);
    set_lid(true);
    expect_open_outputs();
    expect_event(CRATE_EV_MUSIC_START);
    run_ms(30000);

    set_lid(false);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSING, out()->state);
    TEST_ASSERT_EQUAL_UINT16(0, out()->light_permille);
    TEST_ASSERT_EQUAL_UINT16(3000, out()->light_fade_ms);
    TEST_ASSERT_EQUAL_UINT16(0, out()->volume_permille);
    TEST_ASSERT_EQUAL_UINT16(3000, out()->volume_fade_ms);
    TEST_ASSERT_TRUE_MESSAGE(out()->amp_on, "the amplifier stays on for the fade-out");
    TEST_ASSERT_TRUE(out()->music_playing);
    expect_no_event();
    run_ms(2990);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSING, out()->state);
    run_ms(10);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    TEST_ASSERT_FALSE(out()->amp_on);
    TEST_ASSERT_FALSE(out()->music_playing);
    expect_event(CRATE_EV_MUSIC_STOP);
    TEST_ASSERT_FALSE(out()->sleep_ok);
    run_ms(1990);
    TEST_ASSERT_FALSE(out()->sleep_ok);
    run_ms(10);
    TEST_ASSERT_TRUE(out()->sleep_ok);
    expect_no_event();
}

static void test_reopen_during_the_fade_out_keeps_the_music(void)
{
    create(true);
    expect_event(CRATE_EV_MUSIC_START);
    run_ms(10000);
    set_lid(false);
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSING, out()->state);
    set_lid(true);
    expect_open_outputs();
    expect_no_event(); /* no MUSIC_START: it never stopped */
    run_ms(10000);
    expect_no_event(); /* and the old fade-out does not finish behind our back */
    TEST_ASSERT_EQUAL_INT(CRATE_OPEN, out()->state);
}

static void test_reopen_after_closed_starts_from_the_first_track(void)
{
    create(true);
    expect_event(CRATE_EV_MUSIC_START);
    set_lid(false);
    run_ms(3000);
    expect_event(CRATE_EV_MUSIC_STOP);
    run_ms(100);
    set_lid(true);
    expect_open_outputs();
    expect_event(CRATE_EV_MUSIC_START);
}

static void test_lid_left_open_dims_and_stops_the_music(void)
{
    cfg.max_open_ms = 60000;
    create(true);
    expect_event(CRATE_EV_MUSIC_START);
    run_ms(59990);
    TEST_ASSERT_EQUAL_INT(CRATE_OPEN, out()->state);
    run_ms(10);
    TEST_ASSERT_EQUAL_INT(CRATE_DIM, out()->state);
    TEST_ASSERT_EQUAL_UINT16(150, out()->light_permille);
    TEST_ASSERT_EQUAL_UINT16(3000, out()->light_fade_ms);
    TEST_ASSERT_EQUAL_UINT16(0, out()->volume_permille);
    TEST_ASSERT_EQUAL_UINT16(3000, out()->volume_fade_ms);
    TEST_ASSERT_TRUE(out()->music_playing);
    run_ms(3000);
    TEST_ASSERT_EQUAL_INT(CRATE_DIM, out()->state);
    TEST_ASSERT_FALSE(out()->music_playing);
    TEST_ASSERT_FALSE(out()->amp_on);
    TEST_ASSERT_EQUAL_UINT16(150, out()->light_permille);
    expect_event(CRATE_EV_MUSIC_STOP);
    run_ms(600000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_DIM, out()->state, "dim while the lid stays open");
    TEST_ASSERT_FALSE(out()->sleep_ok);
    expect_no_event();

    set_lid(false);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSING, out()->state);
    TEST_ASSERT_EQUAL_UINT16(0, out()->light_permille);
    run_ms(3000);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    expect_no_event(); /* the music had stopped already: no second STOP */

    set_lid(true);
    expect_open_outputs();
    expect_event(CRATE_EV_MUSIC_START);
}

static void test_reopen_while_dim_music_is_fading(void)
{
    cfg.max_open_ms = 60000;
    create(true);
    expect_event(CRATE_EV_MUSIC_START);
    run_ms(61000); /* DIM, one second into the fade */
    set_lid(false);
    run_ms(500);
    set_lid(true);
    expect_open_outputs();
    expect_no_event(); /* still playing: no restart */
    run_ms(59000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_OPEN, out()->state, "the max-open timer restarted at the reopening");
    run_ms(1000);
    TEST_ASSERT_EQUAL_INT(CRATE_DIM, out()->state);
}

static void test_max_open_off(void)
{
    cfg.max_open_ms = 0;
    create(true);
    expect_event(CRATE_EV_MUSIC_START);
    run_ms(3600000);
    expect_open_outputs();
    expect_no_event();
}

static void test_zero_fades(void)
{
    cfg.open_fade_ms = 0;
    cfg.close_fade_ms = 0;
    cfg.sleep_delay_ms = 0;
    create(false);
    TEST_ASSERT_TRUE(out()->sleep_ok);
    set_lid(true);
    TEST_ASSERT_EQUAL_UINT16(0, out()->light_fade_ms);
    expect_event(CRATE_EV_MUSIC_START);
    set_lid(false);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_CLOSED, out()->state, "no fade: closed at once");
    TEST_ASSERT_FALSE(out()->amp_on);
    TEST_ASSERT_TRUE(out()->sleep_ok);
    expect_event(CRATE_EV_MUSIC_STOP);
}

static void test_battery_states(void)
{
    create(false);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_OK, out()->battery);
    battery_for(13200, 20);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_OK, out()->battery);

    battery_for(12700, 9);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_BATT_OK, out()->battery, "9 s below warn_mv is not yet 10 s");
    crate_controller_battery(cc, 12900, now); /* one sample above restarts the count */
    battery_for(12700, 9);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_OK, out()->battery);
    battery_for(12700, 1);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_LOW, out()->battery);

    battery_for(12900, 20); /* above warn_mv but below warn_mv + 300 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_BATT_LOW, out()->battery, "hysteresis of 300 mV");
    battery_for(13100, 10);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_OK, out()->battery);

    battery_for(12300, 10);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_BATT_EMPTY, out()->battery, "straight to EMPTY below cutoff_mv");
    battery_for(12600, 20);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_EMPTY, out()->battery);
    battery_for(12700, 10); /* cutoff + 300 */
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_LOW, out()->battery);
    battery_for(12300, 10);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_EMPTY, out()->battery);
    battery_for(14200, 10); /* charger on, well above warn + 300 */
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_OK, out()->battery);
    expect_no_event(); /* closed crate: battery changes give no event */
}

static void test_battery_runs_empty_while_open(void)
{
    create(true);
    expect_event(CRATE_EV_MUSIC_START);
    battery_for(12300, 10);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_EMPTY, out()->battery);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSING, out()->state);
    TEST_ASSERT_EQUAL_UINT16(0, out()->light_permille);
    TEST_ASSERT_EQUAL_UINT16(0, out()->volume_permille);
    expect_event(CRATE_EV_BLINK);
    run_ms(3000);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    expect_event(CRATE_EV_MUSIC_STOP);
    TEST_ASSERT_TRUE(out()->lid_open);
    run_ms(2000);
    TEST_ASSERT_TRUE_MESSAGE(out()->sleep_ok, "empty: may sleep although the lid is open");
}

static void test_opening_with_an_empty_battery_only_blinks(void)
{
    create(false);
    battery_for(12000, 10);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_EMPTY, out()->battery);
    set_lid(true);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    TEST_ASSERT_EQUAL_UINT16(0, out()->light_permille);
    TEST_ASSERT_FALSE(out()->amp_on);
    TEST_ASSERT_FALSE(out()->music_playing);
    expect_event(CRATE_EV_BLINK);
    expect_no_event();
    run_ms(5000);
    TEST_ASSERT_TRUE(out()->sleep_ok);

    /* charged again while the lid is still open: it does not start by itself */
    battery_for(13500, 10);
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_OK, out()->battery);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    TEST_ASSERT_FALSE_MESSAGE(out()->sleep_ok, "lid open and battery fine: no sleep");
    set_lid(false);
    set_lid(true);
    expect_open_outputs();
    expect_event(CRATE_EV_MUSIC_START);
}

static void test_late_poll_and_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 1000u;
    create(true);
    expect_event(CRATE_EV_MUSIC_START);
    lid = false;
    crate_controller_lid(cc, lid, now); /* first report of the closing lid */
    now += 2000;                         /* the app was busy for two seconds; the counter wrapped */
    crate_controller_lid(cc, lid, now);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSING, out()->state);
    now += 1040; /* 50 ms debounce + 3000 ms fade after the first report, minus 10 */
    crate_controller_tick(cc, now);
    TEST_ASSERT_EQUAL_INT_MESSAGE(CRATE_CLOSING, out()->state, "the fade is timed from the debounced change");
    now += 10;
    crate_controller_tick(cc, now);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    expect_event(CRATE_EV_MUSIC_STOP);
}

/* ---- busy: a download keeps the closed crate awake (CRATE-13) ------------ */

static void drain_events(void)
{
    crate_event_t e;
    while (crate_controller_take_event(cc, &e)) {
    }
}

static void closed_after_use(void)
{
    create(true);
    drain_events();
    set_lid(false);
    run_ms(3000); /* close fade */
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    expect_event(CRATE_EV_MUSIC_STOP);
    expect_no_event();
}

static void test_busy_delays_sleep(void)
{
    closed_after_use();
    crate_controller_busy(cc, true, now);
    run_ms(2000);
    TEST_ASSERT_FALSE_MESSAGE(out()->sleep_ok, "download running: stay awake");
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    TEST_ASSERT_FALSE(out()->amp_on);
    TEST_ASSERT_EQUAL_UINT16(0, out()->light_permille);
    run_ms(60000);
    TEST_ASSERT_FALSE(out()->sleep_ok);
    crate_controller_busy(cc, false, now);
    TEST_ASSERT_TRUE_MESSAGE(out()->sleep_ok, "download done: sleep at once");
    expect_no_event();
}

static void test_busy_has_a_limit(void)
{
    closed_after_use();
    const uint32_t closed_at = now;
    crate_controller_busy(cc, true, now);
    while (now - closed_at < 2000u + 600000u - 10u) {
        run_ms(10);
    }
    TEST_ASSERT_FALSE(out()->sleep_ok);
    run_ms(10);
    TEST_ASSERT_TRUE_MESSAGE(out()->sleep_ok, "10 min after the sleep delay: sleep anyway");
}

static void test_busy_limit_zero(void)
{
    cfg.busy_max_ms = 0;
    closed_after_use();
    crate_controller_busy(cc, true, now);
    run_ms(2000);
    TEST_ASSERT_TRUE(out()->sleep_ok);
}

static void test_busy_changes_nothing_while_open(void)
{
    create(true);
    drain_events();
    crate_controller_busy(cc, true, now);
    run_ms(5000);
    expect_open_outputs();
    expect_no_event();
    crate_controller_busy(cc, false, now);
    expect_open_outputs();
}

static void test_busy_set_before_closing(void)
{
    create(true);
    drain_events();
    crate_controller_busy(cc, true, now);
    set_lid(false);
    run_ms(3000 + 2000 + 1000);
    TEST_ASSERT_EQUAL_INT(CRATE_CLOSED, out()->state);
    TEST_ASSERT_FALSE(out()->sleep_ok);
    expect_event(CRATE_EV_MUSIC_STOP);
    /* reopening during the download works as always */
    set_lid(true);
    TEST_ASSERT_EQUAL_INT(CRATE_OPEN, out()->state);
    expect_event(CRATE_EV_MUSIC_START);
}

static void test_busy_ignored_on_empty_battery(void)
{
    create(false);
    crate_controller_busy(cc, true, now);
    for (int i = 0; i < 12; i++) {
        crate_controller_battery(cc, 12000, now);
        run_ms(1000);
    }
    TEST_ASSERT_EQUAL_INT(CRATE_BATT_EMPTY, out()->battery);
    TEST_ASSERT_TRUE_MESSAGE(out()->sleep_ok, "empty battery: sleep despite the download");
}

static void test_busy_null_is_ignored(void)
{
    crate_controller_busy(NULL, true, now);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_created_closed_is_dark_and_may_sleep);
    RUN_TEST(test_wake_by_the_lid_opens_at_once);
    RUN_TEST(test_debounce);
    RUN_TEST(test_open_then_close);
    RUN_TEST(test_reopen_during_the_fade_out_keeps_the_music);
    RUN_TEST(test_reopen_after_closed_starts_from_the_first_track);
    RUN_TEST(test_lid_left_open_dims_and_stops_the_music);
    RUN_TEST(test_reopen_while_dim_music_is_fading);
    RUN_TEST(test_max_open_off);
    RUN_TEST(test_zero_fades);
    RUN_TEST(test_battery_states);
    RUN_TEST(test_battery_runs_empty_while_open);
    RUN_TEST(test_opening_with_an_empty_battery_only_blinks);
    RUN_TEST(test_late_poll_and_clock_wrap);
    RUN_TEST(test_busy_delays_sleep);
    RUN_TEST(test_busy_has_a_limit);
    RUN_TEST(test_busy_limit_zero);
    RUN_TEST(test_busy_changes_nothing_while_open);
    RUN_TEST(test_busy_set_before_closing);
    RUN_TEST(test_busy_ignored_on_empty_battery);
    RUN_TEST(test_busy_null_is_ignored);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
