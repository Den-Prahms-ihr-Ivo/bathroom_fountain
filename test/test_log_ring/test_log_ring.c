/*
 * Tests for lib/log_ring – requirements LOG-01 ... LOG-04.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "log_ring.h"

static log_ring_t *lr;
static char out[256];

void setUp(void)
{
    lr = NULL;
    memset(out, 'Z', sizeof(out));
}

void tearDown(void)
{
    log_ring_destroy(lr);
    lr = NULL;
}

static void create(size_t capacity)
{
    lr = log_ring_create(capacity);
    TEST_ASSERT_NOT_NULL_MESSAGE(lr, "log_ring_create returned NULL for a valid capacity");
}

static uint32_t add(const char *s)
{
    return log_ring_append(lr, s, strlen(s));
}

static void expect_line(uint32_t seq, const char *text)
{
    memset(out, 'Z', sizeof(out));
    TEST_ASSERT_EQUAL_INT((int)strlen(text), log_ring_read(lr, seq, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(text, out);
}

static void test_create_limits(void)
{
    create(256);
    log_ring_destroy(lr);
    create(65536);
    TEST_ASSERT_NULL(log_ring_create(255));
    TEST_ASSERT_NULL(log_ring_create(65537));
    TEST_ASSERT_NULL(log_ring_create(0));
    log_ring_destroy(NULL);
}

static void test_empty_ring(void)
{
    create(1024);
    TEST_ASSERT_EQUAL_UINT32(0, log_ring_first_seq(lr));
    TEST_ASSERT_EQUAL_UINT32(0, log_ring_last_seq(lr));
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, 1, out, sizeof(out)));
}

static void test_append_and_read_back(void)
{
    create(1024);
    TEST_ASSERT_EQUAL_UINT32(1, add("I (100) boot: hello"));
    TEST_ASSERT_EQUAL_UINT32(2, add("W (200) wifi: retry"));
    TEST_ASSERT_EQUAL_UINT32(3, log_ring_append(lr, "E (300) hue: timeout and more", 20)); /* len counts, no NUL needed */
    TEST_ASSERT_EQUAL_UINT32(1, log_ring_first_seq(lr));
    TEST_ASSERT_EQUAL_UINT32(3, log_ring_last_seq(lr));
    expect_line(1, "I (100) boot: hello");
    expect_line(2, "W (200) wifi: retry");
    expect_line(3, "E (300) hue: timeout");
    expect_line(1, "I (100) boot: hello"); /* reading does not consume */
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, 4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, 0, out, sizeof(out)));
}

static void test_lines_are_cleaned(void)
{
    create(1024);
    add("\033[0;32mI (100) main: green\033[0m\n");
    expect_line(1, "I (100) main: green");
    add("W (1) a: crlf\r\n");
    expect_line(2, "W (1) a: crlf");
    add("I (2) a: tab\there\x01!");
    expect_line(3, "I (2) a: tab here !");
    add("\033[1;31mE (3) x: bold\033[0m and \033[0;33mmore\033[0m");
    expect_line(4, "E (3) x: bold and more");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, add("\033[0m\r\n"), "nothing left: not stored");
    TEST_ASSERT_EQUAL_UINT32(0, add(""));
    TEST_ASSERT_EQUAL_UINT32(0, log_ring_append(lr, NULL, 5));
    TEST_ASSERT_EQUAL_UINT32(5, add("next"));
    TEST_ASSERT_EQUAL_UINT32(5, log_ring_last_seq(lr));
}

static void test_long_line_is_cut(void)
{
    char big[400];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (char)('a' + i % 26);
    }
    create(1024);
    TEST_ASSERT_EQUAL_UINT32(1, log_ring_append(lr, big, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(LOG_RING_LINE_MAX, log_ring_read(lr, 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_size_t(LOG_RING_LINE_MAX, strlen(out));
    TEST_ASSERT_EQUAL_MEMORY(big, out, LOG_RING_LINE_MAX);
}

static void test_read_into_a_small_buffer(void)
{
    create(1024);
    add("I (100) boot: hello");
    char small[8];
    memset(small, 'Z', sizeof(small));
    TEST_ASSERT_EQUAL_INT_MESSAGE(19, log_ring_read(lr, 1, small, sizeof(small)), "returns the full length");
    TEST_ASSERT_EQUAL_STRING("I (100)", small);
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, 1, small, 0));
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, 1, NULL, 8));
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(NULL, 1, small, sizeof(small)));
}

static void test_oldest_lines_are_dropped(void)
{
    create(256);
    /* each line: 30 bytes + 2 overhead = 32; exactly 8 fit into 256 */
    for (int i = 1; i <= 8; i++) {
        char line[64];
        snprintf(line, sizeof(line), "I (%04u) test: line number %02u.", (unsigned)i % 10000u, (unsigned)i % 100u);
        TEST_ASSERT_EQUAL_size_t(30, strlen(line));
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, add(line));
    }
    TEST_ASSERT_EQUAL_UINT32(1, log_ring_first_seq(lr));
    TEST_ASSERT_EQUAL_UINT32(9, add("I (0009) test: line number 09."));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, log_ring_first_seq(lr), "one line had to go");
    TEST_ASSERT_EQUAL_UINT32(9, log_ring_last_seq(lr));
    TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, 1, out, sizeof(out)));
    expect_line(2, "I (0002) test: line number 02.");
    expect_line(9, "I (0009) test: line number 09.");

    /* a long line pushes out several short ones: 200 + 2 bytes need 7 slots of 32 */
    char big[LOG_RING_LINE_MAX + 1];
    memset(big, 'x', LOG_RING_LINE_MAX);
    big[LOG_RING_LINE_MAX] = '\0';
    TEST_ASSERT_EQUAL_UINT32(10, add(big));
    TEST_ASSERT_EQUAL_UINT32(9, log_ring_first_seq(lr));
    expect_line(9, "I (0009) test: line number 09.");
    expect_line(10, big);
}

static void test_long_run_keeps_the_newest_lines_intact(void)
{
    create(700);
    char line[64];
    for (uint32_t i = 1; i <= 5000; i++) {
        const int n = snprintf(line, sizeof(line), "I (%lu) run: %.*s", (unsigned long)i, (int)(i % 37), "abcdefghijklmnopqrstuvwxyz0123456789+");
        TEST_ASSERT_EQUAL_UINT32(i, log_ring_append(lr, line, (size_t)n));
        TEST_ASSERT_EQUAL_UINT32(i, log_ring_last_seq(lr));
        const uint32_t first = log_ring_first_seq(lr);
        TEST_ASSERT_TRUE(first >= 1 && first <= i);
        if (i > 20) {
            TEST_ASSERT_TRUE_MESSAGE(i - first >= 9, "700 bytes hold at least ten of these lines");
        }
        /* oldest and newest are readable and correct */
        char expect[64];
        snprintf(expect, sizeof(expect), "I (%lu) run: %.*s", (unsigned long)first, (int)(first % 37), "abcdefghijklmnopqrstuvwxyz0123456789+");
        expect_line(first, expect);
        expect_line(i, line);
        if (first > 1) {
            TEST_ASSERT_EQUAL_INT(-1, log_ring_read(lr, first - 1, out, sizeof(out)));
        }
    }
}

static void test_level_of_a_line(void)
{
    TEST_ASSERT_EQUAL_CHAR('I', log_line_level("I (1234) wifi: connected"));
    TEST_ASSERT_EQUAL_CHAR('E', log_line_level("E (1) x: y"));
    TEST_ASSERT_EQUAL_CHAR('W', log_line_level("W (1) x: y"));
    TEST_ASSERT_EQUAL_CHAR('D', log_line_level("D (1) x: y"));
    TEST_ASSERT_EQUAL_CHAR('V', log_line_level("V (1) x: y"));
    TEST_ASSERT_EQUAL_CHAR('W', log_line_level("\033[0;33mW (77) hue: slow\033[0m"));
    TEST_ASSERT_EQUAL_CHAR(0, log_line_level("ets Jul 29 2019 12:21:46"));
    TEST_ASSERT_EQUAL_CHAR(0, log_line_level("Info (1) x"));
    TEST_ASSERT_EQUAL_CHAR(0, log_line_level("I(1) x"));
    TEST_ASSERT_EQUAL_CHAR(0, log_line_level("X (1) x"));
    TEST_ASSERT_EQUAL_CHAR(0, log_line_level(""));
    TEST_ASSERT_EQUAL_CHAR(0, log_line_level(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_limits);
    RUN_TEST(test_empty_ring);
    RUN_TEST(test_append_and_read_back);
    RUN_TEST(test_lines_are_cleaned);
    RUN_TEST(test_long_line_is_cut);
    RUN_TEST(test_read_into_a_small_buffer);
    RUN_TEST(test_oldest_lines_are_dropped);
    RUN_TEST(test_long_run_keeps_the_newest_lines_intact);
    RUN_TEST(test_level_of_a_line);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
