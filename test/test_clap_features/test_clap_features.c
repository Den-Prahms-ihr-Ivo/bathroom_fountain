/*
 * Tests for lib/clap_features – requirements CLAP-20 ... CLAP-23.
 * All signals are synthetic; the windows are 560 samples (35 ms at 16 kHz).
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "clap_features.h"

#define N 560
#define FS 16000.0
#define PI 3.14159265358979323846

static clap_features_config_t cfg;
static clap_gate_t gate;
static clap_features_t f;
static int16_t x[N];
static uint32_t rng;

void setUp(void)
{
    clap_features_config_default(&cfg);
    cfg.sample_rate_hz = 16000;
    cfg.low_hz = 300.0f;
    cfg.high_hz = 1000.0f;
    clap_gate_default(&gate);
    gate.min_band_ratio_db = 6.0f;
    gate.max_rise_ms = 2.0f;
    memset(&f, 0, sizeof(f));
    memset(x, 0, sizeof(x));
    rng = 12345u;
}

void tearDown(void) {}

static double noise(void) /* -1 ... 1 */
{
    rng = rng * 1664525u + 1013904223u;
    return (double)(int32_t)rng / 2147483648.0;
}

static void measure(void)
{
    TEST_ASSERT_TRUE_MESSAGE(clap_features_measure(&cfg, x, N, &f), "measure failed for valid input");
}

/* silence for `lead` samples, then amp * sin(2 pi f t) */
static void tone(double hz, double amp, int lead)
{
    for (int i = 0; i < N; i++) {
        x[i] = i < lead ? 0 : (int16_t)lrint(amp * sin(2.0 * PI * hz * (i - lead) / FS));
    }
}

/* 80 samples of quiet, then noise high-passed by differencing, decaying with tau = 4 ms */
static void make_clap(double amp)
{
    double prev = 0.0;
    for (int i = 0; i < N; i++) {
        const double w = noise();
        const double bright = (w - prev) / 2.0;
        prev = w;
        const double env = i < 80 ? 0.0 : exp(-(i - 80) / (0.004 * FS));
        x[i] = (int16_t)lrint(amp * env * bright);
    }
}

/* 80 Hz thump that swells over 12 ms and dies away */
static void make_door(double amp)
{
    for (int i = 0; i < N; i++) {
        const double t = (i - 80) / FS;
        const double env = i < 80 ? 0.0 : (1.0 - exp(-t / 0.004)) * exp(-t / 0.060);
        x[i] = (int16_t)lrint(amp * env * sin(2.0 * PI * 80.0 * t));
    }
}

static void test_defaults(void)
{
    clap_features_config_t c;
    memset(&c, 0, sizeof(c));
    clap_features_config_default(&c);
    TEST_ASSERT_EQUAL_UINT32(16000, c.sample_rate_hz);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 300.0f, c.low_hz);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1000.0f, c.high_hz);
    clap_gate_t g;
    memset(&g, 0, sizeof(g));
    clap_gate_default(&g);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, g.min_band_ratio_db);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 2.0f, g.max_rise_ms);
}

static void test_invalid_arguments(void)
{
    tone(4000.0, 10000.0, 0);
    measure();
    const clap_features_t untouched = f;
    clap_features_config_t bad = cfg;
    bad.sample_rate_hz = 7999;
    TEST_ASSERT_FALSE(clap_features_measure(&bad, x, N, &f));
    bad.sample_rate_hz = 48001;
    TEST_ASSERT_FALSE(clap_features_measure(&bad, x, N, &f));
    bad = cfg;
    bad.low_hz = 0.0f;
    TEST_ASSERT_FALSE(clap_features_measure(&bad, x, N, &f));
    bad = cfg;
    bad.low_hz = 1000.0f; /* not below high_hz */
    TEST_ASSERT_FALSE(clap_features_measure(&bad, x, N, &f));
    bad = cfg;
    bad.high_hz = 8000.0f; /* not below fs / 2 */
    TEST_ASSERT_FALSE(clap_features_measure(&bad, x, N, &f));
    TEST_ASSERT_FALSE(clap_features_measure(NULL, x, N, &f));
    TEST_ASSERT_FALSE(clap_features_measure(&cfg, NULL, N, &f));
    TEST_ASSERT_FALSE(clap_features_measure(&cfg, x, 0, &f));
    TEST_ASSERT_FALSE(clap_features_measure(&cfg, x, N, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &f, sizeof(f));
}

static void test_peak_level(void)
{
    tone(1000.0, 16384.0, 0); /* 1 kHz at 16 kHz hits the peak exactly every 16 samples */
    measure();
    TEST_ASSERT_FLOAT_WITHIN(0.05f, -6.02f, f.peak_dbfs);
    memset(x, 0, sizeof(x));
    x[300] = -32768;
    measure();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, f.peak_dbfs);
    memset(x, 0, sizeof(x));
    x[10] = 33;
    measure();
    TEST_ASSERT_FLOAT_WITHIN(0.1f, -59.94f, f.peak_dbfs);
}

static void test_silence(void)
{
    f.peak_dbfs = 1.0f;
    f.rise_ms = 1.0f;
    f.band_ratio_db = 1.0f;
    measure();
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, -120.0f, f.peak_dbfs);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, f.rise_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, f.band_ratio_db);
    TEST_ASSERT_FALSE(clap_features_is_clap(&gate, &f));
}

static void test_rise_time(void)
{
    /* |x| is exactly the envelope: alternate the sign every sample */
    for (int i = 0; i < N; i++) {
        const double env = i < 100 ? 0.0 : (i < 260 ? (i - 100) / 160.0 : 1.0); /* 10 ms linear ramp */
        x[i] = (int16_t)lrint(20000.0 * env * ((i & 1) ? -1.0 : 1.0));
    }
    measure();
    TEST_ASSERT_FLOAT_WITHIN(0.15f, 8.0f, f.rise_ms); /* 10 % at sample 116, 90 % at 244 */

    for (int i = 0; i < N; i++) {
        x[i] = (int16_t)(i < 100 ? 0 : ((i & 1) ? -20000 : 20000)); /* a step */
    }
    measure();
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, f.rise_ms);

    /* quiet noise before the onset below 10 % does not start the clock */
    for (int i = 0; i < N; i++) {
        x[i] = (int16_t)(i < 100 ? ((i & 1) ? -1500 : 1500) : (i < 116 ? ((i & 1) ? -9000 : 9000) : ((i & 1) ? -20000 : 20000)));
    }
    measure();
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, f.rise_ms); /* 16 samples from 9000 (45 %) to 20000 */
}

static void test_band_ratio_of_pure_tones(void)
{
    tone(4000.0, 10000.0, 0);
    measure();
    TEST_ASSERT_TRUE_MESSAGE(f.band_ratio_db > 30.0f, "a 4 kHz tone is all high band");
    tone(100.0, 10000.0, 0);
    measure();
    TEST_ASSERT_TRUE_MESSAGE(f.band_ratio_db < -30.0f, "a 100 Hz tone is all low band");
    TEST_ASSERT_TRUE(f.band_ratio_db >= -60.0f);

    /* equal parts of both: near 0 dB */
    for (int i = 0; i < N; i++) {
        x[i] = (int16_t)lrint(8000.0 * sin(2.0 * PI * 100.0 * i / FS) + 8000.0 * sin(2.0 * PI * 4000.0 * i / FS + 0.7));
    }
    measure();
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 0.0f, f.band_ratio_db);

    /* the ratio does not depend on the level */
    for (int i = 0; i < N; i++) {
        x[i] = (int16_t)lrint(800.0 * sin(2.0 * PI * 100.0 * i / FS) + 800.0 * sin(2.0 * PI * 4000.0 * i / FS + 0.7));
    }
    const float loud = f.band_ratio_db;
    measure();
    TEST_ASSERT_FLOAT_WITHIN(0.3f, loud, f.band_ratio_db);

    /* high band 12 dB stronger: ratio near +12 */
    for (int i = 0; i < N; i++) {
        x[i] = (int16_t)lrint(2000.0 * sin(2.0 * PI * 100.0 * i / FS) + 7962.0 * sin(2.0 * PI * 4000.0 * i / FS));
    }
    measure();
    TEST_ASSERT_FLOAT_WITHIN(2.0f, 12.0f, f.band_ratio_db);
}

static void test_corner_frequencies_are_used(void)
{
    tone(600.0, 10000.0, 0); /* between the bands: far weaker in both than a tone inside a band */
    measure();
    const float between = f.band_ratio_db;
    TEST_ASSERT_FLOAT_WITHIN(8.0f, 3.0f, between); /* -12 dB/octave on both sides: roughly even */
    cfg.high_hz = 500.0f; /* now 600 Hz is inside the high band */
    cfg.low_hz = 100.0f;
    measure();
    TEST_ASSERT_TRUE(f.band_ratio_db > between + 15.0f);
}

static void test_a_clap_passes_and_a_door_does_not(void)
{
    make_clap(24000.0);
    measure();
    TEST_ASSERT_TRUE_MESSAGE(f.band_ratio_db > 10.0f, "clap: bright");
    TEST_ASSERT_TRUE_MESSAGE(f.rise_ms < 1.0f, "clap: abrupt");
    TEST_ASSERT_TRUE(clap_features_is_clap(&gate, &f));

    make_clap(1500.0); /* a quiet clap has the same shape */
    measure();
    TEST_ASSERT_TRUE(clap_features_is_clap(&gate, &f));

    make_door(24000.0);
    measure();
    TEST_ASSERT_TRUE_MESSAGE(f.band_ratio_db < -10.0f, "door: dull");
    TEST_ASSERT_TRUE_MESSAGE(f.rise_ms > 2.5f, "door: swells");
    TEST_ASSERT_FALSE(clap_features_is_clap(&gate, &f));
}

static void test_gate_limits(void)
{
    clap_features_t v = {.peak_dbfs = -10.0f, .rise_ms = 2.0f, .band_ratio_db = 6.0f};
    TEST_ASSERT_TRUE_MESSAGE(clap_features_is_clap(&gate, &v), "both limits are inclusive");
    v.rise_ms = 2.01f;
    TEST_ASSERT_FALSE(clap_features_is_clap(&gate, &v));
    v.rise_ms = 0.0f;
    v.band_ratio_db = 5.99f;
    TEST_ASSERT_FALSE(clap_features_is_clap(&gate, &v));
    v.band_ratio_db = 40.0f;
    TEST_ASSERT_TRUE(clap_features_is_clap(&gate, &v));
    gate.min_band_ratio_db = -60.0f; /* band check switched off */
    gate.max_rise_ms = 1000.0f;      /* rise check switched off */
    v.band_ratio_db = -60.0f;
    v.rise_ms = 35.0f;
    TEST_ASSERT_TRUE(clap_features_is_clap(&gate, &v));
    TEST_ASSERT_FALSE(clap_features_is_clap(NULL, &v));
    TEST_ASSERT_FALSE(clap_features_is_clap(&gate, NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_invalid_arguments);
    RUN_TEST(test_peak_level);
    RUN_TEST(test_silence);
    RUN_TEST(test_rise_time);
    RUN_TEST(test_band_ratio_of_pure_tones);
    RUN_TEST(test_corner_frequencies_are_used);
    RUN_TEST(test_a_clap_passes_and_a_door_does_not);
    RUN_TEST(test_gate_limits);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
