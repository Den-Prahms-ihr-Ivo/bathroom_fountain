/*
 * Tests for lib/bath_volume – requirements AUD-13, AUD-14.
 */
#include <stdbool.h>
#include <string.h>
#include <unity.h>

#include "bath_volume.h"
#include "mode_volume.h"

static mode_outputs_t o;

void setUp(void)
{
    memset(&o, 0, sizeof(o));
    o.mode = AMBIENCE_NORMAL;
}

void tearDown(void) {}

static void test_normal_quiet_ignores_encoder(void)
{
    TEST_ASSERT_EQUAL_INT(MODE_VOLUME_NONE, bath_volume_mode(&o, false));
}

static void test_ambient(void)
{
    o.mode = AMBIENCE_AMBIENT;
    o.music_playing = true;
    TEST_ASSERT_EQUAL_INT(BATH_VOL_AMBIENT, bath_volume_mode(&o, false));
    o.music_playing = false; /* fading in, music not started yet / no card */
    TEST_ASSERT_EQUAL_INT(BATH_VOL_AMBIENT, bath_volume_mode(&o, false));
}

static void test_fading_out_of_ambient(void)
{
    o.mode = AMBIENCE_NORMAL; /* target flipped at the trigger */
    o.transitioning = true;
    o.music_playing = true;
    TEST_ASSERT_EQUAL_INT(BATH_VOL_AMBIENT, bath_volume_mode(&o, false));
    o.music_playing = false;
    TEST_ASSERT_EQUAL_INT_MESSAGE(MODE_VOLUME_NONE, bath_volume_mode(&o, false), "music over");
}

static void test_cold_run_wins(void)
{
    TEST_ASSERT_EQUAL_INT(BATH_VOL_COLD, bath_volume_mode(&o, true));
    o.mode = AMBIENCE_AMBIENT;
    o.music_playing = true;
    TEST_ASSERT_EQUAL_INT(BATH_VOL_COLD, bath_volume_mode(&o, true));
    TEST_ASSERT_EQUAL_INT(BATH_VOL_COLD, bath_volume_mode(NULL, true));
    TEST_ASSERT_EQUAL_INT(MODE_VOLUME_NONE, bath_volume_mode(NULL, false));
}

static void test_keys_and_names(void)
{
    TEST_ASSERT_EQUAL_STRING("volume", bath_volume_key(BATH_VOL_AMBIENT));
    TEST_ASSERT_EQUAL_STRING("vol_cold", bath_volume_key(BATH_VOL_COLD));
    TEST_ASSERT_NULL(bath_volume_key(BATH_VOL_COUNT));
    TEST_ASSERT_NULL(bath_volume_key(MODE_VOLUME_NONE));
    TEST_ASSERT_EQUAL_STRING("ambient", bath_volume_name(BATH_VOL_AMBIENT));
    TEST_ASSERT_EQUAL_STRING("cold", bath_volume_name(BATH_VOL_COLD));
    TEST_ASSERT_NULL(bath_volume_name(2));
    TEST_ASSERT_NULL(bath_volume_name(-1));
    TEST_ASSERT_TRUE_MESSAGE(BATH_VOL_COUNT <= MODE_VOLUME_MAX, "fits mode_volume");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_normal_quiet_ignores_encoder);
    RUN_TEST(test_ambient);
    RUN_TEST(test_fading_out_of_ambient);
    RUN_TEST(test_cold_run_wins);
    RUN_TEST(test_keys_and_names);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
