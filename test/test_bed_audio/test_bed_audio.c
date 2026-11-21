/*
 * Tests for lib/bed_audio – requirements BED-41 ... BED-45, BED-50, BED-53, AUD-13/14.
 * Needs lib/folder_list (and bed_modes' bed_next_folder) to pass.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "bed_audio.h"
#include "mode_volume.h"

#define UUID_MED "11111111-2222-3333-4444-555555555555"
#define UUID_MOOD "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
#define UUID_NORMAL "99999999-8888-7777-6666-555555555555"

static bed_audio_ctx_t ctx;
static link_audio_t out;
static folder_list_t list;

void setUp(void)
{
    memset(&ctx, 0, sizeof(ctx));
    ctx.med_folder = "meditation";
    ctx.mood_folder = "mood_rain";
    ctx.sleep_folder = "true_crime";
    ctx.volume[BED_VOL_MEDITATION] = 10;
    ctx.volume[BED_VOL_MOOD] = 14;
    ctx.volume[BED_VOL_SLEEP] = 8;
    memset(&out, 0x77, sizeof(out));
    folder_list_init(&list);
}

void tearDown(void) {}

static bed_cmd_t play(bed_mode_t folder, bool resume, bool keep, uint32_t fade_ms)
{
    bed_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.type = BED_CMD_PLAY;
    c.folder = folder;
    c.resume = resume;
    c.keep = keep;
    c.fade_ms = fade_ms;
    return c;
}

static bed_cmd_t simple(bed_cmd_type_t t, uint32_t fade_ms)
{
    bed_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.type = t;
    c.fade_ms = fade_ms;
    return c;
}

static void fill_list(const char *const *names, uint8_t n)
{
    link_folder_t f;
    for (uint8_t i = 0; i < n; i++) {
        memset(&f, 0, sizeof(f));
        f.index = i;
        f.count = n;
        strcpy(f.name, names[i]);
        TEST_ASSERT_TRUE(folder_list_feed(&list, &f));
    }
    TEST_ASSERT_TRUE(folder_list_ready(&list));
}

static void expect_untouched(void)
{
    link_audio_t ref;
    memset(&ref, 0x77, sizeof(ref));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&ref, &out, sizeof(out), "out must be untouched");
}

/* ---- speaker messages -------------------------------------------------- */

static void test_play_each_mode(void)
{
    bed_cmd_t c = play(BED_MODE_SLEEP, true, true, 3000);
    TEST_ASSERT_TRUE(bed_audio_from_cmd(&c, &ctx, &out));
    TEST_ASSERT_EQUAL_UINT8(LINK_AUDIO_PLAY, out.cmd);
    TEST_ASSERT_EQUAL_HEX8(LINK_AUDIO_FLAG_RESUME | LINK_AUDIO_FLAG_KEEP, out.flags);
    TEST_ASSERT_EQUAL_UINT8(8, out.volume);
    TEST_ASSERT_EQUAL_UINT16(30, out.fade_ds);
    TEST_ASSERT_EQUAL_STRING("true_crime", out.folder);

    c = play(BED_MODE_MEDITATION, false, false, 3000);
    TEST_ASSERT_TRUE(bed_audio_from_cmd(&c, &ctx, &out));
    TEST_ASSERT_EQUAL_HEX8(0, out.flags);
    TEST_ASSERT_EQUAL_UINT8(10, out.volume);
    TEST_ASSERT_EQUAL_STRING("meditation", out.folder);

    c = play(BED_MODE_MOOD, false, true, 0);
    TEST_ASSERT_TRUE(bed_audio_from_cmd(&c, &ctx, &out));
    TEST_ASSERT_EQUAL_HEX8(LINK_AUDIO_FLAG_KEEP, out.flags);
    TEST_ASSERT_EQUAL_UINT8(14, out.volume);
    TEST_ASSERT_EQUAL_UINT16(0, out.fade_ds);
    TEST_ASSERT_EQUAL_STRING("mood_rain", out.folder);
}

static void test_play_fills_every_byte(void)
{
    ctx.sleep_folder = "s";
    bed_cmd_t c = play(BED_MODE_SLEEP, false, true, 1000);
    TEST_ASSERT_TRUE(bed_audio_from_cmd(&c, &ctx, &out));
    for (size_t i = 1; i < sizeof(out.folder); i++) {
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, (uint8_t)out.folder[i], "folder NUL-padded");
    }
}

static void test_fade_rounding(void)
{
    const uint32_t ms[] = {0, 49, 50, 149, 150, 2000, 120000, 6553500, 6553549, 6553550, 0xFFFFFFFFu};
    const uint16_t ds[] = {0, 0, 1, 1, 2, 20, 1200, 65535, 65535, 65535, 65535};
    for (size_t i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) {
        bed_cmd_t c = simple(BED_CMD_STOP, ms[i]);
        TEST_ASSERT_TRUE(bed_audio_from_cmd(&c, &ctx, &out));
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(ds[i], out.fade_ds, "fade_ds rounding");
    }
}

static void test_stop_and_next(void)
{
    bed_cmd_t c = simple(BED_CMD_STOP, 2000);
    TEST_ASSERT_TRUE(bed_audio_from_cmd(&c, &ctx, &out));
    link_audio_t want;
    memset(&want, 0, sizeof(want));
    want.cmd = LINK_AUDIO_STOP;
    want.fade_ds = 20;
    TEST_ASSERT_EQUAL_MEMORY(&want, &out, sizeof(out));

    c = simple(BED_CMD_NEXT, 0);
    TEST_ASSERT_TRUE(bed_audio_from_cmd(&c, &ctx, &out));
    memset(&want, 0, sizeof(want));
    want.cmd = LINK_AUDIO_NEXT;
    TEST_ASSERT_EQUAL_MEMORY(&want, &out, sizeof(out));
}

static void test_refused_commands(void)
{
    bed_cmd_t c = simple(BED_CMD_SCENE, 3000);
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    c = simple(BED_CMD_MOOD_FOLDER_NEXT, 0);
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    c = simple((bed_cmd_type_t)42, 0);
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    c = play(BED_MODE_NONE, false, false, 0);
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    c = play((bed_mode_t)9, false, false, 0);
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    expect_untouched();
}

static void test_play_without_usable_folder(void)
{
    bed_cmd_t c = play(BED_MODE_SLEEP, false, true, 0);
    ctx.sleep_folder = "";
    TEST_ASSERT_FALSE_MESSAGE(bed_audio_from_cmd(&c, &ctx, &out), "\"\": the app picks a folder first");
    ctx.sleep_folder = NULL;
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    ctx.sleep_folder = "true crime";
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    ctx.sleep_folder = "abcdefghijklmnopqrstuvwx"; /* 24 */
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    ctx.sleep_folder = "sleep";
    ctx.volume[BED_VOL_SLEEP] = 33;
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, &out));
    expect_untouched();
    TEST_ASSERT_FALSE(bed_audio_from_cmd(NULL, &ctx, &out));
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, NULL, &out));
    TEST_ASSERT_FALSE(bed_audio_from_cmd(&c, &ctx, NULL));
}

static void test_volume_message(void)
{
    TEST_ASSERT_TRUE(bed_audio_volume_msg(17, &out));
    link_audio_t want;
    memset(&want, 0, sizeof(want));
    want.cmd = LINK_AUDIO_VOLUME;
    want.volume = 17;
    TEST_ASSERT_EQUAL_MEMORY(&want, &out, sizeof(out));
    TEST_ASSERT_TRUE(bed_audio_volume_msg(0, &out));
    TEST_ASSERT_TRUE(bed_audio_volume_msg(32, &out));
    memset(&out, 0x77, sizeof(out));
    TEST_ASSERT_FALSE(bed_audio_volume_msg(33, &out));
    expect_untouched();
    TEST_ASSERT_FALSE(bed_audio_volume_msg(5, NULL));
}

/* ---- volume slots and keys --------------------------------------------- */

static void test_volume_slots(void)
{
    TEST_ASSERT_EQUAL_INT(BED_VOL_MEDITATION, bed_audio_volume_index(BED_MODE_MEDITATION));
    TEST_ASSERT_EQUAL_INT(BED_VOL_MOOD, bed_audio_volume_index(BED_MODE_MOOD));
    TEST_ASSERT_EQUAL_INT(BED_VOL_SLEEP, bed_audio_volume_index(BED_MODE_SLEEP));
    TEST_ASSERT_EQUAL_INT(MODE_VOLUME_NONE, bed_audio_volume_index(BED_MODE_NONE));
    TEST_ASSERT_EQUAL_INT(MODE_VOLUME_NONE, bed_audio_volume_index((bed_mode_t)7));
    TEST_ASSERT_EQUAL_STRING("vol_med", bed_audio_volume_key(BED_VOL_MEDITATION));
    TEST_ASSERT_EQUAL_STRING("vol_mood", bed_audio_volume_key(BED_VOL_MOOD));
    TEST_ASSERT_EQUAL_STRING("vol_sleep", bed_audio_volume_key(BED_VOL_SLEEP));
    TEST_ASSERT_NULL(bed_audio_volume_key(-1));
    TEST_ASSERT_NULL(bed_audio_volume_key(3));
}

/* ---- scenes ------------------------------------------------------------ */

static void test_scenes(void)
{
    cfg_bed_t bed;
    memset(&bed, 0, sizeof(bed));
    strcpy(bed.scene_med, UUID_MED);
    strcpy(bed.scene_mood, UUID_MOOD);
    /* scene_sleep left "" */
    TEST_ASSERT_EQUAL_STRING(UUID_MED, bed_audio_scene(BED_SCENE_MEDITATION, &bed, UUID_NORMAL));
    TEST_ASSERT_EQUAL_STRING(UUID_MOOD, bed_audio_scene(BED_SCENE_MOOD, &bed, UUID_NORMAL));
    TEST_ASSERT_NULL_MESSAGE(bed_audio_scene(BED_SCENE_SLEEP, &bed, UUID_NORMAL), "\"\" = leave the lights");
    TEST_ASSERT_EQUAL_STRING(UUID_NORMAL, bed_audio_scene(BED_SCENE_NORMAL, &bed, UUID_NORMAL));
    TEST_ASSERT_NULL(bed_audio_scene(BED_SCENE_NORMAL, &bed, ""));
    TEST_ASSERT_NULL(bed_audio_scene(BED_SCENE_NORMAL, &bed, NULL));
    TEST_ASSERT_EQUAL_STRING_MESSAGE(UUID_NORMAL, bed_audio_scene(BED_SCENE_NORMAL, NULL, UUID_NORMAL),
                                     "normal does not need bed");
    TEST_ASSERT_NULL(bed_audio_scene(BED_SCENE_MOOD, NULL, UUID_NORMAL));
    TEST_ASSERT_NULL(bed_audio_scene((bed_scene_t)12, &bed, UUID_NORMAL));
}

/* ---- mood folders ------------------------------------------------------ */

static const char *const card[] = {"sleep", "mood_rain", "meditation", "mood_chill", "mood_jazz"};

static void test_pick_keeps_current_on_card(void)
{
    fill_list(card, 5);
    char f[24];
    TEST_ASSERT_TRUE(bed_audio_pick_folder(&list, "mood_jazz", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("mood_jazz", f);
}

static void test_pick_replaces_missing_or_empty(void)
{
    fill_list(card, 5);
    char f[24];
    TEST_ASSERT_TRUE(bed_audio_pick_folder(&list, "", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("mood_chill", f);
    TEST_ASSERT_TRUE_MESSAGE(bed_audio_pick_folder(&list, "mood_disco", BED_PREFIX_MOOD, f, sizeof(f)), "gone from the card");
    TEST_ASSERT_EQUAL_STRING("mood_jazz", f);
}

static void test_pick_before_list_arrives(void)
{
    char f[24] = "x";
    TEST_ASSERT_TRUE(bed_audio_pick_folder(&list, "mood_rain", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("mood_rain", f);
    strcpy(f, "x");
    TEST_ASSERT_FALSE(bed_audio_pick_folder(&list, "", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("", f);
}

static void test_pick_without_mood_folders(void)
{
    const char *const plain[] = {"sleep", "meditation"};
    fill_list(plain, 2);
    char f[24] = "x";
    TEST_ASSERT_FALSE(bed_audio_pick_folder(&list, "", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("", f);
    TEST_ASSERT_FALSE(bed_audio_pick_folder(&list, "mood_rain", BED_PREFIX_MOOD, f, sizeof(f)));
}

static void test_pick_failures(void)
{
    fill_list(card, 5);
    char f[24] = "x";
    TEST_ASSERT_FALSE_MESSAGE(bed_audio_pick_folder(&list, "mood_jazz", BED_PREFIX_MOOD, f, 9), "needs 10 bytes");
    TEST_ASSERT_EQUAL_STRING("", f);
    TEST_ASSERT_FALSE(bed_audio_pick_folder(NULL, "mood_jazz", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_FALSE(bed_audio_pick_folder(&list, NULL, BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_FALSE(bed_audio_pick_folder(&list, "mood_jazz", BED_PREFIX_MOOD, NULL, sizeof(f)));
}

static void test_next_mood_cycles(void)
{
    fill_list(card, 5);
    char f[24];
    TEST_ASSERT_TRUE(bed_audio_next_folder(&list, "mood_chill", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("mood_jazz", f);
    TEST_ASSERT_TRUE(bed_audio_next_folder(&list, "mood_jazz", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("mood_rain", f);
    TEST_ASSERT_TRUE(bed_audio_next_folder(&list, "mood_rain", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("mood_chill", f);
    TEST_ASSERT_TRUE(bed_audio_next_folder(&list, "", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("mood_chill", f);
}

static void test_next_mood_failures(void)
{
    char f[24] = "x";
    TEST_ASSERT_FALSE_MESSAGE(bed_audio_next_folder(&list, "mood_rain", BED_PREFIX_MOOD, f, sizeof(f)), "no list yet");
    TEST_ASSERT_EQUAL_STRING("", f);
    const char *const plain[] = {"sleep"};
    fill_list(plain, 1);
    TEST_ASSERT_FALSE(bed_audio_next_folder(&list, "", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_FALSE(bed_audio_next_folder(NULL, "", BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_FALSE(bed_audio_next_folder(&list, NULL, BED_PREFIX_MOOD, f, sizeof(f)));
    TEST_ASSERT_FALSE(bed_audio_next_folder(&list, "", BED_PREFIX_MOOD, NULL, sizeof(f)));
}

static void test_sleep_family(void)
{
    const char *const card2[] = {"sleep", "sleep_true_crime", "mood_rain", "sleep_rain", "meditation"};
    fill_list(card2, 5);
    char f[24];
    TEST_ASSERT_TRUE(bed_audio_pick_folder(&list, "", BED_PREFIX_SLEEP, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("sleep_rain", f);
    TEST_ASSERT_TRUE_MESSAGE(bed_audio_pick_folder(&list, "sleep", BED_PREFIX_SLEEP, f, sizeof(f)),
                             "an old value without the prefix is replaced");
    TEST_ASSERT_EQUAL_STRING("sleep_rain", f);
    TEST_ASSERT_TRUE(bed_audio_pick_folder(&list, "sleep_true_crime", BED_PREFIX_SLEEP, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("sleep_true_crime", f);
    TEST_ASSERT_TRUE(bed_audio_next_folder(&list, "sleep_rain", BED_PREFIX_SLEEP, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("sleep_true_crime", f);
    TEST_ASSERT_TRUE(bed_audio_next_folder(&list, "sleep_true_crime", BED_PREFIX_SLEEP, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("sleep_rain", f, "wraps round, never a mood folder");
    TEST_ASSERT_TRUE_MESSAGE(bed_audio_pick_folder(&list, "mood_rain", BED_PREFIX_SLEEP, f, sizeof(f)),
                             "a folder of the other family is not kept");
    TEST_ASSERT_EQUAL_STRING("sleep_rain", f);
    TEST_ASSERT_FALSE(bed_audio_pick_folder(&list, "", NULL, f, sizeof(f)));
    TEST_ASSERT_FALSE(bed_audio_next_folder(&list, "", NULL, f, sizeof(f)));
}

static void test_prefix_case_insensitive(void)
{
    const char *const card3[] = {"Sleep_Waves", "sleep_rain"};
    fill_list(card3, 2);
    char f[24];
    TEST_ASSERT_TRUE(bed_audio_pick_folder(&list, "Sleep_Waves", BED_PREFIX_SLEEP, f, sizeof(f)));
    TEST_ASSERT_EQUAL_STRING("Sleep_Waves", f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_play_each_mode);
    RUN_TEST(test_play_fills_every_byte);
    RUN_TEST(test_fade_rounding);
    RUN_TEST(test_stop_and_next);
    RUN_TEST(test_refused_commands);
    RUN_TEST(test_play_without_usable_folder);
    RUN_TEST(test_volume_message);
    RUN_TEST(test_volume_slots);
    RUN_TEST(test_scenes);
    RUN_TEST(test_pick_keeps_current_on_card);
    RUN_TEST(test_pick_replaces_missing_or_empty);
    RUN_TEST(test_pick_before_list_arrives);
    RUN_TEST(test_pick_without_mood_folders);
    RUN_TEST(test_pick_failures);
    RUN_TEST(test_next_mood_cycles);
    RUN_TEST(test_next_mood_failures);
    RUN_TEST(test_sleep_family);
    RUN_TEST(test_prefix_case_insensitive);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
