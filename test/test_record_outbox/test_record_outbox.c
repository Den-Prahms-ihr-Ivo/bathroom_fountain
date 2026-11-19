/*
 * Tests for lib/record_outbox – requirements COLD-18, COLD-19.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "record_outbox.h"

static record_outbox_config_t cfg;
static record_outbox_t *ob;
static uint32_t now;
static char buf[600];

void setUp(void)
{
    record_outbox_config_default(&cfg);
    cfg.capacity = 4;
    cfg.item_max = 64;
    cfg.retry_ms = 60000;
    ob = NULL;
    now = 900000u;
    memset(buf, 0, sizeof(buf));
}

void tearDown(void)
{
    record_outbox_destroy(ob);
    ob = NULL;
}

static void create(void)
{
    ob = record_outbox_create(&cfg);
    TEST_ASSERT_NOT_NULL_MESSAGE(ob, "record_outbox_create returned NULL for a valid config");
}

static void push(uint32_t id, const char *text)
{
    TEST_ASSERT_TRUE_MESSAGE(record_outbox_push(ob, id, text, strlen(text), NULL), text);
}

/* id of the record handed out now, 0 if none; its text ends up in buf */
static uint32_t take(void)
{
    uint32_t id = 0;
    memset(buf, 0, sizeof(buf));
    const size_t n = record_outbox_take_due(ob, now, &id, buf, sizeof(buf) - 1);
    if (n == 0) {
        return 0;
    }
    buf[n] = '\0';
    return id;
}

static void test_defaults_and_validation(void)
{
    record_outbox_config_t d;
    memset(&d, 0, sizeof(d));
    record_outbox_config_default(&d);
    TEST_ASSERT_EQUAL_UINT8(8, d.capacity);
    TEST_ASSERT_EQUAL_UINT16(256, d.item_max);
    TEST_ASSERT_EQUAL_UINT32(60000, d.retry_ms);
    record_outbox_config_t b = d;
    b.capacity = 0;
    TEST_ASSERT_NULL(record_outbox_create(&b));
    b.capacity = 17;
    TEST_ASSERT_NULL(record_outbox_create(&b));
    b = d;
    b.item_max = 15;
    TEST_ASSERT_NULL(record_outbox_create(&b));
    b.item_max = 513;
    TEST_ASSERT_NULL(record_outbox_create(&b));
    b = d;
    b.retry_ms = 999;
    TEST_ASSERT_NULL(record_outbox_create(&b));
    b.retry_ms = 3600001;
    TEST_ASSERT_NULL(record_outbox_create(&b));
    TEST_ASSERT_NULL(record_outbox_create(NULL));
    create();
    TEST_ASSERT_EQUAL_size_t(0, record_outbox_count(ob));
    record_outbox_destroy(NULL);
}

static void test_nothing_goes_out_while_offline(void)
{
    create();
    push(11, "{\"id\":11}");
    TEST_ASSERT_EQUAL_size_t(1, record_outbox_count(ob));
    TEST_ASSERT_EQUAL_UINT32(0, take());
    now += 3600000;
    TEST_ASSERT_EQUAL_UINT32(0, take());
    record_outbox_set_connected(ob, true, now);
    TEST_ASSERT_EQUAL_UINT32(11, take());
    TEST_ASSERT_EQUAL_STRING("{\"id\":11}", buf);
}

static void test_oldest_first_one_per_call_then_retry(void)
{
    create();
    record_outbox_set_connected(ob, true, now);
    push(5, "five");
    push(3, "three");
    push(9, "nine");
    TEST_ASSERT_EQUAL_UINT32(5, take());
    TEST_ASSERT_EQUAL_UINT32(3, take());
    TEST_ASSERT_EQUAL_UINT32(9, take());
    TEST_ASSERT_EQUAL_STRING("nine", buf);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, take(), "all sent, none due yet");
    now += 59999;
    TEST_ASSERT_EQUAL_UINT32(0, take());
    TEST_ASSERT_TRUE(record_outbox_ack(ob, 3));
    now += 1;
    TEST_ASSERT_EQUAL_UINT32(5, take());
    TEST_ASSERT_EQUAL_UINT32(9, take());
    TEST_ASSERT_EQUAL_UINT32(0, take());
    TEST_ASSERT_EQUAL_size_t(2, record_outbox_count(ob));
}

static void test_ack_removes(void)
{
    create();
    record_outbox_set_connected(ob, true, now);
    push(7, "seven");
    TEST_ASSERT_EQUAL_UINT32(7, take());
    TEST_ASSERT_TRUE(record_outbox_ack(ob, 7));
    TEST_ASSERT_EQUAL_size_t(0, record_outbox_count(ob));
    TEST_ASSERT_FALSE_MESSAGE(record_outbox_ack(ob, 7), "a repeated ack finds nothing");
    TEST_ASSERT_FALSE(record_outbox_ack(ob, 12345));
    now += 120000;
    TEST_ASSERT_EQUAL_UINT32(0, take());
    /* an ack may come before the record was even handed out (sent before a reboot) */
    push(8, "eight");
    TEST_ASSERT_TRUE(record_outbox_ack(ob, 8));
    TEST_ASSERT_EQUAL_UINT32(0, take());
}

static void test_reconnect_makes_everything_due(void)
{
    create();
    record_outbox_set_connected(ob, true, now);
    push(1, "a");
    push(2, "b");
    TEST_ASSERT_EQUAL_UINT32(1, take());
    TEST_ASSERT_EQUAL_UINT32(2, take());
    now += 1000;
    record_outbox_set_connected(ob, false, now);
    TEST_ASSERT_EQUAL_UINT32(0, take());
    now += 1000;
    record_outbox_set_connected(ob, true, now);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, take(), "due at once after the reconnect, not after retry_ms");
    TEST_ASSERT_EQUAL_UINT32(2, take());
    /* reporting "connected" again while connected changes nothing */
    record_outbox_set_connected(ob, true, now);
    TEST_ASSERT_EQUAL_UINT32(0, take());
}

static void test_full_outbox_drops_the_oldest(void)
{
    create();
    push(1, "one");
    push(2, "two");
    push(3, "three");
    push(4, "four");
    uint32_t dropped = 99;
    TEST_ASSERT_TRUE(record_outbox_push(ob, 5, "five", 4, &dropped));
    TEST_ASSERT_EQUAL_UINT32(1, dropped);
    TEST_ASSERT_EQUAL_size_t(4, record_outbox_count(ob));
    dropped = 99;
    TEST_ASSERT_TRUE(record_outbox_ack(ob, 3));
    TEST_ASSERT_TRUE(record_outbox_push(ob, 6, "six", 3, &dropped));
    TEST_ASSERT_EQUAL_UINT32(0, dropped);
    record_outbox_set_connected(ob, true, now);
    TEST_ASSERT_EQUAL_UINT32(2, take());
    TEST_ASSERT_EQUAL_UINT32(4, take());
    TEST_ASSERT_EQUAL_UINT32(5, take());
    TEST_ASSERT_EQUAL_UINT32(6, take());
    TEST_ASSERT_EQUAL_STRING("six", buf);
}

static void test_push_rules(void)
{
    create();
    char big[65];
    memset(big, 'x', sizeof(big));
    uint32_t dropped = 99;
    TEST_ASSERT_FALSE(record_outbox_push(ob, 0, "zero", 4, &dropped));
    TEST_ASSERT_EQUAL_UINT32(0, dropped);
    TEST_ASSERT_FALSE(record_outbox_push(ob, 1, NULL, 4, NULL));
    TEST_ASSERT_FALSE(record_outbox_push(ob, 1, "x", 0, NULL));
    TEST_ASSERT_FALSE(record_outbox_push(ob, 1, big, 65, NULL));
    TEST_ASSERT_TRUE(record_outbox_push(ob, 1, big, 64, NULL));
    TEST_ASSERT_FALSE_MESSAGE(record_outbox_push(ob, 1, "again", 5, NULL), "same id twice");
    TEST_ASSERT_EQUAL_size_t(1, record_outbox_count(ob));
    /* the data was copied */
    char text[8] = "abc";
    push(2, text);
    strcpy(text, "XYZ");
    record_outbox_set_connected(ob, true, now);
    take();
    TEST_ASSERT_EQUAL_UINT32(2, take());
    TEST_ASSERT_EQUAL_STRING("abc", buf);
}

static void test_out_too_small_keeps_it_due(void)
{
    create();
    record_outbox_set_connected(ob, true, now);
    push(1, "0123456789");
    char small[5];
    uint32_t id = 77;
    TEST_ASSERT_EQUAL_size_t(0, record_outbox_take_due(ob, now, &id, small, sizeof(small)));
    TEST_ASSERT_EQUAL_UINT32(1, take());
    TEST_ASSERT_EQUAL_STRING("0123456789", buf);
}

static void test_save_and_load(void)
{
    create();
    TEST_ASSERT_EQUAL_size_t(4, record_outbox_saved_size(ob));
    uint8_t blob[400];
    TEST_ASSERT_EQUAL_size_t(4, record_outbox_save(ob, blob, sizeof(blob)));
    const uint8_t empty[] = {'R', 'O', 1, 0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(empty, blob, 4);

    push(0x01020304u, "ab");
    push(7, "cde");
    TEST_ASSERT_EQUAL_size_t(4 + 8 + 9, record_outbox_saved_size(ob));
    const uint8_t expected[] = {'R', 'O', 1, 2, 0x04, 0x03, 0x02, 0x01, 2, 0, 'a', 'b', 7, 0, 0, 0, 3, 0, 'c', 'd', 'e'};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), record_outbox_save(ob, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, blob, sizeof(expected));
    TEST_ASSERT_EQUAL_size_t(0, record_outbox_save(ob, blob, sizeof(expected) - 1));

    /* a reboot: a new outbox gets the records back, not yet sent */
    record_outbox_set_connected(ob, true, now);
    take();
    record_outbox_destroy(ob);
    create();
    TEST_ASSERT_TRUE(record_outbox_load(ob, expected, sizeof(expected)));
    TEST_ASSERT_EQUAL_size_t(2, record_outbox_count(ob));
    record_outbox_set_connected(ob, true, now);
    TEST_ASSERT_EQUAL_UINT32(0x01020304u, take());
    TEST_ASSERT_EQUAL_STRING("ab", buf);
    TEST_ASSERT_EQUAL_UINT32(7, take());
    TEST_ASSERT_EQUAL_STRING("cde", buf);
}

static void test_load_refuses_broken_blobs(void)
{
    create();
    push(42, "keep");
    const uint8_t good[] = {'R', 'O', 1, 1, 9, 0, 0, 0, 2, 0, 'h', 'i'};
    uint8_t b[32];
#define REFUSED(len)                                                         \
    TEST_ASSERT_FALSE(record_outbox_load(ob, b, len));                      \
    TEST_ASSERT_EQUAL_size_t(1, record_outbox_count(ob))
    memcpy(b, good, sizeof(good));
    b[0] = 'X';
    REFUSED(sizeof(good));
    memcpy(b, good, sizeof(good));
    b[2] = 2; /* other version */
    REFUSED(sizeof(good));
    memcpy(b, good, sizeof(good));
    b[3] = 5; /* more than capacity */
    REFUSED(sizeof(good));
    memcpy(b, good, sizeof(good));
    REFUSED(sizeof(good) - 1); /* truncated */
    memcpy(b, good, sizeof(good));
    b[sizeof(good)] = 0;
    REFUSED(sizeof(good) + 1); /* trailing byte */
    memcpy(b, good, sizeof(good));
    b[8] = 65; /* longer than item_max */
    REFUSED(sizeof(good));
    memcpy(b, good, sizeof(good));
    b[4] = 0; /* id 0 */
    REFUSED(sizeof(good));
    const uint8_t dup[] = {'R', 'O', 1, 2, 9, 0, 0, 0, 1, 0, 'a', 9, 0, 0, 0, 1, 0, 'b'};
    memcpy(b, dup, sizeof(dup));
    REFUSED(sizeof(dup)); /* the same id twice */
    TEST_ASSERT_FALSE(record_outbox_load(ob, NULL, 4));
    REFUSED(3);
#undef REFUSED
    record_outbox_set_connected(ob, true, now);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(42, take(), "the old content is still there");
    TEST_ASSERT_TRUE(record_outbox_load(ob, good, sizeof(good)));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, record_outbox_count(ob), "load replaces the content");
}

static void test_clock_wrap(void)
{
    now = 0xFFFFFFFFu - 1000u;
    create();
    record_outbox_set_connected(ob, true, now);
    push(1, "a");
    TEST_ASSERT_EQUAL_UINT32(1, take());
    now += 59999;
    TEST_ASSERT_EQUAL_UINT32(0, take());
    now += 1;
    TEST_ASSERT_EQUAL_UINT32(1, take());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_nothing_goes_out_while_offline);
    RUN_TEST(test_oldest_first_one_per_call_then_retry);
    RUN_TEST(test_ack_removes);
    RUN_TEST(test_reconnect_makes_everything_due);
    RUN_TEST(test_full_outbox_drops_the_oldest);
    RUN_TEST(test_push_rules);
    RUN_TEST(test_out_too_small_keeps_it_due);
    RUN_TEST(test_save_and_load);
    RUN_TEST(test_load_refuses_broken_blobs);
    RUN_TEST(test_clock_wrap);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
