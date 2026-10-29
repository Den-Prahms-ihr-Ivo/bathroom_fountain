/*
 * Tests for lib/playlist – requirements AUD-02, AUD-03, AUD-09, AUD-12.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "playlist.h"

static playlist_config_t cfg;
static playlist_t *pl;

void setUp(void)
{
    playlist_config_default(&cfg);
    cfg.max_tracks = 256;
    cfg.max_name_len = 96;
    cfg.shuffle = false;
    cfg.seed = 1;
    pl = NULL;
}

void tearDown(void)
{
    playlist_destroy(pl);
    pl = NULL;
}

static void create(void)
{
    pl = playlist_create(&cfg);
    TEST_ASSERT_NOT_NULL_MESSAGE(pl, "playlist_create returned NULL for a valid config");
}

static void add_ok(playlist_t *p, const char *name)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(PLAYLIST_ADDED, playlist_add(p, name, false), name);
}

static void add_numbered(playlist_t *p, int count)
{
    char name[32];
    for (int i = 0; i < count; i++) {
        name[0] = (char)('a' + i % 26);
        name[1] = (char)('a' + i / 26);
        strcpy(name + 2, ".mp3");
        add_ok(p, name);
    }
}

static void expect_next(const char *expected)
{
    const char *got = playlist_next(pl);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, "playlist_next returned NULL");
    TEST_ASSERT_EQUAL_STRING(expected, got);
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

static void test_defaults(void)
{
    playlist_config_t d;
    memset(&d, 0xA5, sizeof(d));
    playlist_config_default(&d);
    TEST_ASSERT_EQUAL_UINT16(256, d.max_tracks);
    TEST_ASSERT_EQUAL_UINT16(96, d.max_name_len);
    TEST_ASSERT_FALSE(d.shuffle);
    TEST_ASSERT_EQUAL_UINT32(1, d.seed);
}

static void test_create_rejects_invalid_config(void)
{
    create();
    TEST_ASSERT_NULL(playlist_create(NULL));
    playlist_config_t bad = cfg;
    bad.max_tracks = 0;
    TEST_ASSERT_NULL_MESSAGE(playlist_create(&bad), "max_tracks 0");
    bad.max_tracks = 1025;
    TEST_ASSERT_NULL_MESSAGE(playlist_create(&bad), "max_tracks 1025");
    bad = cfg;
    bad.max_name_len = 7;
    TEST_ASSERT_NULL_MESSAGE(playlist_create(&bad), "max_name_len 7");
    bad.max_name_len = 257;
    TEST_ASSERT_NULL_MESSAGE(playlist_create(&bad), "max_name_len 257");
}

/* ------------------------------------------------------------------------ */
/* Filtering                                                                */
/* ------------------------------------------------------------------------ */

static void test_only_visible_mp3_files_are_accepted(void)
{
    create();
    TEST_ASSERT_EQUAL_INT(PLAYLIST_ADDED, playlist_add(pl, "song.mp3", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_ADDED, playlist_add(pl, "Song.MP3", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_ADDED, playlist_add(pl, "song.Mp3", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_ADDED, playlist_add(pl, "x.mp3", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "notes.txt", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "song.mp3.txt", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "song.mp4", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "mp3", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "song", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_HIDDEN, playlist_add(pl, ".mp3", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_HIDDEN, playlist_add(pl, "._song.mp3", false));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_DIR, playlist_add(pl, ".Trashes", true));
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_DIR, playlist_add(pl, "music.mp3", true));
    playlist_finalize(pl);
    TEST_ASSERT_EQUAL_size_t(4, playlist_count(pl));
}

static void test_too_long_names_are_skipped(void)
{
    cfg.max_name_len = 16;
    create();
    TEST_ASSERT_EQUAL_INT(PLAYLIST_ADDED, playlist_add(pl, "abcdefghijk.mp3", false));          /* 15 */
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_TOO_LONG, playlist_add(pl, "abcdefghijkl.mp3", false)); /* 16 */
    TEST_ASSERT_EQUAL_INT_MESSAGE(PLAYLIST_SKIPPED_HIDDEN, playlist_add(pl, "._abcdefghijkl.mp3", false),
                                  "hidden is checked before length");
    TEST_ASSERT_EQUAL_INT_MESSAGE(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "abcdefghijklmnop.txt", false),
                                  "extension is checked before length");
}

static void test_full_playlist_skips_further_tracks(void)
{
    cfg.max_tracks = 3;
    create();
    add_numbered(pl, 3);
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_FULL, playlist_add(pl, "zz.mp3", false));
    TEST_ASSERT_EQUAL_INT_MESSAGE(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "zz.txt", false),
                                  "full is the last check");
    playlist_finalize(pl);
    TEST_ASSERT_EQUAL_size_t(3, playlist_count(pl));
}

static void test_names_are_copied(void)
{
    create();
    char buf[32];
    strcpy(buf, "b.mp3");
    add_ok(pl, buf);
    strcpy(buf, "a.mp3");
    add_ok(pl, buf);
    strcpy(buf, "overwritten");
    playlist_finalize(pl);
    expect_next("a.mp3");
    expect_next("b.mp3");
}

/* ------------------------------------------------------------------------ */
/* Order (AUD-02)                                                           */
/* ------------------------------------------------------------------------ */

static void test_natural_order(void)
{
    create();
    const char *in[] = {"b.mp3", "Track 10 - Rain.mp3", "track 2 - Sea.mp3", "A.mp3", "track 1 - Forest.mp3",
                        "10.mp3", "9.mp3", "1.mp3", "01.mp3"};
    const char *expect[] = {"01.mp3", "1.mp3", "9.mp3", "10.mp3", "A.mp3", "b.mp3",
                            "track 1 - Forest.mp3", "track 2 - Sea.mp3", "Track 10 - Rain.mp3"};
    for (size_t i = 0; i < 9; i++) {
        add_ok(pl, in[i]);
    }
    playlist_finalize(pl);
    for (size_t i = 0; i < 9; i++) {
        expect_next(expect[i]);
    }
}

static void test_long_digit_runs_do_not_overflow(void)
{
    create();
    add_ok(pl, "x 100000000000000000000.mp3");
    add_ok(pl, "x 99999999999999999999.mp3");
    add_ok(pl, "x 0000000000000000000000000007.mp3");
    playlist_finalize(pl);
    expect_next("x 0000000000000000000000000007.mp3");
    expect_next("x 99999999999999999999.mp3");
    expect_next("x 100000000000000000000.mp3");
}

static void test_sequential_playback_loops(void)
{
    create();
    add_ok(pl, "c.mp3");
    add_ok(pl, "a.mp3");
    add_ok(pl, "b.mp3");
    playlist_finalize(pl);
    expect_next("a.mp3");
    expect_next("b.mp3");
    expect_next("c.mp3");
    expect_next("a.mp3");
    expect_next("b.mp3");
}

static void test_returned_names_stay_valid(void)
{
    create();
    add_ok(pl, "a.mp3");
    add_ok(pl, "b.mp3");
    playlist_finalize(pl);
    const char *first = playlist_next(pl);
    playlist_next(pl);
    playlist_next(pl);
    TEST_ASSERT_EQUAL_STRING("a.mp3", first);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle and AUD-09                                                     */
/* ------------------------------------------------------------------------ */

static void test_next_is_null_before_finalize(void)
{
    create();
    add_ok(pl, "a.mp3");
    TEST_ASSERT_NULL(playlist_next(pl));
}

static void test_empty_card_gives_no_music(void)
{
    create();
    TEST_ASSERT_EQUAL_INT(PLAYLIST_SKIPPED_NOT_AUDIO, playlist_add(pl, "readme.txt", false));
    playlist_finalize(pl);
    TEST_ASSERT_EQUAL_size_t(0, playlist_count(pl));
    TEST_ASSERT_NULL(playlist_next(pl));
    TEST_ASSERT_NULL(playlist_next(pl));
}

static void test_add_after_finalize_is_rejected_until_clear(void)
{
    create();
    add_ok(pl, "a.mp3");
    playlist_finalize(pl);
    TEST_ASSERT_EQUAL_INT(PLAYLIST_ERR_FINALIZED, playlist_add(pl, "b.mp3", false));
    TEST_ASSERT_EQUAL_size_t(1, playlist_count(pl));

    playlist_clear(pl);
    TEST_ASSERT_EQUAL_size_t(0, playlist_count(pl));
    TEST_ASSERT_NULL_MESSAGE(playlist_next(pl), "cleared playlist is not finalized");
    add_ok(pl, "z.mp3");
    add_ok(pl, "y.mp3");
    playlist_finalize(pl);
    expect_next("y.mp3");
    expect_next("z.mp3");
}

/* ------------------------------------------------------------------------ */
/* Unplayable files (AUD-12)                                                */
/* ------------------------------------------------------------------------ */

static void test_bad_track_is_skipped(void)
{
    create();
    add_ok(pl, "a.mp3");
    add_ok(pl, "b.mp3");
    add_ok(pl, "c.mp3");
    playlist_finalize(pl);
    expect_next("a.mp3");
    expect_next("b.mp3");
    playlist_mark_bad(pl, "b.mp3");
    TEST_ASSERT_EQUAL_size_t(2, playlist_count(pl));
    expect_next("c.mp3");
    expect_next("a.mp3");
    expect_next("c.mp3");
}

static void test_all_bad_gives_no_music(void)
{
    create();
    add_ok(pl, "a.mp3");
    add_ok(pl, "b.mp3");
    playlist_finalize(pl);
    playlist_mark_bad(pl, "a.mp3");
    playlist_mark_bad(pl, "b.mp3");
    TEST_ASSERT_EQUAL_size_t(0, playlist_count(pl));
    TEST_ASSERT_NULL(playlist_next(pl));
}

static void test_unknown_bad_name_is_ignored(void)
{
    create();
    add_ok(pl, "a.mp3");
    playlist_finalize(pl);
    playlist_mark_bad(pl, "nope.mp3");
    playlist_mark_bad(pl, "A.MP3");
    TEST_ASSERT_EQUAL_size_t(1, playlist_count(pl));
    expect_next("a.mp3");
}

static void test_clear_forgets_bad_marks(void)
{
    create();
    add_ok(pl, "a.mp3");
    playlist_finalize(pl);
    playlist_mark_bad(pl, "a.mp3");
    playlist_clear(pl);
    add_ok(pl, "a.mp3");
    playlist_finalize(pl);
    expect_next("a.mp3");
}

/* ------------------------------------------------------------------------ */
/* Shuffle (AUD-03)                                                         */
/* ------------------------------------------------------------------------ */

static int index_of(const char *name, int n)
{
    if (name == NULL || strlen(name) != 6) {
        return -1;
    }
    const int i = (name[0] - 'a') + 26 * (name[1] - 'a');
    return (i >= 0 && i < n) ? i : -1;
}

/* Take `rounds` rounds of n playable tracks and check each is a permutation
 * of the playable set and that no track plays twice in a row. */
static void check_rounds(int n_total, const bool *bad, int playable, int rounds)
{
    int prev = -1;
    for (int r = 0; r < rounds; r++) {
        bool seen[64] = {false};
        for (int k = 0; k < playable; k++) {
            const int i = index_of(playlist_next(pl), n_total);
            TEST_ASSERT_TRUE_MESSAGE(i >= 0, "unknown track returned");
            TEST_ASSERT_FALSE_MESSAGE(bad != NULL && bad[i], "bad track returned");
            TEST_ASSERT_FALSE_MESSAGE(seen[i], "track repeated within a round");
            if (playable >= 2) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(prev, i, "same track twice in a row");
            }
            seen[i] = true;
            prev = i;
        }
    }
}

static void test_shuffle_plays_every_track_once_per_round(void)
{
    cfg.shuffle = true;
    create();
    add_numbered(pl, 6);
    playlist_finalize(pl);
    check_rounds(6, NULL, 6, 50);
}

static void test_shuffle_never_repeats_across_round_boundary(void)
{
    cfg.shuffle = true;
    for (uint32_t seed = 0; seed < 20; seed++) {
        cfg.seed = seed;
        create();
        add_numbered(pl, 2);
        playlist_finalize(pl);
        check_rounds(2, NULL, 2, 20);
        playlist_destroy(pl);
        pl = NULL;
    }
}

static void test_shuffle_differs_from_natural_order(void)
{
    cfg.shuffle = true;
    create();
    add_numbered(pl, 20);
    playlist_finalize(pl);
    int in_order = 0;
    for (int i = 0; i < 20; i++) {
        if (index_of(playlist_next(pl), 20) == i) {
            in_order++;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(in_order < 20, "first shuffled round equals natural order");
}

static void test_shuffle_is_deterministic_per_seed(void)
{
    cfg.shuffle = true;
    cfg.seed = 1234;
    create();
    playlist_t *twin = playlist_create(&cfg);
    cfg.seed = 99;
    playlist_t *other = playlist_create(&cfg);
    TEST_ASSERT_NOT_NULL(twin);
    TEST_ASSERT_NOT_NULL(other);
    add_numbered(pl, 20);
    add_numbered(twin, 20);
    add_numbered(other, 20);
    playlist_finalize(pl);
    playlist_finalize(twin);
    playlist_finalize(other);

    int differences = 0;
    for (int i = 0; i < 60; i++) {
        const char *a = playlist_next(pl);
        TEST_ASSERT_EQUAL_STRING(a, playlist_next(twin));
        if (i < 20 && strcmp(a, playlist_next(other)) != 0) {
            differences++;
        }
    }
    playlist_destroy(twin);
    playlist_destroy(other);
    TEST_ASSERT_TRUE_MESSAGE(differences > 0, "different seeds must give different orders");
}

static void test_shuffle_skips_bad_tracks(void)
{
    cfg.shuffle = true;
    create();
    add_numbered(pl, 6);
    playlist_finalize(pl);
    playlist_mark_bad(pl, "ca.mp3"); /* index 2 */
    bool bad[6] = {false, false, true, false, false, false};
    check_rounds(6, bad, 5, 30);
}

static void test_shuffle_with_one_track(void)
{
    cfg.shuffle = true;
    create();
    add_ok(pl, "only.mp3");
    playlist_finalize(pl);
    expect_next("only.mp3");
    expect_next("only.mp3");
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_create_rejects_invalid_config);
    RUN_TEST(test_only_visible_mp3_files_are_accepted);
    RUN_TEST(test_too_long_names_are_skipped);
    RUN_TEST(test_full_playlist_skips_further_tracks);
    RUN_TEST(test_names_are_copied);
    RUN_TEST(test_natural_order);
    RUN_TEST(test_long_digit_runs_do_not_overflow);
    RUN_TEST(test_sequential_playback_loops);
    RUN_TEST(test_returned_names_stay_valid);
    RUN_TEST(test_next_is_null_before_finalize);
    RUN_TEST(test_empty_card_gives_no_music);
    RUN_TEST(test_add_after_finalize_is_rejected_until_clear);
    RUN_TEST(test_bad_track_is_skipped);
    RUN_TEST(test_all_bad_gives_no_music);
    RUN_TEST(test_unknown_bad_name_is_ignored);
    RUN_TEST(test_clear_forgets_bad_marks);
    RUN_TEST(test_shuffle_plays_every_track_once_per_round);
    RUN_TEST(test_shuffle_never_repeats_across_round_boundary);
    RUN_TEST(test_shuffle_differs_from_natural_order);
    RUN_TEST(test_shuffle_is_deterministic_per_seed);
    RUN_TEST(test_shuffle_skips_bad_tracks);
    RUN_TEST(test_shuffle_with_one_track);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
