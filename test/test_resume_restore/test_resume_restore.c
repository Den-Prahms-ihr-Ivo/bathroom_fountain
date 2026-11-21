/*
 * Tests for lib/resume_restore – requirements BED-45, BED-46.
 * Uses the real lib/playlist and lib/resume_point.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "resume_restore.h"

#define SEED 0x5EED1234u
#define TRACKS 7

static playlist_t *pl;

void setUp(void)
{
    pl = NULL;
}

void tearDown(void)
{
    playlist_destroy(pl);
    pl = NULL;
}

static playlist_t *build(uint32_t seed, int tracks)
{
    playlist_config_t cfg;
    playlist_config_default(&cfg);
    cfg.shuffle = true;
    cfg.seed = seed;
    playlist_t *p = playlist_create(&cfg);
    TEST_ASSERT_NOT_NULL(p);
    char name[32];
    for (int i = 0; i < tracks; i++) {
        snprintf(name, sizeof(name), "ep%02d.mp3", i + 1);
        TEST_ASSERT_EQUAL_INT(PLAYLIST_ADDED, playlist_add(p, name, false));
    }
    playlist_finalize(p);
    return p;
}

/* The name the index-th playlist_next() gives for this seed. */
static void name_at(uint32_t seed, uint32_t index, char *out)
{
    playlist_t *p = build(seed, TRACKS);
    const char *n = NULL;
    for (uint32_t i = 0; i < index; i++) {
        n = playlist_next(p);
    }
    TEST_ASSERT_NOT_NULL(n);
    strcpy(out, n);
    playlist_destroy(p);
}

static resume_point_t point(uint32_t index, uint32_t offset)
{
    resume_point_t rp;
    char track[64];
    name_at(SEED, index, track);
    TEST_ASSERT_TRUE(resume_capture(&rp, "sleep", SEED, index, track, offset));
    return rp;
}

/* ---- capture ----------------------------------------------------------- */

static void test_capture_fills_point(void)
{
    resume_point_t rp;
    memset(&rp, 0xEE, sizeof(rp));
    TEST_ASSERT_TRUE(resume_capture(&rp, "sleep", 77, 3, "ep02.mp3", 4096));
    TEST_ASSERT_EQUAL_STRING("sleep", rp.folder);
    TEST_ASSERT_EQUAL_STRING("ep02.mp3", rp.track);
    TEST_ASSERT_EQUAL_UINT32(77, rp.seed);
    TEST_ASSERT_EQUAL_UINT32(3, rp.index);
    TEST_ASSERT_EQUAL_UINT32(4096, rp.offset);
    uint8_t blob[RESUME_BLOB_MAX];
    TEST_ASSERT_TRUE_MESSAGE(resume_encode(&rp, blob, sizeof(blob)) > 0, "captured point is encodable");
}

static void test_capture_refuses_invalid(void)
{
    resume_point_t rp;
    memset(&rp, 0xEE, sizeof(rp));
    const resume_point_t before = rp;
    char long_track[RESUME_TRACK_MAX + 2];
    memset(long_track, 't', sizeof(long_track) - 1);
    long_track[sizeof(long_track) - 1] = '\0';
    TEST_ASSERT_FALSE(resume_capture(&rp, "sleep", 1, 0, "a.mp3", 0));
    TEST_ASSERT_FALSE(resume_capture(&rp, "", 1, 1, "a.mp3", 0));
    TEST_ASSERT_FALSE(resume_capture(&rp, "sleep", 1, 1, "", 0));
    TEST_ASSERT_FALSE(resume_capture(&rp, "sleep", 1, 1, long_track, 0));
    TEST_ASSERT_FALSE(resume_capture(&rp, "abcdefghijklmnopqrstuvwx", 1, 1, "a.mp3", 0));
    TEST_ASSERT_FALSE(resume_capture(&rp, "sleep", 1, 1, "sub/a.mp3", 0));
    TEST_ASSERT_FALSE(resume_capture(&rp, NULL, 1, 1, "a.mp3", 0));
    TEST_ASSERT_FALSE(resume_capture(&rp, "sleep", 1, 1, NULL, 0));
    TEST_ASSERT_FALSE(resume_capture(NULL, "sleep", 1, 1, "a.mp3", 0));
    TEST_ASSERT_EQUAL_MEMORY(&before, &rp, sizeof(rp));
    /* the longest valid track name works */
    long_track[RESUME_TRACK_MAX] = '\0';
    TEST_ASSERT_TRUE(resume_capture(&rp, "sleep", 1, 1, long_track, 0));
}

/* ---- restore ----------------------------------------------------------- */

static void test_restore_finds_track_and_offset(void)
{
    const resume_point_t rp = point(3, 123456);
    pl = build(SEED, TRACKS);
    uint32_t offset = 0;
    const char *t = resume_restore(pl, &rp, "sleep", &offset);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING(rp.track, t);
    TEST_ASSERT_EQUAL_UINT32(123456, offset);
}

static void test_restore_continues_the_same_order(void)
{
    /* after resuming at index 2, the next tracks are those of the original order */
    char want[TRACKS + 3][32];
    playlist_t *orig = build(SEED, TRACKS);
    for (int i = 0; i < TRACKS + 3; i++) {
        strcpy(want[i], playlist_next(orig));
    }
    playlist_destroy(orig);

    resume_point_t rp;
    TEST_ASSERT_TRUE(resume_capture(&rp, "sleep", SEED, 2, want[1], 10));
    pl = build(SEED, TRACKS);
    uint32_t offset = 0;
    TEST_ASSERT_NOT_NULL(resume_restore(pl, &rp, "sleep", &offset));
    for (int i = 2; i < TRACKS + 3; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want[i], playlist_next(pl), "order continues, also into the next round");
    }
}

static void test_restore_in_a_later_round(void)
{
    const resume_point_t rp = point(TRACKS + 2, 99); /* second round */
    pl = build(SEED, TRACKS);
    uint32_t offset = 0;
    TEST_ASSERT_NOT_NULL(resume_restore(pl, &rp, "sleep", &offset));
    TEST_ASSERT_EQUAL_UINT32(99, offset);
}

static void test_restore_fails_when_card_changed(void)
{
    const resume_point_t rp = point(3, 500);
    pl = build(SEED, TRACKS + 1); /* a file was added: other order */
    uint32_t offset = 7;
    const char *t = resume_restore(pl, &rp, "sleep", &offset);
    if (t != NULL) {
        /* the same name may come out by chance; then it must be the right one */
        TEST_ASSERT_EQUAL_STRING(rp.track, t);
    } else {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(7, offset, "offset untouched on failure");
    }
}

static void test_restore_fails_on_name_mismatch(void)
{
    resume_point_t rp = point(3, 500);
    strcpy(rp.track, "gone.mp3");
    pl = build(SEED, TRACKS);
    uint32_t offset = 7;
    TEST_ASSERT_NULL(resume_restore(pl, &rp, "sleep", &offset));
    TEST_ASSERT_EQUAL_UINT32(7, offset);
}

static void test_restore_fails_with_other_seed(void)
{
    const resume_point_t rp = point(4, 500);
    /* find a seed whose 4th track differs */
    uint32_t other = SEED + 1;
    char n[32];
    for (;; other++) {
        name_at(other, 4, n);
        if (strcmp(n, rp.track) != 0) {
            break;
        }
    }
    pl = build(other, TRACKS);
    uint32_t offset = 7;
    TEST_ASSERT_NULL(resume_restore(pl, &rp, "sleep", &offset));
}

static void test_refusals_do_not_touch_playlist(void)
{
    resume_point_t rp = point(3, 500);
    pl = build(SEED, TRACKS);
    uint32_t offset = 7;
    TEST_ASSERT_NULL_MESSAGE(resume_restore(pl, &rp, "mood_rain", &offset), "other folder");
    TEST_ASSERT_NULL_MESSAGE(resume_restore(pl, &rp, "Sleep", &offset), "folder match is exact");
    TEST_ASSERT_NULL(resume_restore(NULL, &rp, "sleep", &offset));
    TEST_ASSERT_NULL(resume_restore(pl, NULL, "sleep", &offset));
    TEST_ASSERT_NULL(resume_restore(pl, &rp, NULL, &offset));
    TEST_ASSERT_NULL(resume_restore(pl, &rp, "sleep", NULL));
    resume_point_t bad = rp;
    bad.index = 0;
    TEST_ASSERT_NULL(resume_restore(pl, &bad, "sleep", &offset));
    bad.index = RESUME_REPLAY_MAX + 1;
    TEST_ASSERT_NULL(resume_restore(pl, &bad, "sleep", &offset));
    TEST_ASSERT_EQUAL_UINT32(7, offset);
    /* the playlist is still at its start: a valid restore works now */
    TEST_ASSERT_NOT_NULL(resume_restore(pl, &rp, "sleep", &offset));
    TEST_ASSERT_EQUAL_UINT32(500, offset);
}

static void test_empty_playlist(void)
{
    resume_point_t rp;
    TEST_ASSERT_TRUE(resume_capture(&rp, "sleep", SEED, 1, "ep01.mp3", 0));
    pl = build(SEED, 0);
    uint32_t offset = 7;
    TEST_ASSERT_NULL(resume_restore(pl, &rp, "sleep", &offset));
    TEST_ASSERT_EQUAL_UINT32(7, offset);
}

static void test_replay_limit_itself_is_allowed(void)
{
    resume_point_t rp;
    char track[32];
    name_at(SEED, RESUME_REPLAY_MAX, track);
    TEST_ASSERT_TRUE(resume_capture(&rp, "sleep", SEED, RESUME_REPLAY_MAX, track, 1));
    pl = build(SEED, TRACKS);
    uint32_t offset = 0;
    TEST_ASSERT_NOT_NULL(resume_restore(pl, &rp, "sleep", &offset));
    TEST_ASSERT_EQUAL_UINT32(1, offset);
}

static void test_resume_key(void)
{
    char k[16];
    TEST_ASSERT_TRUE(resume_key("sleep_rain", k, sizeof(k)));
    TEST_ASSERT_EQUAL_STRING("rp931cbb23", k);
    TEST_ASSERT_TRUE(resume_key("sleep_true_crime", k, sizeof(k)));
    TEST_ASSERT_EQUAL_STRING("rpf88b15f8", k);
    TEST_ASSERT_TRUE(resume_key("a", k, 11));
    TEST_ASSERT_EQUAL_STRING("rpe40c292c", k);
    strcpy(k, "x");
    TEST_ASSERT_FALSE(resume_key("a", k, 10));
    TEST_ASSERT_EQUAL_STRING("", k);
    TEST_ASSERT_FALSE(resume_key("", k, sizeof(k)));
    TEST_ASSERT_FALSE(resume_key(NULL, k, sizeof(k)));
    TEST_ASSERT_FALSE(resume_key("a", NULL, sizeof(k)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_capture_fills_point);
    RUN_TEST(test_capture_refuses_invalid);
    RUN_TEST(test_restore_finds_track_and_offset);
    RUN_TEST(test_restore_continues_the_same_order);
    RUN_TEST(test_restore_in_a_later_round);
    RUN_TEST(test_restore_fails_when_card_changed);
    RUN_TEST(test_restore_fails_on_name_mismatch);
    RUN_TEST(test_restore_fails_with_other_seed);
    RUN_TEST(test_refusals_do_not_touch_playlist);
    RUN_TEST(test_empty_playlist);
    RUN_TEST(test_replay_limit_itself_is_allowed);
    RUN_TEST(test_resume_key);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
