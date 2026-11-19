/*
 * Tests for lib/link_peer – requirements LINK-05 ... LINK-09.
 * Frames are six header bytes plus two payload bytes; only the seq matters.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "link_peer.h"

static link_peer_config_t cfg;
static link_peer_t *lp;
static uint32_t now;
static uint8_t tx[LINK_PEER_FRAME_MAX];

void setUp(void)
{
    link_peer_config_default(&cfg);
    cfg.retry_ms = 60;
    cfg.max_tries = 5;
    cfg.alive_timeout_ms = 15000;
    cfg.heartbeat_ms = 5000;
    lp = NULL;
    now = 700000u;
    memset(tx, 0, sizeof(tx));
}

void tearDown(void)
{
    link_peer_destroy(lp);
    lp = NULL;
}

static void create(void)
{
    lp = link_peer_create(&cfg, now);
    TEST_ASSERT_NOT_NULL_MESSAGE(lp, "link_peer_create returned NULL for a valid config");
}

static bool send(uint16_t seq)
{
    const uint8_t f[8] = {0xBA, 1, 5, 1, (uint8_t)seq, (uint8_t)(seq >> 8), 0x11, (uint8_t)seq};
    return link_peer_send(lp, f, sizeof(f), now);
}

/* seq of the frame that is due, or 0 */
static uint16_t take(void)
{
    memset(tx, 0, sizeof(tx));
    const size_t n = link_peer_take_tx(lp, now, tx, sizeof(tx));
    if (n == 0) {
        return 0;
    }
    TEST_ASSERT_EQUAL_size_t(8, n);
    TEST_ASSERT_EQUAL_HEX8(0xBA, tx[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11, tx[6]);
    return (uint16_t)(tx[4] | (tx[5] << 8));
}

static void expect_event(link_peer_event_id_t id, uint16_t seq)
{
    link_peer_event_t e;
    TEST_ASSERT_TRUE_MESSAGE(link_peer_take_event(lp, &e), "event missing");
    TEST_ASSERT_EQUAL_INT(id, e.id);
    TEST_ASSERT_EQUAL_UINT16(seq, e.seq);
}

static void expect_no_event(void)
{
    link_peer_event_t e;
    TEST_ASSERT_FALSE(link_peer_take_event(lp, &e));
}

static void test_defaults_and_validation(void)
{
    link_peer_config_t d;
    memset(&d, 0, sizeof(d));
    link_peer_config_default(&d);
    TEST_ASSERT_EQUAL_UINT16(60, d.retry_ms);
    TEST_ASSERT_EQUAL_UINT8(5, d.max_tries);
    TEST_ASSERT_EQUAL_UINT32(15000, d.alive_timeout_ms);
    TEST_ASSERT_EQUAL_UINT32(5000, d.heartbeat_ms);
    create();
    TEST_ASSERT_FALSE(link_peer_alive(lp));
    TEST_ASSERT_EQUAL_size_t(0, link_peer_pending(lp));
    expect_no_event();

    link_peer_config_t bad = d;
    bad.retry_ms = 9;
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    bad.retry_ms = 1001;
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    bad = d;
    bad.max_tries = 0;
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    bad.max_tries = 21;
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    bad = d;
    bad.alive_timeout_ms = 999;
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    bad.alive_timeout_ms = 600001;
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    bad = d;
    bad.heartbeat_ms = 499;
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    bad.heartbeat_ms = 15000; /* not below the alive timeout */
    TEST_ASSERT_NULL(link_peer_create(&bad, now));
    TEST_ASSERT_NULL(link_peer_create(NULL, now));
    link_peer_destroy(NULL);
}

static void test_sequence_numbers_skip_zero(void)
{
    create();
    TEST_ASSERT_EQUAL_UINT16(1, link_peer_next_seq(lp));
    TEST_ASSERT_EQUAL_UINT16(2, link_peer_next_seq(lp));
    for (uint32_t i = 3; i <= 65535u; i++) {
        TEST_ASSERT_EQUAL_UINT16((uint16_t)i, link_peer_next_seq(lp));
    }
    TEST_ASSERT_EQUAL_UINT16(1, link_peer_next_seq(lp));
}

static void test_frame_goes_out_once_then_on_each_retry(void)
{
    create();
    TEST_ASSERT_EQUAL_UINT16(0, take());
    TEST_ASSERT_TRUE(send(7));
    TEST_ASSERT_EQUAL_size_t(1, link_peer_pending(lp));
    TEST_ASSERT_EQUAL_UINT16(7, take());
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, take(), "not again before retry_ms");
    now += 59;
    TEST_ASSERT_EQUAL_UINT16(0, take());
    now += 1;
    TEST_ASSERT_EQUAL_UINT16(7, take());
    TEST_ASSERT_EQUAL_UINT16(0, take());
    now += 60;
    TEST_ASSERT_EQUAL_UINT16(7, take());
    expect_no_event();
}

static void test_ack_ends_the_retries(void)
{
    create();
    send(7);
    TEST_ASSERT_EQUAL_UINT16(7, take());
    now += 20;
    link_peer_on_ack(lp, 8, now); /* some other seq */
    TEST_ASSERT_EQUAL_size_t(1, link_peer_pending(lp));
    link_peer_on_ack(lp, 7, now);
    TEST_ASSERT_EQUAL_size_t(0, link_peer_pending(lp));
    expect_event(LINK_PEER_DELIVERED, 7);
    expect_no_event();
    now += 500;
    link_peer_tick(lp, now);
    TEST_ASSERT_EQUAL_UINT16(0, take());
    link_peer_on_ack(lp, 7, now); /* a late repeat of the ack */
    expect_no_event();
}

static void test_gives_up_after_max_tries(void)
{
    create();
    send(9);
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT16(9, take());
        now += 59;
        link_peer_tick(lp, now);
        expect_no_event();
        TEST_ASSERT_EQUAL_size_t(1, link_peer_pending(lp));
        now += 1;
    }
    link_peer_tick(lp, now); /* 60 ms after the fifth transmission */
    expect_event(LINK_PEER_FAILED, 9);
    TEST_ASSERT_EQUAL_size_t(0, link_peer_pending(lp));
    TEST_ASSERT_EQUAL_UINT16(0, take());
    link_peer_on_ack(lp, 9, now); /* too late */
    expect_no_event();
}

static void test_frames_go_in_order_one_at_a_time(void)
{
    create();
    TEST_ASSERT_TRUE(send(1));
    TEST_ASSERT_TRUE(send(2));
    TEST_ASSERT_TRUE(send(3));
    TEST_ASSERT_EQUAL_UINT16(1, take());
    TEST_ASSERT_EQUAL_UINT16(0, take());
    link_peer_on_ack(lp, 2, now); /* not the head: ignored */
    TEST_ASSERT_EQUAL_size_t(3, link_peer_pending(lp));
    now += 5;
    link_peer_on_ack(lp, 1, now);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2, take(), "the next frame is due at once");
    now += 5;
    link_peer_on_ack(lp, 2, now);
    TEST_ASSERT_EQUAL_UINT16(3, take());
    link_peer_on_ack(lp, 3, now);
    expect_event(LINK_PEER_DELIVERED, 1);
    expect_event(LINK_PEER_DELIVERED, 2);
    expect_event(LINK_PEER_DELIVERED, 3);
    TEST_ASSERT_EQUAL_size_t(0, link_peer_pending(lp));
}

static void test_a_failed_frame_does_not_block_the_next(void)
{
    cfg.max_tries = 2;
    create();
    send(4);
    send(5);
    TEST_ASSERT_EQUAL_UINT16(4, take());
    now += 60;
    TEST_ASSERT_EQUAL_UINT16(4, take());
    now += 60;
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(5, take(), "4 was given up, 5 is due at once");
    expect_event(LINK_PEER_FAILED, 4);
    link_peer_on_ack(lp, 5, now);
    expect_event(LINK_PEER_DELIVERED, 5);
}

static void test_queue_limits(void)
{
    create();
    for (uint16_t s = 1; s <= LINK_PEER_QUEUE; s++) {
        TEST_ASSERT_TRUE(send(s));
    }
    TEST_ASSERT_FALSE_MESSAGE(send(5), "queue full");
    TEST_ASSERT_EQUAL_size_t(LINK_PEER_QUEUE, link_peer_pending(lp));
    TEST_ASSERT_EQUAL_UINT16(1, take());
    link_peer_on_ack(lp, 1, now);
    TEST_ASSERT_TRUE(send(5));

    const uint8_t f[40] = {0xBA, 1, 5, 1, 9, 0};
    TEST_ASSERT_FALSE(link_peer_send(lp, NULL, 8, now));
    TEST_ASSERT_FALSE(link_peer_send(lp, f, 5, now));
    TEST_ASSERT_FALSE(link_peer_send(lp, f, LINK_PEER_FRAME_MAX + 1, now));
    const uint8_t zero[8] = {0xBA, 1, 5, 1, 0, 0, 0, 0};
    TEST_ASSERT_FALSE_MESSAGE(link_peer_send(lp, zero, 8, now), "seq 0 is never acknowledged");
    TEST_ASSERT_EQUAL_size_t(LINK_PEER_QUEUE, link_peer_pending(lp));

    uint8_t small[4];
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, link_peer_take_tx(lp, now, small, sizeof(small)), "out too small");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(2, take(), "and the attempt was not counted");
}

static void test_duplicates_are_recognised(void)
{
    create();
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 10, now));
    TEST_ASSERT_FALSE_MESSAGE(link_peer_on_rx(lp, 10, now), "the peer repeated it");
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 11, now));
    TEST_ASSERT_FALSE(link_peer_on_rx(lp, 10, now));
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 0, now)); /* unacknowledged frames are never duplicates */
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 0, now));
    /* only the last LINK_PEER_DEDUPE are remembered */
    for (uint16_t s = 12; s < 12 + LINK_PEER_DEDUPE - 2; s++) {
        TEST_ASSERT_TRUE(link_peer_on_rx(lp, s, now));
    }
    TEST_ASSERT_FALSE_MESSAGE(link_peer_on_rx(lp, 10, now), "still among the last eight");
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 100, now));
    TEST_ASSERT_TRUE_MESSAGE(link_peer_on_rx(lp, 10, now), "pushed out of the window");
}

static void test_hello_resets_the_duplicate_filter(void)
{
    create();
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 1, now));
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 2, now));
    TEST_ASSERT_FALSE(link_peer_on_rx(lp, 1, now));
    link_peer_reset_rx(lp); /* the peer restarted and counts from 1 again */
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 1, now));
    TEST_ASSERT_TRUE(link_peer_on_rx(lp, 2, now));
}

static void test_liveness(void)
{
    create();
    now += 60000;
    link_peer_tick(lp, now);
    expect_no_event(); /* never heard: no DOWN */
    TEST_ASSERT_FALSE(link_peer_alive(lp));

    link_peer_on_rx(lp, 0, now);
    TEST_ASSERT_TRUE(link_peer_alive(lp));
    expect_event(LINK_PEER_UP, 0);
    now += 14999;
    link_peer_tick(lp, now);
    TEST_ASSERT_TRUE(link_peer_alive(lp));
    expect_no_event();
    now += 1;
    link_peer_tick(lp, now);
    TEST_ASSERT_FALSE(link_peer_alive(lp));
    expect_event(LINK_PEER_DOWN, 0);
    now += 100000;
    link_peer_tick(lp, now);
    expect_no_event(); /* DOWN is reported once */

    link_peer_on_rx(lp, 5, now);
    expect_event(LINK_PEER_UP, 0);
    for (int i = 0; i < 10; i++) { /* every frame, also a duplicate, keeps it alive */
        now += 10000;
        link_peer_on_rx(lp, 5, now);
        link_peer_tick(lp, now);
    }
    TEST_ASSERT_TRUE(link_peer_alive(lp));
    expect_no_event();
}

static void test_heartbeat_cadence(void)
{
    create();
    TEST_ASSERT_TRUE_MESSAGE(link_peer_heartbeat_due(lp, now), "the first one at once");
    TEST_ASSERT_FALSE(link_peer_heartbeat_due(lp, now));
    now += 4999;
    TEST_ASSERT_FALSE(link_peer_heartbeat_due(lp, now));
    now += 1;
    TEST_ASSERT_TRUE(link_peer_heartbeat_due(lp, now));
    TEST_ASSERT_FALSE(link_peer_heartbeat_due(lp, now));
    now += 12000; /* a late call gives one heartbeat, not two */
    TEST_ASSERT_TRUE(link_peer_heartbeat_due(lp, now));
    TEST_ASSERT_FALSE(link_peer_heartbeat_due(lp, now));
}

static void test_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 100u;
    create();
    link_peer_on_rx(lp, 0, now);
    expect_event(LINK_PEER_UP, 0);
    send(3);
    TEST_ASSERT_EQUAL_UINT16(3, take());
    now += 60; /* still before the wrap */
    TEST_ASSERT_EQUAL_UINT16(3, take());
    now += 60; /* wrapped */
    TEST_ASSERT_EQUAL_UINT16(3, take());
    link_peer_on_ack(lp, 3, now);
    expect_event(LINK_PEER_DELIVERED, 3);
    now += 14000;
    link_peer_tick(lp, now);
    TEST_ASSERT_TRUE(link_peer_alive(lp));
    now += 1000;
    link_peer_tick(lp, now);
    TEST_ASSERT_FALSE(link_peer_alive(lp));
}

static void test_event_queue_keeps_the_newest(void)
{
    create();
    for (uint16_t s = 1; s <= LINK_PEER_EVENTS + 3; s++) {
        send(s);
        TEST_ASSERT_EQUAL_UINT16(s, take());
        link_peer_on_ack(lp, s, now);
    }
    for (uint16_t s = 4; s <= LINK_PEER_EVENTS + 3; s++) {
        expect_event(LINK_PEER_DELIVERED, s);
    }
    expect_no_event();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_sequence_numbers_skip_zero);
    RUN_TEST(test_frame_goes_out_once_then_on_each_retry);
    RUN_TEST(test_ack_ends_the_retries);
    RUN_TEST(test_gives_up_after_max_tries);
    RUN_TEST(test_frames_go_in_order_one_at_a_time);
    RUN_TEST(test_a_failed_frame_does_not_block_the_next);
    RUN_TEST(test_queue_limits);
    RUN_TEST(test_duplicates_are_recognised);
    RUN_TEST(test_hello_resets_the_duplicate_filter);
    RUN_TEST(test_liveness);
    RUN_TEST(test_heartbeat_cadence);
    RUN_TEST(test_clock_wrap);
    RUN_TEST(test_event_queue_keeps_the_newest);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
