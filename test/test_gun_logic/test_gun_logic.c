/*
 * Tests for lib/gun_logic – requirements GAME-30 ... GAME-36.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "gun_logic.h"

static gun_config_t cfg;
static gun_logic_t *g;
static uint32_t now;

void setUp(void)
{
    gun_config_default(&cfg);
    cfg.cooldown_ms = 250;
    cfg.idle_sleep_ms = 60000;
    cfg.demo_hold_ms = 3000;
    g = NULL;
    now = 9000000u;
}

void tearDown(void)
{
    gun_logic_destroy(g);
    g = NULL;
}

static void create(void)
{
    g = gun_logic_create(&cfg, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(g, "gun_logic_create returned NULL for a valid config");
}

static void setup(uint8_t id, uint8_t sights, uint8_t ammo)
{
    const gun_setup_t s = {id, sights, ammo};
    TEST_ASSERT_TRUE(gun_logic_setup(g, &s, now));
}

/* One trigger pull of 80 ms, then wait until `gap_ms` after the press. */
static void pull(uint32_t gap_ms)
{
    gun_logic_trigger(g, true, now);
    now += 80;
    gun_logic_trigger(g, false, now);
    now += gap_ms - 80;
    gun_logic_tick(g, now);
}

static void expect_action(gun_action_id_t id, uint8_t shot, uint8_t ammo_left)
{
    gun_action_t a;
    TEST_ASSERT_TRUE_MESSAGE(gun_logic_take_action(g, &a), "action missing");
    TEST_ASSERT_EQUAL_INT(id, a.id);
    if (id == GUN_ACT_FIRE) {
        TEST_ASSERT_EQUAL_UINT8(shot, a.shot);
    }
    TEST_ASSERT_EQUAL_UINT8(ammo_left, a.ammo_left);
}

static void expect_no_action(void)
{
    gun_action_t a;
    TEST_ASSERT_FALSE(gun_logic_take_action(g, &a));
}

static void test_defaults_and_validation(void)
{
    gun_config_t d;
    memset(&d, 0, sizeof(d));
    gun_config_default(&d);
    TEST_ASSERT_EQUAL_UINT16(250, d.cooldown_ms);
    TEST_ASSERT_EQUAL_UINT32(60000, d.idle_sleep_ms);
    TEST_ASSERT_EQUAL_UINT16(3000, d.demo_hold_ms);
    create();
    const gun_outputs_t *o = gun_logic_outputs(g);
    TEST_ASSERT_EQUAL_UINT8(0, o->gun_id);
    TEST_ASSERT_EQUAL_UINT8(30, o->sights);
    TEST_ASSERT_EQUAL_UINT8(GUN_NO_MAGAZINE, o->ammo_left);
    TEST_ASSERT_FALSE(o->sleep);
    expect_no_action();

    gun_config_t bad = d;
    bad.cooldown_ms = 49;
    TEST_ASSERT_NULL(gun_logic_create(&bad, now));
    bad.cooldown_ms = 2001;
    TEST_ASSERT_NULL(gun_logic_create(&bad, now));
    bad = d;
    bad.idle_sleep_ms = 4999;
    TEST_ASSERT_NULL(gun_logic_create(&bad, now));
    bad.idle_sleep_ms = 600001;
    TEST_ASSERT_NULL(gun_logic_create(&bad, now));
    bad = d;
    bad.demo_hold_ms = 999;
    TEST_ASSERT_NULL(gun_logic_create(&bad, now));
    bad.demo_hold_ms = 10001;
    TEST_ASSERT_NULL(gun_logic_create(&bad, now));
    TEST_ASSERT_NULL(gun_logic_create(NULL, now));
    gun_logic_destroy(NULL);
}

static void test_setup(void)
{
    create();
    setup(11, 60, 6);
    const gun_outputs_t *o = gun_logic_outputs(g);
    TEST_ASSERT_EQUAL_UINT8(11, o->gun_id);
    TEST_ASSERT_EQUAL_UINT8(60, o->sights);
    TEST_ASSERT_EQUAL_UINT8(6, o->ammo_left);
    gun_setup_t bad = {16, 60, 6};
    TEST_ASSERT_FALSE(gun_logic_setup(g, &bad, now));
    bad.gun_id = 1;
    bad.sights = 101;
    TEST_ASSERT_FALSE(gun_logic_setup(g, &bad, now));
    bad.sights = 50;
    bad.ammo = 31;
    TEST_ASSERT_FALSE(gun_logic_setup(g, &bad, now));
    TEST_ASSERT_FALSE(gun_logic_setup(g, NULL, now));
    TEST_ASSERT_EQUAL_UINT8(11, o->gun_id);
    TEST_ASSERT_EQUAL_UINT8(6, o->ammo_left);
}

static void test_each_press_fires_once(void)
{
    create();
    gun_logic_trigger(g, true, now);
    expect_action(GUN_ACT_FIRE, 0, GUN_NO_MAGAZINE);
    for (int i = 0; i < 20; i++) { /* held: nothing more */
        now += 20;
        gun_logic_trigger(g, true, now);
        gun_logic_tick(g, now);
    }
    expect_no_action();
    gun_logic_trigger(g, false, now);
    expect_no_action(); /* the release does nothing */
    gun_logic_trigger(g, false, now);
    now += 500;
    gun_logic_trigger(g, true, now);
    expect_action(GUN_ACT_FIRE, 1, GUN_NO_MAGAZINE);
}

static void test_shot_counter_wraps_at_16(void)
{
    create();
    for (int i = 0; i < 35; i++) {
        pull(300);
        expect_action(GUN_ACT_FIRE, (uint8_t)(i % 16), GUN_NO_MAGAZINE);
    }
}

static void test_cooldown(void)
{
    create();
    pull(100);
    expect_action(GUN_ACT_FIRE, 0, GUN_NO_MAGAZINE);
    pull(100); /* 100 ms after the first: too soon */
    expect_no_action();
    now += 49; /* 249 ms after the first */
    gun_logic_trigger(g, true, now);
    gun_logic_trigger(g, false, now + 10);
    expect_no_action();
    now += 11; /* 260 ms */
    gun_logic_trigger(g, true, now);
    expect_action(GUN_ACT_FIRE, 1, GUN_NO_MAGAZINE);
    gun_logic_trigger(g, false, now + 10);
    now += 250; /* exactly the cooldown after the second */
    gun_logic_trigger(g, true, now);
    expect_action(GUN_ACT_FIRE, 2, GUN_NO_MAGAZINE);
}

static void test_magazine_runs_empty(void)
{
    create();
    setup(3, 30, 2);
    pull(300);
    expect_action(GUN_ACT_FIRE, 0, 1);
    pull(300);
    expect_action(GUN_ACT_FIRE, 1, 0);
    TEST_ASSERT_EQUAL_UINT8(0, gun_logic_outputs(g)->ammo_left);
    pull(300);
    expect_action(GUN_ACT_DRY, 0, 0);
    pull(300);
    expect_action(GUN_ACT_DRY, 0, 0);
    /* reload: magazine out and in */
    gun_logic_magazine(g, false, now);
    expect_no_action();
    pull(300);
    expect_action(GUN_ACT_DRY, 0, 0);
    gun_logic_magazine(g, true, now);
    expect_action(GUN_ACT_RELOADED, 0, 2);
    TEST_ASSERT_EQUAL_UINT8(2, gun_logic_outputs(g)->ammo_left);
    pull(300);
    expect_action(GUN_ACT_FIRE, 2, 1); /* the shot counter did not count the dry clicks */
}

static void test_magazine_details(void)
{
    create();
    setup(3, 30, 5);
    pull(300);
    expect_action(GUN_ACT_FIRE, 0, 4);
    gun_logic_magazine(g, true, now); /* it is in already: nothing */
    expect_no_action();
    TEST_ASSERT_EQUAL_UINT8(4, gun_logic_outputs(g)->ammo_left);
    gun_logic_magazine(g, false, now);
    gun_logic_magazine(g, false, now);
    pull(300);
    expect_action(GUN_ACT_DRY, 0, 4); /* rounds left, but no magazine */
    gun_logic_magazine(g, true, now);
    expect_action(GUN_ACT_RELOADED, 0, 5);
    /* a new setup refills too */
    pull(300);
    expect_action(GUN_ACT_FIRE, 1, 4);
    setup(3, 30, 8);
    TEST_ASSERT_EQUAL_UINT8(8, gun_logic_outputs(g)->ammo_left);
    /* without the magazine feature the switch means nothing */
    setup(3, 30, 0);
    gun_logic_magazine(g, false, now);
    pull(300);
    expect_action(GUN_ACT_FIRE, 2, GUN_NO_MAGAZINE);
    gun_logic_magazine(g, true, now);
    expect_no_action();
}

static void test_holding_the_trigger_requests_a_demo(void)
{
    create();
    gun_logic_trigger(g, true, now);
    expect_action(GUN_ACT_FIRE, 0, GUN_NO_MAGAZINE);
    now += 2999;
    gun_logic_tick(g, now);
    expect_no_action();
    now += 1;
    gun_logic_tick(g, now);
    expect_action(GUN_ACT_DEMO_REQUEST, 0, GUN_NO_MAGAZINE);
    now += 5000;
    gun_logic_tick(g, now);
    expect_no_action(); /* once per press */
    gun_logic_trigger(g, false, now);
    now += 300;
    gun_logic_trigger(g, true, now);
    expect_action(GUN_ACT_FIRE, 1, GUN_NO_MAGAZINE);
    now += 2000;
    gun_logic_trigger(g, false, now); /* released before 3 s */
    now += 2000;
    gun_logic_tick(g, now);
    expect_no_action();
}

static void test_sleep_after_idle(void)
{
    create();
    setup(1, 45, 0);
    now += 59999;
    gun_logic_tick(g, now);
    TEST_ASSERT_FALSE(gun_logic_outputs(g)->sleep);
    TEST_ASSERT_EQUAL_UINT8(45, gun_logic_outputs(g)->sights);
    now += 1;
    gun_logic_tick(g, now);
    TEST_ASSERT_TRUE(gun_logic_outputs(g)->sleep);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, gun_logic_outputs(g)->sights, "sights off before sleeping");
    gun_logic_motion(g, now); /* picked up again */
    TEST_ASSERT_FALSE(gun_logic_outputs(g)->sleep);
    TEST_ASSERT_EQUAL_UINT8(45, gun_logic_outputs(g)->sights);

    /* motion and the trigger keep it awake */
    for (int i = 0; i < 10; i++) {
        now += 50000;
        gun_logic_motion(g, now);
        gun_logic_tick(g, now);
        TEST_ASSERT_FALSE(gun_logic_outputs(g)->sleep);
    }
    now += 50000;
    pull(300);
    now += 59000;
    gun_logic_tick(g, now);
    TEST_ASSERT_FALSE(gun_logic_outputs(g)->sleep);
    now += 1000;
    gun_logic_tick(g, now);
    TEST_ASSERT_TRUE(gun_logic_outputs(g)->sleep);

    /* a held trigger never sleeps */
    gun_logic_trigger(g, true, now);
    TEST_ASSERT_FALSE(gun_logic_outputs(g)->sleep);
    now += 200000;
    gun_logic_tick(g, now);
    TEST_ASSERT_FALSE(gun_logic_outputs(g)->sleep);
    gun_logic_trigger(g, false, now);
    now += 60000;
    gun_logic_tick(g, now);
    TEST_ASSERT_TRUE(gun_logic_outputs(g)->sleep);
}

static void test_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 100u;
    create();
    pull(300); /* the press is before the wrap, the next one after */
    expect_action(GUN_ACT_FIRE, 0, GUN_NO_MAGAZINE);
    pull(300);
    expect_action(GUN_ACT_FIRE, 1, GUN_NO_MAGAZINE);
    now += 59800; /* 60 s after the release */
    gun_logic_tick(g, now);
    TEST_ASSERT_TRUE(gun_logic_outputs(g)->sleep);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_setup);
    RUN_TEST(test_each_press_fires_once);
    RUN_TEST(test_shot_counter_wraps_at_16);
    RUN_TEST(test_cooldown);
    RUN_TEST(test_magazine_runs_empty);
    RUN_TEST(test_magazine_details);
    RUN_TEST(test_holding_the_trigger_requests_a_demo);
    RUN_TEST(test_sleep_after_idle);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
