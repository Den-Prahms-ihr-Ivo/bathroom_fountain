/*
 * Tests for lib/folder_list – requirements BED-43, BED-50.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "folder_list.h"

static folder_list_t fl;

void setUp(void)
{
    memset(&fl, 0xA5, sizeof(fl)); /* init must not rely on zeroed memory */
    folder_list_init(&fl);
}

void tearDown(void) {}

static bool feed(uint8_t index, uint8_t count, const char *name)
{
    link_folder_t f;
    memset(&f, 0, sizeof(f));
    f.index = index;
    f.count = count;
    strncpy(f.name, name, LINK_FOLDER_LEN);
    return folder_list_feed(&fl, &f);
}

static void expect_list(const char *const *names, size_t n)
{
    TEST_ASSERT_TRUE_MESSAGE(folder_list_ready(&fl), "list ready");
    TEST_ASSERT_EQUAL_size_t(n, folder_list_count(&fl));
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_STRING(names[i], folder_list_name(&fl, i));
    }
    TEST_ASSERT_NULL(folder_list_name(&fl, n));
}

static void test_starts_empty_and_not_ready(void)
{
    TEST_ASSERT_FALSE(folder_list_ready(&fl));
    TEST_ASSERT_EQUAL_size_t(0, folder_list_count(&fl));
    TEST_ASSERT_NULL(folder_list_name(&fl, 0));
    TEST_ASSERT_FALSE(folder_list_has(&fl, "sleep"));
}

static void test_list_in_order(void)
{
    TEST_ASSERT_TRUE(feed(0, 3, "sleep"));
    TEST_ASSERT_TRUE(feed(1, 3, "mood_rain"));
    TEST_ASSERT_FALSE_MESSAGE(folder_list_ready(&fl), "not complete yet");
    TEST_ASSERT_EQUAL_size_t(0, folder_list_count(&fl));
    TEST_ASSERT_TRUE(feed(2, 3, "meditation"));
    const char *want[] = {"sleep", "mood_rain", "meditation"};
    expect_list(want, 3);
    TEST_ASSERT_TRUE(folder_list_has(&fl, "mood_rain"));
    TEST_ASSERT_FALSE_MESSAGE(folder_list_has(&fl, "Mood_rain"), "case-sensitive");
    TEST_ASSERT_FALSE(folder_list_has(&fl, "mood"));
}

static void test_out_of_order_and_duplicates(void)
{
    TEST_ASSERT_TRUE(feed(0, 3, "a"));
    TEST_ASSERT_TRUE(feed(2, 3, "c"));
    TEST_ASSERT_TRUE(feed(2, 3, "c")); /* retry */
    TEST_ASSERT_TRUE_MESSAGE(feed(0, 3, "a"), "retried index 0 does not restart the list");
    TEST_ASSERT_FALSE(folder_list_ready(&fl));
    TEST_ASSERT_TRUE(feed(1, 3, "b"));
    const char *want[] = {"a", "b", "c"};
    expect_list(want, 3);
}

static void test_old_list_stays_until_new_one_complete(void)
{
    feed(0, 2, "one");
    feed(1, 2, "two");
    TEST_ASSERT_TRUE(feed(0, 3, "x"));
    TEST_ASSERT_TRUE(feed(1, 3, "y"));
    const char *old[] = {"one", "two"};
    expect_list(old, 2);
    TEST_ASSERT_TRUE(feed(2, 3, "z"));
    const char *now[] = {"x", "y", "z"};
    expect_list(now, 3);
}

static void test_new_count_restarts_reception(void)
{
    feed(0, 3, "a");
    feed(1, 3, "b");
    /* the speaker rescanned: two folders now */
    TEST_ASSERT_TRUE(feed(0, 2, "p"));
    TEST_ASSERT_FALSE_MESSAGE(feed(2, 3, "c"), "frame of the dropped list");
    TEST_ASSERT_FALSE(folder_list_ready(&fl));
    TEST_ASSERT_TRUE(feed(1, 2, "q"));
    const char *want[] = {"p", "q"};
    expect_list(want, 2);
}

static void test_empty_card(void)
{
    feed(0, 2, "one");
    feed(1, 2, "two");
    feed(0, 3, "x"); /* a reception in progress is dropped too */
    TEST_ASSERT_TRUE(feed(0, 0, ""));
    TEST_ASSERT_TRUE(folder_list_ready(&fl));
    TEST_ASSERT_EQUAL_size_t(0, folder_list_count(&fl));
    TEST_ASSERT_FALSE(folder_list_has(&fl, "one"));
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 3, "y"), "no list being received any more");
}

static void test_refused_frames(void)
{
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 3, "b"), "no index 0 yet");
    TEST_ASSERT_FALSE_MESSAGE(feed(3, 3, "d"), "index >= count");
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 0, ""), "count 0 with index 1");
    feed(0, 3, "a");
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 4, "b"), "count differs");
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 3, ""), "empty name");
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 3, "true crime"), "invalid name");
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 3, "a/b"), "invalid name");
    TEST_ASSERT_FALSE(folder_list_feed(&fl, NULL));
    TEST_ASSERT_FALSE(folder_list_feed(NULL, NULL));
    /* nothing of that changed the reception */
    TEST_ASSERT_TRUE(feed(1, 3, "b"));
    TEST_ASSERT_TRUE(feed(2, 3, "c"));
    const char *want[] = {"a", "b", "c"};
    expect_list(want, 3);
}

static void test_refused_index_zero_keeps_reception(void)
{
    feed(0, 2, "a");
    TEST_ASSERT_FALSE(feed(0, 5, "bad name!"));
    TEST_ASSERT_TRUE_MESSAGE(feed(1, 2, "b"), "the old reception is still running");
    const char *want[] = {"a", "b"};
    expect_list(want, 2);
}

static void test_more_than_max_folders(void)
{
    char name[16];
    const uint8_t count = FOLDER_LIST_MAX + 5;
    for (uint8_t i = 0; i < FOLDER_LIST_MAX; i++) {
        snprintf(name, sizeof(name), "f%02u", (unsigned)i);
        TEST_ASSERT_TRUE(feed(i, count, name));
    }
    /* complete without the frames beyond the maximum */
    TEST_ASSERT_TRUE(folder_list_ready(&fl));
    TEST_ASSERT_EQUAL_size_t(FOLDER_LIST_MAX, folder_list_count(&fl));
    TEST_ASSERT_EQUAL_STRING("f31", folder_list_name(&fl, FOLDER_LIST_MAX - 1));
    TEST_ASSERT_TRUE_MESSAGE(feed(FOLDER_LIST_MAX + 2, count, "extra"), "accepted and ignored");
    TEST_ASSERT_FALSE(folder_list_has(&fl, "extra"));
    TEST_ASSERT_EQUAL_size_t(FOLDER_LIST_MAX, folder_list_count(&fl));
}

static void test_single_folder_and_long_name(void)
{
    TEST_ASSERT_TRUE(feed(0, 1, "abcdefghijklmnopqrstuvw")); /* 23 characters */
    const char *want[] = {"abcdefghijklmnopqrstuvw"};
    expect_list(want, 1);
}

static void test_late_duplicate_after_completion(void)
{
    feed(0, 2, "a");
    feed(1, 2, "b");
    /* a retried frame of the finished list arrives late */
    TEST_ASSERT_FALSE_MESSAGE(feed(1, 2, "b"), "no list being received");
    TEST_ASSERT_TRUE(feed(0, 2, "a")); /* starts a reception that never completes */
    const char *want[] = {"a", "b"};
    expect_list(want, 2);
}

static void test_null_reads(void)
{
    folder_list_init(NULL);
    TEST_ASSERT_FALSE(folder_list_ready(NULL));
    TEST_ASSERT_EQUAL_size_t(0, folder_list_count(NULL));
    TEST_ASSERT_NULL(folder_list_name(NULL, 0));
    TEST_ASSERT_FALSE(folder_list_has(NULL, "a"));
    feed(0, 1, "a");
    TEST_ASSERT_FALSE(folder_list_has(&fl, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty_and_not_ready);
    RUN_TEST(test_list_in_order);
    RUN_TEST(test_out_of_order_and_duplicates);
    RUN_TEST(test_old_list_stays_until_new_one_complete);
    RUN_TEST(test_new_count_restarts_reception);
    RUN_TEST(test_empty_card);
    RUN_TEST(test_refused_frames);
    RUN_TEST(test_refused_index_zero_keeps_reception);
    RUN_TEST(test_more_than_max_folders);
    RUN_TEST(test_single_folder_and_long_name);
    RUN_TEST(test_late_duplicate_after_completion);
    RUN_TEST(test_null_reads);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
