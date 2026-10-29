/*
 * Tests for lib/pcm_mix – requirement AUD-01 (mono, left output).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "pcm_mix.h"

void setUp(void) {}
void tearDown(void) {}

/* Downmix one stereo frame and return L; also checks R is silent. */
static int16_t mix1(int16_t l, int16_t r)
{
    const int16_t in[2] = {l, r};
    int16_t out[2] = {0x5A5A, 0x5A5A};
    TEST_ASSERT_EQUAL_size_t(1, pcm_mix_to_output(in, 1, 2, PCM_RIGHT_SILENT, out));
    TEST_ASSERT_EQUAL_INT16_MESSAGE(0, out[1], "right channel must be silent");
    return out[0];
}

/* ------------------------------------------------------------------------ */

static void test_stereo_is_averaged(void)
{
    TEST_ASSERT_EQUAL_INT16(500, mix1(1000, 0));
    TEST_ASSERT_EQUAL_INT16_MESSAGE(500, mix1(0, 1000), "right-only content must not get lost");
    TEST_ASSERT_EQUAL_INT16(200, mix1(100, 300));
    TEST_ASSERT_EQUAL_INT16(-150, mix1(-100, -200));
}

static void test_extremes_do_not_overflow(void)
{
    TEST_ASSERT_EQUAL_INT16(32767, mix1(32767, 32767));
    TEST_ASSERT_EQUAL_INT16(-32768, mix1(-32768, -32768));
    TEST_ASSERT_EQUAL_INT16(32766, mix1(32767, 32765));
    TEST_ASSERT_EQUAL_INT16(0, mix1(32767, -32767));
}

static void test_halves_truncate_toward_zero(void)
{
    TEST_ASSERT_EQUAL_INT16(0, mix1(1, 0));
    TEST_ASSERT_EQUAL_INT16(0, mix1(-1, 0));
    TEST_ASSERT_EQUAL_INT16(1, mix1(3, 0));
    TEST_ASSERT_EQUAL_INT16(-1, mix1(-3, 0));
    TEST_ASSERT_EQUAL_INT16_MESSAGE(0, mix1(32767, -32768), "-1 / 2 truncates to 0");
}

static void test_identical_channels_pass_unchanged_and_antiphase_cancels(void)
{
    srand(11);
    for (int i = 0; i < 2000; i++) {
        const int16_t x = (int16_t)((rand() % 65535) - 32767);
        TEST_ASSERT_EQUAL_INT16(x, mix1(x, x));
        TEST_ASSERT_EQUAL_INT16(0, mix1(x, (int16_t)-x));
    }
}

static void test_right_copy_mode(void)
{
    const int16_t in[6] = {1000, 0, -32768, -32768, 7, 8};
    int16_t out[6];
    TEST_ASSERT_EQUAL_size_t(3, pcm_mix_to_output(in, 3, 2, PCM_RIGHT_COPY, out));
    const int16_t expect[6] = {500, 500, -32768, -32768, 7, 7};
    TEST_ASSERT_EQUAL_INT16_ARRAY(expect, out, 6);
}

static void test_mono_source_passes_through(void)
{
    const int16_t in[5] = {0, 1, -1, 32767, -32768};
    int16_t out[10];
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(5, pcm_mix_to_output(in, 5, 1, PCM_RIGHT_SILENT, out));
    const int16_t expect[10] = {0, 0, 1, 0, -1, 0, 32767, 0, -32768, 0};
    TEST_ASSERT_EQUAL_INT16_ARRAY(expect, out, 10);

    TEST_ASSERT_EQUAL_size_t(5, pcm_mix_to_output(in, 5, 1, PCM_RIGHT_COPY, out));
    const int16_t expect_copy[10] = {0, 0, 1, 1, -1, -1, 32767, 32767, -32768, -32768};
    TEST_ASSERT_EQUAL_INT16_ARRAY(expect_copy, out, 10);
}

static void test_invalid_arguments_leave_output_untouched(void)
{
    const int16_t in[6] = {1, 2, 3, 4, 5, 6};
    int16_t out[6];
    int16_t sentinel[6];
    memset(out, 0x5A, sizeof(out));
    memcpy(sentinel, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, pcm_mix_to_output(in, 2, 0, PCM_RIGHT_SILENT, out));
    TEST_ASSERT_EQUAL_size_t(0, pcm_mix_to_output(in, 2, 3, PCM_RIGHT_SILENT, out));
    TEST_ASSERT_EQUAL_size_t(0, pcm_mix_to_output(in, 2, 2, (pcm_right_t)7, out));
    TEST_ASSERT_EQUAL_INT16_ARRAY(sentinel, out, 6);
}

static void test_zero_frames(void)
{
    TEST_ASSERT_EQUAL_size_t(0, pcm_mix_to_output(NULL, 0, 2, PCM_RIGHT_SILENT, NULL));
    /* frames 0 with valid arguments is not an error, it just does nothing */
    const int16_t in[2] = {1, 2};
    int16_t out[2] = {0x5A5A, 0x5A5A};
    TEST_ASSERT_EQUAL_size_t(0, pcm_mix_to_output(in, 0, 2, PCM_RIGHT_SILENT, out));
    TEST_ASSERT_EQUAL_INT16(0x5A5A, out[0]);
}

static void test_in_place_stereo_matches_out_of_place(void)
{
    enum { N = 1001 }; /* odd frame count on purpose */
    static int16_t src[2 * N];
    static int16_t ref[2 * N];
    static int16_t buf[2 * N];
    srand(5);
    for (int i = 0; i < 2 * N; i++) {
        src[i] = (int16_t)((rand() % 65536) - 32768);
    }
    TEST_ASSERT_EQUAL_size_t(N, pcm_mix_to_output(src, N, 2, PCM_RIGHT_SILENT, ref));
    memcpy(buf, src, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(N, pcm_mix_to_output(buf, N, 2, PCM_RIGHT_SILENT, buf));
    TEST_ASSERT_EQUAL_INT16_ARRAY(ref, buf, 2 * N);
}

static void test_long_buffer_matches_formula(void)
{
    enum { N = 4096 };
    static int16_t src[2 * N];
    static int16_t out[2 * N];
    srand(99);
    for (int i = 0; i < 2 * N; i++) {
        src[i] = (int16_t)((rand() % 65536) - 32768);
    }
    TEST_ASSERT_EQUAL_size_t(N, pcm_mix_to_output(src, N, 2, PCM_RIGHT_SILENT, out));
    for (int i = 0; i < N; i++) {
        const int32_t expect = ((int32_t)src[2 * i] + (int32_t)src[2 * i + 1]) / 2;
        TEST_ASSERT_EQUAL_INT16((int16_t)expect, out[2 * i]);
        TEST_ASSERT_EQUAL_INT16(0, out[2 * i + 1]);
    }
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stereo_is_averaged);
    RUN_TEST(test_extremes_do_not_overflow);
    RUN_TEST(test_halves_truncate_toward_zero);
    RUN_TEST(test_identical_channels_pass_unchanged_and_antiphase_cancels);
    RUN_TEST(test_right_copy_mode);
    RUN_TEST(test_mono_source_passes_through);
    RUN_TEST(test_invalid_arguments_leave_output_untouched);
    RUN_TEST(test_zero_frames);
    RUN_TEST(test_in_place_stereo_matches_out_of_place);
    RUN_TEST(test_long_buffer_matches_formula);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
