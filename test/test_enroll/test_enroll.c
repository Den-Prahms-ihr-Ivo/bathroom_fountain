/*
 * Tests for lib/enroll – requirements PAIR-01 ... PAIR-08.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "enroll.h"

#define T0 5000u

static enroll_config_t cfg;
static enroll_t *e;

void setUp(void)
{
    enroll_config_default(&cfg);
    e = NULL;
}

void tearDown(void)
{
    enroll_destroy(e);
    e = NULL;
}

static void create(bool claimed)
{
    e = enroll_create(&cfg, claimed, T0);
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "enroll_create returned NULL for a valid config");
}

static enroll_reply_t reply(enroll_reply_kind_t kind, uint16_t window_s)
{
    enroll_reply_t r;
    memset(&r, 0, sizeof(r));
    r.kind = kind;
    r.window_s = window_s;
    if (kind == ENROLL_REPLY_CLAIMED) {
        strcpy(r.creds.device_id, "bath-3f2a");
        strcpy(r.creds.mqtt_host, "ambience.example.org");
        r.creds.mqtt_port = 8883;
        strcpy(r.creds.mqtt_user, "bath-3f2a");
        strcpy(r.creds.mqtt_pass, "s3cret-pass");
    }
    if (kind == ENROLL_REPLY_REJECTED) {
        strcpy(r.reason, "conflict");
    }
    return r;
}

static bool feed(enroll_reply_kind_t kind, uint16_t window_s, uint32_t now)
{
    const enroll_reply_t r = reply(kind, window_s);
    return enroll_reply(e, &r, now);
}

static void expect_act(enroll_act_t a)
{
    enroll_act_t got = (enroll_act_t)0;
    TEST_ASSERT_TRUE_MESSAGE(enroll_take(e, &got), "an action was expected");
    TEST_ASSERT_EQUAL_INT_MESSAGE(a, got, "action");
}

static void expect_none(void)
{
    enroll_act_t got;
    TEST_ASSERT_FALSE_MESSAGE(enroll_take(e, &got), "no (further) action expected");
}

static void drain(void)
{
    enroll_act_t got;
    while (enroll_take(e, &got)) {
    }
}

static void expect_state(enroll_state_t s, bool blink, bool rejected)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(s, enroll_outputs(e)->state, "state");
    TEST_ASSERT_EQUAL_MESSAGE(blink, enroll_outputs(e)->blink, "blink");
    TEST_ASSERT_EQUAL_MESSAGE(rejected, enroll_outputs(e)->rejected, "rejected");
}

/* Unclaimed, server asked to confirm at T0 with a 60 s window. */
static void to_confirm(void)
{
    create(false);
    enroll_tick(e, T0);
    drain();
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_CONFIRM, 60, T0));
    drain();
}

/* ---- configuration and start --------------------------------------------- */

static void test_defaults_and_validation(void)
{
    enroll_config_t d;
    memset(&d, 0, sizeof(d));
    enroll_config_default(&d);
    TEST_ASSERT_EQUAL_UINT32(10000, d.hello_ms);
    TEST_ASSERT_EQUAL_UINT32(2000, d.poll_ms);
    TEST_ASSERT_EQUAL_UINT8(3, d.auth_fail_limit);
    TEST_ASSERT_NULL(enroll_create(NULL, false, T0));
    enroll_config_t b;
    b = d; b.hello_ms = 999;      TEST_ASSERT_NULL(enroll_create(&b, false, T0));
    b = d; b.hello_ms = 600001;   TEST_ASSERT_NULL(enroll_create(&b, false, T0));
    b = d; b.poll_ms = 499;       TEST_ASSERT_NULL(enroll_create(&b, false, T0));
    b = d; b.poll_ms = 60001;     TEST_ASSERT_NULL(enroll_create(&b, false, T0));
    b = d; b.auth_fail_limit = 0; TEST_ASSERT_NULL(enroll_create(&b, false, T0));
    b = d; b.auth_fail_limit = 21; TEST_ASSERT_NULL(enroll_create(&b, false, T0));
    b = d; b.hello_ms = 1000; b.poll_ms = 500; b.auth_fail_limit = 1;
    enroll_t *ok = enroll_create(&b, false, T0);
    TEST_ASSERT_NOT_NULL(ok);
    enroll_destroy(ok);
    b = d; b.hello_ms = 600000; b.poll_ms = 60000; b.auth_fail_limit = 20;
    ok = enroll_create(&b, true, T0);
    TEST_ASSERT_NOT_NULL(ok);
    enroll_destroy(ok);
    enroll_destroy(NULL);
}

static void test_claimed_device_does_nothing(void)
{
    create(true);
    expect_state(ENROLL_CLAIMED, false, false);
    enroll_tick(e, T0 + 3600000u);
    expect_none();
    TEST_ASSERT_FALSE(feed(ENROLL_REPLY_CONFIRM, 60, T0 + 1));
    TEST_ASSERT_FALSE(feed(ENROLL_REPLY_CLAIMED, 0, T0 + 2));
    TEST_ASSERT_FALSE_MESSAGE(enroll_button_held(e, T0 + 3), "hold is not used: the app does its own thing");
    expect_none();
    expect_state(ENROLL_CLAIMED, false, false);
}

static void test_unclaimed_says_hello_periodically(void)
{
    create(false);
    expect_state(ENROLL_UNCLAIMED, false, false);
    enroll_tick(e, T0);
    expect_act(ENROLL_ACT_HELLO);
    expect_none();
    enroll_tick(e, T0 + 9999);
    expect_none();
    enroll_tick(e, T0 + 10000);
    expect_act(ENROLL_ACT_HELLO);
    TEST_ASSERT_FALSE_MESSAGE(feed(ENROLL_REPLY_WAIT, 0, T0 + 10100), "wait changes nothing");
    enroll_tick(e, T0 + 19999);
    expect_none();
    enroll_tick(e, T0 + 20000);
    expect_act(ENROLL_ACT_HELLO);
}

static void test_late_tick_sends_one_hello(void)
{
    create(false);
    enroll_tick(e, T0 + 95000);
    expect_act(ENROLL_ACT_HELLO);
    expect_none();
    enroll_tick(e, T0 + 104999);
    expect_none();
    enroll_tick(e, T0 + 105000);
    expect_act(ENROLL_ACT_HELLO);
}

static void test_hold_while_unclaimed_is_not_used(void)
{
    create(false);
    enroll_tick(e, T0);
    drain();
    TEST_ASSERT_FALSE(enroll_button_held(e, T0 + 1));
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
}

static void test_credentials_without_confirm_are_ignored(void)
{
    create(false);
    enroll_tick(e, T0);
    drain();
    TEST_ASSERT_FALSE(feed(ENROLL_REPLY_CLAIMED, 0, T0 + 1));
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
    TEST_ASSERT_EQUAL_STRING("", enroll_creds(e)->device_id);
}

/* ---- confirm window ------------------------------------------------------ */

static void test_confirm_opens_window_and_blinks(void)
{
    to_confirm();
    expect_state(ENROLL_CONFIRM, true, false);
    enroll_tick(e, T0 + 1999);
    expect_none();
    enroll_tick(e, T0 + 2000);
    expect_act(ENROLL_ACT_HELLO); /* polls faster now */
    enroll_tick(e, T0 + 4000);
    expect_act(ENROLL_ACT_HELLO);
    expect_none();
}

static void test_window_expires(void)
{
    to_confirm();
    enroll_tick(e, T0 + 59999);
    drain();
    expect_state(ENROLL_CONFIRM, true, false);
    enroll_tick(e, T0 + 60000);
    expect_act(ENROLL_ACT_HELLO);
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
    enroll_tick(e, T0 + 69999);
    expect_none();
    enroll_tick(e, T0 + 70000);
    expect_act(ENROLL_ACT_HELLO);
    TEST_ASSERT_FALSE_MESSAGE(enroll_button_held(e, T0 + 70001), "too late");
}

static void test_confirm_again_renews_window(void)
{
    to_confirm();
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_CONFIRM, 30, T0 + 50000));
    enroll_tick(e, T0 + 79999);
    drain();
    expect_state(ENROLL_CONFIRM, true, false);
    enroll_tick(e, T0 + 80000);
    drain();
    expect_state(ENROLL_UNCLAIMED, false, false);
}

static void test_user_cancels(void)
{
    to_confirm();
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_WAIT, 0, T0 + 1500)); /* before the next poll */
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
    enroll_tick(e, T0 + 11499);
    expect_none();
    enroll_tick(e, T0 + 11500);
    expect_act(ENROLL_ACT_HELLO);
}

static void test_claimed_answer_needs_the_hold(void)
{
    to_confirm();
    TEST_ASSERT_FALSE(feed(ENROLL_REPLY_CLAIMED, 0, T0 + 100));
    expect_none();
    expect_state(ENROLL_CONFIRM, true, false);
}

/* ---- the hold and the credentials ---------------------------------------- */

static void test_hold_confirms_and_saves(void)
{
    to_confirm();
    TEST_ASSERT_TRUE(enroll_button_held(e, T0 + 1000));
    expect_act(ENROLL_ACT_CONFIRM);
    expect_none();
    expect_state(ENROLL_CONFIRMING, true, false);

    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_CLAIMED, 0, T0 + 1300));
    expect_act(ENROLL_ACT_SAVE);
    expect_none();
    expect_state(ENROLL_CLAIMED, false, false);
    const enroll_creds_t *c = enroll_creds(e);
    TEST_ASSERT_EQUAL_STRING("bath-3f2a", c->device_id);
    TEST_ASSERT_EQUAL_STRING("ambience.example.org", c->mqtt_host);
    TEST_ASSERT_EQUAL_UINT16(8883, c->mqtt_port);
    TEST_ASSERT_EQUAL_STRING("bath-3f2a", c->mqtt_user);
    TEST_ASSERT_EQUAL_STRING("s3cret-pass", c->mqtt_pass);
    enroll_tick(e, T0 + 600000);
    expect_none();
}

static void test_confirm_is_retried_until_answered(void)
{
    to_confirm();
    enroll_button_held(e, T0 + 1000);
    drain();
    TEST_ASSERT_TRUE_MESSAGE(enroll_button_held(e, T0 + 1500), "second hold: used, nothing new");
    expect_none();
    enroll_tick(e, T0 + 2999);
    expect_none();
    enroll_tick(e, T0 + 3000);
    expect_act(ENROLL_ACT_CONFIRM);
    TEST_ASSERT_FALSE_MESSAGE(feed(ENROLL_REPLY_CONFIRM, 60, T0 + 3100), "server not done yet");
    enroll_tick(e, T0 + 5000);
    expect_act(ENROLL_ACT_CONFIRM);
    expect_none();
    expect_state(ENROLL_CONFIRMING, true, false);
}

static void test_confirming_runs_out_with_window(void)
{
    to_confirm();
    enroll_button_held(e, T0 + 50000);
    drain();
    enroll_tick(e, T0 + 60000);
    expect_act(ENROLL_ACT_HELLO);
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
}

static void test_server_refuses_confirm(void)
{
    to_confirm();
    enroll_button_held(e, T0 + 1000);
    drain();
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_REJECTED, 0, T0 + 1200));
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, true);
    enroll_tick(e, T0 + 11200);
    expect_act(ENROLL_ACT_HELLO);

    /* WAIT during confirming: back to unclaimed, not rejected */
    tearDown();
    to_confirm();
    enroll_button_held(e, T0 + 1000);
    drain();
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_WAIT, 0, T0 + 1200));
    expect_state(ENROLL_UNCLAIMED, false, false);
}

static void test_rejected_flag(void)
{
    create(false);
    enroll_tick(e, T0);
    drain();
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_REJECTED, 0, T0 + 1));
    expect_state(ENROLL_UNCLAIMED, false, true);
    enroll_tick(e, T0 + 10000);
    expect_act(ENROLL_ACT_HELLO); /* keeps checking in */
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_CONFIRM, 60, T0 + 10001));
    expect_state(ENROLL_CONFIRM, true, false); /* cleared on leaving UNCLAIMED */
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_REJECTED, 0, T0 + 10002));
    expect_state(ENROLL_UNCLAIMED, false, true);
}

/* ---- after pairing ------------------------------------------------------- */

static void claimed_via_pairing(void)
{
    to_confirm();
    enroll_button_held(e, T0 + 1000);
    feed(ENROLL_REPLY_CLAIMED, 0, T0 + 1100);
    drain();
}

static void test_auth_failures_clear_credentials(void)
{
    claimed_via_pairing();
    enroll_auth_result(e, false, T0 + 2000);
    enroll_auth_result(e, false, T0 + 3000);
    expect_none();
    expect_state(ENROLL_CLAIMED, false, false);
    enroll_auth_result(e, false, T0 + 4000);
    expect_act(ENROLL_ACT_CLEAR);
    expect_act(ENROLL_ACT_HELLO);
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
    enroll_tick(e, T0 + 13999);
    expect_none();
    enroll_tick(e, T0 + 14000);
    expect_act(ENROLL_ACT_HELLO);
}

static void test_auth_ok_resets_count(void)
{
    create(true);
    enroll_auth_result(e, false, T0);
    enroll_auth_result(e, false, T0 + 1);
    enroll_auth_result(e, true, T0 + 2);
    enroll_auth_result(e, false, T0 + 3);
    enroll_auth_result(e, false, T0 + 4);
    expect_none();
    expect_state(ENROLL_CLAIMED, false, false);
    enroll_auth_result(e, false, T0 + 5);
    expect_act(ENROLL_ACT_CLEAR);
}

static void test_auth_results_ignored_when_unclaimed(void)
{
    create(false);
    enroll_tick(e, T0);
    drain();
    for (int i = 0; i < 10; i++) {
        enroll_auth_result(e, false, T0 + 1 + (uint32_t)i);
    }
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
}

static void test_auth_limit_one(void)
{
    cfg.auth_fail_limit = 1;
    create(true);
    enroll_auth_result(e, false, T0);
    expect_act(ENROLL_ACT_CLEAR);
    expect_act(ENROLL_ACT_HELLO);
}

static void test_forget(void)
{
    create(true);
    TEST_ASSERT_TRUE(enroll_forget(e, T0 + 10));
    expect_act(ENROLL_ACT_CLEAR);
    expect_act(ENROLL_ACT_HELLO);
    expect_none();
    expect_state(ENROLL_UNCLAIMED, false, false);
    TEST_ASSERT_FALSE(enroll_forget(e, T0 + 20));
    expect_none();
}

static void test_pairing_again_after_forget(void)
{
    claimed_via_pairing();
    enroll_forget(e, T0 + 2000);
    drain();
    TEST_ASSERT_TRUE(feed(ENROLL_REPLY_CONFIRM, 60, T0 + 3000));
    TEST_ASSERT_TRUE(enroll_button_held(e, T0 + 4000));
    expect_act(ENROLL_ACT_CONFIRM);
}

/* ---- clock, queue, NULL -------------------------------------------------- */

static void test_window_across_counter_wrap(void)
{
    const uint32_t t = 0xFFFFFFFFu - 20000u;
    e = enroll_create(&cfg, false, t);
    TEST_ASSERT_NOT_NULL(e);
    enroll_tick(e, t);
    drain();
    feed(ENROLL_REPLY_CONFIRM, 60, t);
    enroll_tick(e, t + 59999u);
    drain();
    expect_state(ENROLL_CONFIRM, true, false);
    enroll_tick(e, t + 60000u);
    drain();
    expect_state(ENROLL_UNCLAIMED, false, false);
}

static void test_queue_drops_oldest(void)
{
    cfg.hello_ms = 1000;
    create(false);
    for (uint32_t i = 0; i < ENROLL_ACTS + 3; i++) {
        enroll_tick(e, T0 + i * 1000u);
    }
    for (int i = 0; i < ENROLL_ACTS; i++) {
        expect_act(ENROLL_ACT_HELLO);
    }
    expect_none();
}

static void test_null_arguments(void)
{
    TEST_ASSERT_FALSE(enroll_reply(NULL, NULL, T0));
    TEST_ASSERT_FALSE(enroll_button_held(NULL, T0));
    enroll_tick(NULL, T0);
    enroll_auth_result(NULL, false, T0);
    TEST_ASSERT_FALSE(enroll_forget(NULL, T0));
    TEST_ASSERT_NULL(enroll_outputs(NULL));
    TEST_ASSERT_NULL(enroll_creds(NULL));
    create(false);
    TEST_ASSERT_FALSE(enroll_reply(e, NULL, T0));
    TEST_ASSERT_FALSE(enroll_take(e, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_claimed_device_does_nothing);
    RUN_TEST(test_unclaimed_says_hello_periodically);
    RUN_TEST(test_late_tick_sends_one_hello);
    RUN_TEST(test_hold_while_unclaimed_is_not_used);
    RUN_TEST(test_credentials_without_confirm_are_ignored);
    RUN_TEST(test_confirm_opens_window_and_blinks);
    RUN_TEST(test_window_expires);
    RUN_TEST(test_confirm_again_renews_window);
    RUN_TEST(test_user_cancels);
    RUN_TEST(test_claimed_answer_needs_the_hold);
    RUN_TEST(test_hold_confirms_and_saves);
    RUN_TEST(test_confirm_is_retried_until_answered);
    RUN_TEST(test_confirming_runs_out_with_window);
    RUN_TEST(test_server_refuses_confirm);
    RUN_TEST(test_rejected_flag);
    RUN_TEST(test_auth_failures_clear_credentials);
    RUN_TEST(test_auth_ok_resets_count);
    RUN_TEST(test_auth_results_ignored_when_unclaimed);
    RUN_TEST(test_auth_limit_one);
    RUN_TEST(test_forget);
    RUN_TEST(test_pairing_again_after_forget);
    RUN_TEST(test_window_across_counter_wrap);
    RUN_TEST(test_queue_drops_oldest);
    RUN_TEST(test_null_arguments);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
