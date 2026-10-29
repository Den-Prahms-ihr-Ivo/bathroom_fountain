/*
 * Tests for lib/mic_frontend – requirements CLAP-01, CLAP-14.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "mic_frontend.h"

#define FS     16000u
#define PI_F   3.14159265f
#define MAXN   32000u

static mic_frontend_config_t cfg;
static mic_frontend_t *mf;
static int32_t in_buf[MAXN];
static int16_t out_buf[MAXN];

void setUp(void)
{
    mic_frontend_config_default(&cfg);
    cfg.sample_rate_hz = FS;
    cfg.shift = 16;
    cfg.hp_cutoff_hz = 20.0f;
    mf = NULL;
}

void tearDown(void)
{
    mic_frontend_destroy(mf);
    mf = NULL;
}

static void create(void)
{
    mf = mic_frontend_create(&cfg);
    TEST_ASSERT_NOT_NULL_MESSAGE(mf, "mic_frontend_create returned NULL for a valid config");
}

/* An int16-scale value as the I2S word that maps onto it with shift 16. */
static int32_t word_of(float v16)
{
    return (int32_t)lroundf(v16 * 65536.0f);
}

static void fill_sine(size_t n, float freq, float amp16)
{
    for (size_t i = 0; i < n; i++) {
        in_buf[i] = word_of(amp16 * sinf(2.0f * PI_F * freq * (float)i / (float)FS));
    }
}

static float peak_abs(size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = from; i < to; i++) {
        const float a = fabsf((float)out_buf[i]);
        if (a > m) {
            m = a;
        }
    }
    return m;
}

static float db(float ratio)
{
    return 20.0f * log10f(ratio);
}

/* ------------------------------------------------------------------------ */

static void test_defaults(void)
{
    mic_frontend_config_t d;
    memset(&d, 0xA5, sizeof(d));
    mic_frontend_config_default(&d);
    TEST_ASSERT_EQUAL_UINT32(16000, d.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT8(16, d.shift);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 20.0f, d.hp_cutoff_hz);
}

static void test_create_rejects_invalid_config(void)
{
    create();
    TEST_ASSERT_NULL(mic_frontend_create(NULL));

    mic_frontend_config_t bad = cfg;
    bad.sample_rate_hz = 0;
    TEST_ASSERT_NULL_MESSAGE(mic_frontend_create(&bad), "rate 0");

    bad = cfg;
    bad.shift = 32;
    TEST_ASSERT_NULL_MESSAGE(mic_frontend_create(&bad), "shift 32");

    bad = cfg;
    bad.hp_cutoff_hz = -1.0f;
    TEST_ASSERT_NULL_MESSAGE(mic_frontend_create(&bad), "negative cutoff");

    bad = cfg;
    bad.hp_cutoff_hz = 8000.0f;
    TEST_ASSERT_NULL_MESSAGE(mic_frontend_create(&bad), "cutoff at Nyquist");

    bad = cfg;
    bad.hp_cutoff_hz = NAN;
    TEST_ASSERT_NULL_MESSAGE(mic_frontend_create(&bad), "NaN cutoff");

    bad = cfg;
    bad.hp_cutoff_hz = 0.0f;
    mic_frontend_t *ok = mic_frontend_create(&bad);
    TEST_ASSERT_NOT_NULL_MESSAGE(ok, "cutoff 0 disables the filter and is valid");
    mic_frontend_destroy(ok);
}

static void test_shift_without_filter_is_exact_floor(void)
{
    cfg.hp_cutoff_hz = 0.0f;
    create();
    const int32_t words[] = {0, 65536, 0x7FFFFF00, (int32_t)0x80000000, -65536, -1, -65537, 98304, -98304};
    const int16_t expect[] = {0, 1, 32767, -32768, -1, -1, -2, 1, -2};
    int16_t out[9];
    TEST_ASSERT_EQUAL_size_t(9, mic_frontend_process(mf, words, 9, out));
    TEST_ASSERT_EQUAL_INT16_ARRAY(expect, out, 9);
}

static void test_smaller_shift_adds_gain_and_saturates(void)
{
    cfg.hp_cutoff_hz = 0.0f;
    cfg.shift = 12; /* +24 dB */
    create();
    const int32_t words[] = {4096 * 100, 0x10000000, -0x10000000, 0x7FFFFF00};
    const int16_t expect[] = {100, 32767, -32768, 32767};
    int16_t out[4];
    mic_frontend_process(mf, words, 4, out);
    TEST_ASSERT_EQUAL_INT16_ARRAY(expect, out, 4);
}

static void test_dc_offset_is_removed(void)
{
    create();
    const size_t n = FS / 5; /* 200 ms */
    for (size_t i = 0; i < n; i++) {
        in_buf[i] = word_of(-2000.0f);
    }
    mic_frontend_process(mf, in_buf, n, out_buf);
    TEST_ASSERT_TRUE_MESSAGE(out_buf[0] <= -1990, "first output is the step itself");
    TEST_ASSERT_TRUE_MESSAGE(peak_abs(n - FS / 50, n) <= 1.0f, "DC must have decayed after 200 ms");
}

static void test_passband_tone_is_unchanged(void)
{
    create();
    const size_t n = FS / 2;
    fill_sine(n, 1000.0f, 3000.0f);
    mic_frontend_process(mf, in_buf, n, out_buf);
    const float gain = db(peak_abs(n / 2, n) / 3000.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, 0.0f, gain, "1 kHz must pass within 0.2 dB");
}

static void test_cutoff_frequency_is_about_minus_3_db(void)
{
    cfg.hp_cutoff_hz = 100.0f;
    create();
    const size_t n = FS;
    fill_sine(n, 100.0f, 10000.0f);
    mic_frontend_process(mf, in_buf, n, out_buf);
    const float gain = db(peak_abs(n / 2, n) / 10000.0f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, -3.0f, gain, "gain at the cutoff frequency");
}

static void test_higher_cutoff_suppresses_low_thumps(void) /* CLAP-14 */
{
    cfg.hp_cutoff_hz = 300.0f;
    create();
    const size_t n = FS;
    fill_sine(n, 50.0f, 10000.0f);
    mic_frontend_process(mf, in_buf, n, out_buf);
    const float gain = db(peak_abs(n / 2, n) / 10000.0f);
    TEST_ASSERT_TRUE_MESSAGE(gain < -12.0f, "50 Hz must be at least 12 dB down with a 300 Hz cutoff");
}

/* The output is the filter value rounded to nearest, for both signs: after a
 * step, y[n] = R^n * step decays smoothly, so truncation would be off by up
 * to one count on many samples. */
static void test_filter_output_is_rounded_to_nearest(void)
{
    create();
    const double r = exp(-2.0 * 3.14159265358979 * 20.0 / 16000.0);
    const float steps[] = {1000.0f, -1000.0f};
    for (size_t k = 0; k < 2; k++) {
        mic_frontend_reset(mf);
        const size_t n = 2000;
        for (size_t i = 0; i < n; i++) {
            in_buf[i] = word_of(steps[k]);
        }
        mic_frontend_process(mf, in_buf, n, out_buf);
        double y = (double)steps[k];
        for (size_t i = 0; i < n; i++) {
            TEST_ASSERT_TRUE_MESSAGE(fabs((double)out_buf[i] - y) <= 0.51, "output must be y rounded to nearest");
            y *= r;
        }
    }
}

/* The filter state stays unsaturated: after a full-scale step the output
 * stays clipped for many samples instead of collapsing at once. */
static void test_filter_state_is_not_saturated(void)
{
    create();
    const size_t half = FS / 2;
    for (size_t i = 0; i < 2 * half; i++) {
        in_buf[i] = (i < half) ? (int32_t)0x80000000 : 0x7FFFFF00;
    }
    mic_frontend_process(mf, in_buf, 2 * half, out_buf);
    TEST_ASSERT_EQUAL_INT16_MESSAGE(32767, out_buf[half], "edge must clip, not wrap");
    TEST_ASSERT_EQUAL_INT16_MESSAGE(32767, out_buf[half + 40], "still clipped 40 samples later");
    TEST_ASSERT_TRUE_MESSAGE(out_buf[half + 1000] < 32767, "and decays eventually");
}

static void test_result_independent_of_block_size(void)
{
    cfg.hp_cutoff_hz = 150.0f;
    create();
    const size_t n = 4000;
    srand(7);
    for (size_t i = 0; i < n; i++) {
        in_buf[i] = (int32_t)((rand() % 2000001) - 1000000) * 256 + 0x123400;
    }
    static int16_t ref[4000];
    mic_frontend_process(mf, in_buf, n, ref);

    const size_t blocks[] = {1, 7, 256};
    for (size_t b = 0; b < 3; b++) {
        mic_frontend_reset(mf);
        for (size_t pos = 0; pos < n; pos += blocks[b]) {
            const size_t k = (n - pos < blocks[b]) ? n - pos : blocks[b];
            TEST_ASSERT_EQUAL_size_t(k, mic_frontend_process(mf, in_buf + pos, k, out_buf + pos));
        }
        TEST_ASSERT_EQUAL_INT16_ARRAY(ref, out_buf, n);
    }
}

static void test_reset_clears_filter_state(void)
{
    create();
    const size_t n = 2000;
    fill_sine(n, 440.0f, 5000.0f);
    static int16_t fresh[2000];
    mic_frontend_process(mf, in_buf, n, fresh);

    for (size_t i = 0; i < n; i++) {
        in_buf[i] = word_of(-20000.0f);
    }
    mic_frontend_process(mf, in_buf, n, out_buf);

    mic_frontend_reset(mf);
    fill_sine(n, 440.0f, 5000.0f);
    mic_frontend_process(mf, in_buf, n, out_buf);
    TEST_ASSERT_EQUAL_INT16_ARRAY(fresh, out_buf, n);
}

static void test_zero_length_is_safe(void)
{
    create();
    TEST_ASSERT_EQUAL_size_t(0, mic_frontend_process(mf, NULL, 0, NULL));
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_create_rejects_invalid_config);
    RUN_TEST(test_shift_without_filter_is_exact_floor);
    RUN_TEST(test_smaller_shift_adds_gain_and_saturates);
    RUN_TEST(test_dc_offset_is_removed);
    RUN_TEST(test_passband_tone_is_unchanged);
    RUN_TEST(test_cutoff_frequency_is_about_minus_3_db);
    RUN_TEST(test_higher_cutoff_suppresses_low_thumps);
    RUN_TEST(test_filter_output_is_rounded_to_nearest);
    RUN_TEST(test_filter_state_is_not_saturated);
    RUN_TEST(test_result_independent_of_block_size);
    RUN_TEST(test_reset_clears_filter_state);
    RUN_TEST(test_zero_length_is_safe);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
