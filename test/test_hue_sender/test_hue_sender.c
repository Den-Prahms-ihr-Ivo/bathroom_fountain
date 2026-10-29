/*
 * Tests for lib/hue_sender – requirements LIGHT-02, NFR-09.
 */
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "hue_api.h"
#include "hue_sender.h"
#include "mode_controller.h"

#define T0 100000u

static hue_sender_t *hs;
static const hue_command_t CMD_A = {HUE_SCENE_AMBIENT, 4000};
static const hue_command_t CMD_B = {HUE_SCENE_NORMAL, 4000};

void setUp(void)
{
    hs = hue_sender_create();
    TEST_ASSERT_NOT_NULL_MESSAGE(hs, "hue_sender_create returned NULL");
}

void tearDown(void)
{
    hue_sender_destroy(hs);
    hs = NULL;
}

static void expect_send(uint32_t now, const hue_command_t *expected)
{
    hue_command_t out;
    memset(&out, 0xA5, sizeof(out));
    TEST_ASSERT_TRUE_MESSAGE(hue_sender_poll(hs, now, &out), "expected a command to send");
    TEST_ASSERT_EQUAL_INT(expected->scene, out.scene);
    TEST_ASSERT_EQUAL_UINT32(expected->duration_ms, out.duration_ms);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_IN_FLIGHT, hue_sender_state(hs));
}

static void expect_nothing(uint32_t now)
{
    hue_command_t out;
    TEST_ASSERT_FALSE_MESSAGE(hue_sender_poll(hs, now, &out), "expected nothing to send");
}

/* ------------------------------------------------------------------------ */

static void test_starts_idle(void)
{
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_IDLE, hue_sender_state(hs));
    TEST_ASSERT_EQUAL_UINT32(0, hue_sender_attempts(hs));
    TEST_ASSERT_EQUAL_INT(HUE_OK, hue_sender_last_result(hs));
    expect_nothing(T0);
}

static void test_submitted_command_is_sent_once(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_PENDING, hue_sender_state(hs));
    TEST_ASSERT_EQUAL_UINT32(0, hue_sender_attempts(hs));
    expect_send(T0, &CMD_A);
    TEST_ASSERT_EQUAL_UINT32(1, hue_sender_attempts(hs));
    expect_nothing(T0 + 60000);
}

static void test_success_goes_idle(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_report(hs, HUE_OK, T0 + 80);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_IDLE, hue_sender_state(hs));
    TEST_ASSERT_EQUAL_UINT32(0, hue_sender_attempts(hs));
    expect_nothing(T0 + 60000);
}

static void test_retryable_failure_is_retried_after_backoff(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_report(hs, HUE_ERR_TRANSPORT, T0 + 100);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_WAITING_RETRY, hue_sender_state(hs));
    TEST_ASSERT_EQUAL_INT(HUE_ERR_TRANSPORT, hue_sender_last_result(hs));
    expect_nothing(T0 + 599);
    expect_send(T0 + 600, &CMD_A);
    TEST_ASSERT_EQUAL_UINT32(2, hue_sender_attempts(hs));

    hue_sender_report(hs, HUE_ERR_BRIDGE_BUSY, T0 + 700);
    expect_nothing(T0 + 1699);
    expect_send(T0 + 1700, &CMD_A);
    TEST_ASSERT_EQUAL_UINT32(3, hue_sender_attempts(hs));
}

static void test_backoff_doubles_and_never_gives_up(void)
{
    const uint32_t delays[] = {500, 1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000, 30000};
    uint32_t t = T0;
    hue_sender_submit(hs, &CMD_A, t);
    expect_send(t, &CMD_A);
    for (size_t i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        hue_sender_report(hs, HUE_ERR_RATE_LIMITED, t);
        expect_nothing(t + delays[i] - 1);
        t += delays[i];
        expect_send(t, &CMD_A);
    }
    TEST_ASSERT_EQUAL_UINT32(11, hue_sender_attempts(hs));
}

static void test_latest_wins_before_sending(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    hue_sender_submit(hs, &CMD_B, T0 + 5);
    expect_send(T0 + 5, &CMD_B);
    hue_sender_report(hs, HUE_OK, T0 + 50);
    expect_nothing(T0 + 60000);
}

static void test_submit_during_retry_wait_sends_new_command_now(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_report(hs, HUE_ERR_TRANSPORT, T0 + 100);
    hue_sender_submit(hs, &CMD_B, T0 + 200);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_PENDING, hue_sender_state(hs));
    TEST_ASSERT_EQUAL_UINT32(0, hue_sender_attempts(hs));
    expect_send(T0 + 200, &CMD_B);
    TEST_ASSERT_EQUAL_UINT32(1, hue_sender_attempts(hs));
}

static void test_submit_during_flight_waits_for_report(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_submit(hs, &CMD_B, T0 + 10);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_IN_FLIGHT, hue_sender_state(hs));
    expect_nothing(T0 + 20);
    hue_sender_report(hs, HUE_OK, T0 + 90);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_PENDING, hue_sender_state(hs));
    expect_send(T0 + 90, &CMD_B);
    TEST_ASSERT_EQUAL_UINT32(1, hue_sender_attempts(hs));
}

static void test_newer_command_supersedes_failed_flight(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_submit(hs, &CMD_B, T0 + 10);
    hue_sender_report(hs, HUE_ERR_TRANSPORT, T0 + 90);
    expect_send(T0 + 90, &CMD_B); /* no backoff, and not A */
}

static void test_non_retryable_errors_drop_the_command(void)
{
    const hue_result_t results[] = {HUE_ERR_NOT_FOUND, HUE_ERR_REJECTED, HUE_ERR_BAD_RESPONSE};
    uint32_t t = T0;
    for (size_t i = 0; i < 3; i++) {
        hue_sender_submit(hs, &CMD_A, t);
        expect_send(t, &CMD_A);
        hue_sender_report(hs, results[i], t + 50);
        TEST_ASSERT_EQUAL_INT(HUE_SENDER_IDLE, hue_sender_state(hs));
        TEST_ASSERT_EQUAL_INT(results[i], hue_sender_last_result(hs));
        expect_nothing(t + 120000);
        t += 200000;
    }
}

static void test_auth_error_waits_for_pairing(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_report(hs, HUE_ERR_AUTH, T0 + 50);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_NEEDS_PAIRING, hue_sender_state(hs));
    expect_nothing(T0 + 3600000u);

    hue_sender_paired(hs, T0 + 3600000u);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_PENDING, hue_sender_state(hs));
    expect_send(T0 + 3600000u, &CMD_A);
    TEST_ASSERT_EQUAL_UINT32(1, hue_sender_attempts(hs));
}

static void test_submit_while_unpaired_replaces_kept_command(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_report(hs, HUE_ERR_AUTH, T0 + 50);
    hue_sender_submit(hs, &CMD_B, T0 + 1000);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_NEEDS_PAIRING, hue_sender_state(hs));
    expect_nothing(T0 + 1000);
    hue_sender_paired(hs, T0 + 2000);
    expect_send(T0 + 2000, &CMD_B);
}

static void test_auth_error_keeps_the_newer_command(void)
{
    hue_sender_submit(hs, &CMD_A, T0);
    expect_send(T0, &CMD_A);
    hue_sender_submit(hs, &CMD_B, T0 + 10);
    hue_sender_report(hs, HUE_ERR_AUTH, T0 + 50);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_NEEDS_PAIRING, hue_sender_state(hs));
    hue_sender_paired(hs, T0 + 100);
    expect_send(T0 + 100, &CMD_B);
}

static void test_paired_without_command_goes_idle(void)
{
    hue_sender_paired(hs, T0);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_IDLE, hue_sender_state(hs));
    expect_nothing(T0);
}

static void test_report_without_flight_is_ignored(void)
{
    hue_sender_report(hs, HUE_ERR_TRANSPORT, T0);
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_IDLE, hue_sender_state(hs));
    TEST_ASSERT_EQUAL_INT(HUE_OK, hue_sender_last_result(hs));

    hue_sender_submit(hs, &CMD_A, T0 + 10);
    hue_sender_report(hs, HUE_OK, T0 + 20); /* not sent yet */
    TEST_ASSERT_EQUAL_INT(HUE_SENDER_PENDING, hue_sender_state(hs));
    expect_send(T0 + 30, &CMD_A);
}

static void test_retry_timing_across_counter_wrap(void)
{
    const uint32_t t = UINT32_MAX - 200u;
    hue_sender_submit(hs, &CMD_A, t);
    expect_send(t, &CMD_A);
    hue_sender_report(hs, HUE_ERR_TRANSPORT, t);
    expect_nothing(t + 100u); /* not wrapped yet, due time already wrapped */
    expect_nothing(t + 499u);
    expect_send(t + 500u, &CMD_A);
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_idle);
    RUN_TEST(test_submitted_command_is_sent_once);
    RUN_TEST(test_success_goes_idle);
    RUN_TEST(test_retryable_failure_is_retried_after_backoff);
    RUN_TEST(test_backoff_doubles_and_never_gives_up);
    RUN_TEST(test_latest_wins_before_sending);
    RUN_TEST(test_submit_during_retry_wait_sends_new_command_now);
    RUN_TEST(test_submit_during_flight_waits_for_report);
    RUN_TEST(test_newer_command_supersedes_failed_flight);
    RUN_TEST(test_non_retryable_errors_drop_the_command);
    RUN_TEST(test_auth_error_waits_for_pairing);
    RUN_TEST(test_submit_while_unpaired_replaces_kept_command);
    RUN_TEST(test_auth_error_keeps_the_newer_command);
    RUN_TEST(test_paired_without_command_goes_idle);
    RUN_TEST(test_report_without_flight_is_ignored);
    RUN_TEST(test_retry_timing_across_counter_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
