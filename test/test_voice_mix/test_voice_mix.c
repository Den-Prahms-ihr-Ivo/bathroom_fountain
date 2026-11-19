/*
 * Tests for lib/voice_mix – requirements COLD-10, COLD-11.
 * The clip source is an array that counts how often it was asked.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "voice_mix.h"

#define MAX_FRAMES 60000

typedef struct {
    const int16_t *data;
    size_t n;
    size_t pos;
    int calls;
    int calls_after_end;
    bool ended;
    size_t biggest_request;
} source_t;

static voice_mix_config_t cfg;
static voice_mix_t *vm;
static int16_t clip[24000];
static int16_t out[MAX_FRAMES * 2];
static source_t src;
static source_t src2;

static size_t source_read(void *ctx, int16_t *dst, size_t max)
{
    source_t *s = ctx;
    s->calls++;
    if (s->ended) {
        s->calls_after_end++;
    }
    if (max > s->biggest_request) {
        s->biggest_request = max;
    }
    size_t n = s->n - s->pos;
    if (n > max) {
        n = max;
    }
    if (n > 100) {
        n = 100; /* short reads are allowed */
    }
    memcpy(dst, s->data + s->pos, n * sizeof(int16_t));
    s->pos += n;
    if (n == 0) {
        s->ended = true;
    }
    return n;
}

static void source_init(source_t *s, const int16_t *data, size_t n)
{
    memset(s, 0, sizeof(*s));
    s->data = data;
    s->n = n;
}

void setUp(void)
{
    voice_mix_config_default(&cfg);
    cfg.out_rate_hz = 48000;
    cfg.duck_db = 12.0f;
    cfg.duck_ramp_ms = 60;
    cfg.voice_gain_db = 0.0f;
    vm = NULL;
    memset(clip, 0, sizeof(clip));
    memset(out, 0, sizeof(out));
    source_init(&src, clip, 0);
    source_init(&src2, clip, 0);
}

void tearDown(void)
{
    voice_mix_destroy(vm);
    vm = NULL;
}

static void create(void)
{
    vm = voice_mix_create(&cfg);
    TEST_ASSERT_NOT_NULL_MESSAGE(vm, "voice_mix_create returned NULL for a valid config");
}

static void music(int16_t left, int16_t right, size_t frames)
{
    for (size_t i = 0; i < frames; i++) {
        out[2 * i] = left;
        out[2 * i + 1] = right;
    }
}

static void start(uint32_t rate, size_t n)
{
    source_init(&src, clip, n);
    TEST_ASSERT_TRUE(voice_mix_start(vm, rate, source_read, &src));
}

/* Number of frames processed one at a time until the mixer is idle. */
static size_t run_until_idle(size_t limit)
{
    size_t frames = 0;
    while (voice_mix_active(vm) && frames < limit) {
        int16_t f[2] = {0, 0};
        voice_mix_process(vm, f, 1);
        frames++;
    }
    return frames;
}

static void test_defaults_and_validation(void)
{
    voice_mix_config_t d;
    memset(&d, 0, sizeof(d));
    voice_mix_config_default(&d);
    TEST_ASSERT_EQUAL_UINT32(48000, d.out_rate_hz);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 12.0f, d.duck_db);
    TEST_ASSERT_EQUAL_UINT16(60, d.duck_ramp_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, d.voice_gain_db);
    create();

    voice_mix_config_t bad = d;
    bad.out_rate_hz = 7999;
    TEST_ASSERT_NULL(voice_mix_create(&bad));
    bad = d;
    bad.out_rate_hz = 48001;
    TEST_ASSERT_NULL(voice_mix_create(&bad));
    bad = d;
    bad.duck_db = -0.5f;
    TEST_ASSERT_NULL(voice_mix_create(&bad));
    bad = d;
    bad.duck_db = 40.5f;
    TEST_ASSERT_NULL(voice_mix_create(&bad));
    bad = d;
    bad.duck_ramp_ms = 1001;
    TEST_ASSERT_NULL(voice_mix_create(&bad));
    bad = d;
    bad.voice_gain_db = 12.5f;
    TEST_ASSERT_NULL(voice_mix_create(&bad));
    bad = d;
    bad.voice_gain_db = -20.5f;
    TEST_ASSERT_NULL(voice_mix_create(&bad));
    TEST_ASSERT_NULL(voice_mix_create(NULL));
    voice_mix_destroy(NULL);
}

static void test_idle_passes_music_through_bit_exact(void)
{
    create();
    for (size_t i = 0; i < 2000; i++) {
        out[i] = (int16_t)(i * 37u - 30000u);
    }
    static int16_t copy[2000];
    memcpy(copy, out, sizeof(copy));
    voice_mix_process(vm, out, 1000);
    TEST_ASSERT_EQUAL_INT16_ARRAY(copy, out, 2000);
    TEST_ASSERT_FALSE(voice_mix_active(vm));
}

static void test_same_rate_clip_comes_out_unchanged_on_both_channels(void)
{
    create();
    for (int i = 0; i < 1000; i++) {
        clip[i] = (int16_t)(1000 + i * 3);
    }
    start(48000, 1000);
    TEST_ASSERT_TRUE(voice_mix_active(vm));
    voice_mix_process(vm, out, 1200); /* music is silence */
    for (int i = 0; i < 1000; i++) {
        TEST_ASSERT_EQUAL_INT16(clip[i], out[2 * i]);
        TEST_ASSERT_EQUAL_INT16(clip[i], out[2 * i + 1]);
    }
    for (int i = 1001; i < 1200; i++) { /* frame 1000 may hold the last value */
        TEST_ASSERT_EQUAL_INT16(0, out[2 * i]);
    }
    TEST_ASSERT_FALSE(voice_mix_active(vm));
    TEST_ASSERT_EQUAL_INT(0, src.calls_after_end);
    TEST_ASSERT_TRUE(src.biggest_request <= VOICE_MIX_READ_MAX);
    voice_mix_process(vm, out, 1200);
    TEST_ASSERT_EQUAL_INT(0, src.calls_after_end);
}

static void test_music_is_ducked_while_the_voice_speaks(void)
{
    create();
    start(48000, 24000); /* 500 ms of silent voice */
    music(10000, -10000, 24000);
    voice_mix_process(vm, out, 24000);
    TEST_ASSERT_TRUE(out[0] > 9900);            /* the ramp starts from 1 */
    TEST_ASSERT_INT16_WITHIN(60, 6256, out[2 * 1440]); /* half way: (1 + 0.2512) / 2 */
    for (size_t i = 1; i < 24000; i++) {
        TEST_ASSERT_TRUE_MESSAGE(out[2 * i] <= out[2 * (i - 1)], "falling ramp must be monotonic");
        TEST_ASSERT_EQUAL_INT16(out[2 * i], -out[2 * i + 1]);
    }
    for (size_t i = 2890; i < 24000; i++) { /* 60 ms = 2880 frames */
        TEST_ASSERT_INT16_WITHIN(1, 2512, out[2 * i]);
    }
    TEST_ASSERT_FALSE(voice_mix_active(vm));

    /* the clip is over: back up within 60 ms, then bit-exact */
    music(10000, -10000, 6000);
    voice_mix_process(vm, out, 6000);
    for (size_t i = 1; i < 6000; i++) {
        TEST_ASSERT_TRUE_MESSAGE(out[2 * i] >= out[2 * (i - 1)], "rising ramp must be monotonic");
    }
    TEST_ASSERT_INT16_WITHIN(80, 6256, out[2 * 1440]);
    for (size_t i = 2890; i < 6000; i++) {
        TEST_ASSERT_EQUAL_INT16(10000, out[2 * i]);
        TEST_ASSERT_EQUAL_INT16(-10000, out[2 * i + 1]);
    }
}

static void test_zero_ramp_switches_at_once(void)
{
    cfg.duck_ramp_ms = 0;
    cfg.duck_db = 20.0f;
    create();
    start(48000, 100);
    music(20000, 20000, 300);
    voice_mix_process(vm, out, 300);
    TEST_ASSERT_INT16_WITHIN(1, 2000, out[0]);
    TEST_ASSERT_INT16_WITHIN(1, 2000, out[2 * 99]);
    TEST_ASSERT_EQUAL_INT16(20000, out[2 * 102]);
    TEST_ASSERT_EQUAL_INT16(20000, out[2 * 299]);
}

static void test_slower_clip_is_interpolated(void)
{
    cfg.duck_ramp_ms = 0;
    create();
    for (int i = 0; i < 2000; i++) {
        clip[i] = (int16_t)(i * 10);
    }
    start(24000, 2000);
    voice_mix_process(vm, out, 3990);
    TEST_ASSERT_EQUAL_INT16(0, out[0]);
    for (int k = 0; k < 3990; k++) {
        TEST_ASSERT_INT16_WITHIN(1, k * 5, out[2 * k]); /* half steps between the clip samples */
    }
    TEST_ASSERT_TRUE(voice_mix_active(vm));
    const size_t rest = run_until_idle(100);
    TEST_ASSERT_TRUE_MESSAGE(rest >= 9 && rest <= 11, "2000 samples at 24 kHz are 4000 frames at 48 kHz (+-1)");
}

static void test_clip_length_at_other_rates(void)
{
    create();
    start(48000, 4800); /* 100 ms */
    TEST_ASSERT_TRUE(voice_mix_set_out_rate(vm, 44100));
    size_t frames = run_until_idle(MAX_FRAMES);
    TEST_ASSERT_TRUE_MESSAGE(frames >= 4409 && frames <= 4411, "100 ms at 44.1 kHz");

    start(16000, 1600); /* 100 ms */
    frames = run_until_idle(MAX_FRAMES);
    TEST_ASSERT_TRUE_MESSAGE(frames >= 4409 && frames <= 4411, "16 kHz clip, 100 ms at 44.1 kHz");

    TEST_ASSERT_FALSE(voice_mix_set_out_rate(vm, 7999));
    TEST_ASSERT_FALSE(voice_mix_set_out_rate(vm, 96000));
    start(44100, 4410);
    frames = run_until_idle(MAX_FRAMES);
    TEST_ASSERT_TRUE_MESSAGE(frames >= 4409 && frames <= 4411, "a rejected rate changes nothing");
}

static void test_rate_change_keeps_the_clip_position(void)
{
    create();
    start(48000, 9600); /* 200 ms */
    voice_mix_process(vm, out, 4800); /* 100 ms played */
    TEST_ASSERT_TRUE(voice_mix_set_out_rate(vm, 24000));
    const size_t frames = run_until_idle(MAX_FRAMES);
    TEST_ASSERT_TRUE_MESSAGE(frames >= 2399 && frames <= 2401, "the other 100 ms are 2400 frames at 24 kHz");
}

static void test_mix_saturates(void)
{
    cfg.duck_db = 0.0f;
    create();
    for (int i = 0; i < 100; i++) {
        clip[i] = (int16_t)(i < 50 ? 10000 : -10000);
    }
    start(48000, 100);
    music(30000, -30000, 100);
    voice_mix_process(vm, out, 100);
    TEST_ASSERT_EQUAL_INT16(32767, out[2 * 10]);      /* 30000 + 10000 */
    TEST_ASSERT_EQUAL_INT16(-20000, out[2 * 10 + 1]); /* -30000 + 10000 */
    TEST_ASSERT_EQUAL_INT16(20000, out[2 * 60]);
    TEST_ASSERT_EQUAL_INT16(-32768, out[2 * 60 + 1]);
}

static void test_voice_gain(void)
{
    cfg.voice_gain_db = 6.0206f;
    create();
    for (int i = 0; i < 100; i++) {
        clip[i] = (int16_t)(i * 100 - 5000);
    }
    start(48000, 100);
    voice_mix_process(vm, out, 100);
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_INT16_WITHIN(1, 2 * clip[i], out[2 * i]);
    }
    voice_mix_destroy(vm);
    cfg.voice_gain_db = -6.0206f;
    create();
    memset(out, 0, sizeof(out));
    start(48000, 100);
    voice_mix_process(vm, out, 100);
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_INT16_WITHIN(1, clip[i] / 2, out[2 * i]);
    }
}

static void test_stop_silences_the_voice_at_once(void)
{
    create();
    for (int i = 0; i < 24000; i++) {
        clip[i] = 5000;
    }
    start(48000, 24000);
    voice_mix_process(vm, out, 4800);
    TEST_ASSERT_EQUAL_INT16(5000, out[2 * 4799]);
    voice_mix_stop(vm);
    TEST_ASSERT_FALSE(voice_mix_active(vm));
    const int calls = src.calls;
    music(10000, 10000, 4800);
    voice_mix_process(vm, out, 4800);
    TEST_ASSERT_TRUE(out[0] < 2600); /* no voice any more, music still ducked */
    TEST_ASSERT_EQUAL_INT16(10000, out[2 * 4799]);
    TEST_ASSERT_EQUAL_INT(calls, src.calls);
    voice_mix_stop(vm); /* idle: no effect */
    voice_mix_stop(NULL);
}

static void test_a_new_clip_replaces_the_running_one(void)
{
    static int16_t second[500];
    for (int i = 0; i < 500; i++) {
        second[i] = (int16_t)(-3000 - i);
    }
    create();
    for (int i = 0; i < 24000; i++) {
        clip[i] = 7000;
    }
    start(48000, 24000);
    voice_mix_process(vm, out, 1000);
    const int calls = src.calls;
    source_init(&src2, second, 500);
    TEST_ASSERT_TRUE(voice_mix_start(vm, 48000, source_read, &src2));
    memset(out, 0, sizeof(out));
    voice_mix_process(vm, out, 600);
    for (int i = 0; i < 500; i++) {
        TEST_ASSERT_EQUAL_INT16(second[i], out[2 * i]);
    }
    TEST_ASSERT_EQUAL_INT16(0, out[2 * 550]);
    TEST_ASSERT_EQUAL_INT(calls, src.calls);
    TEST_ASSERT_FALSE(voice_mix_active(vm));

    TEST_ASSERT_FALSE(voice_mix_start(vm, 48000, NULL, &src2));
    TEST_ASSERT_FALSE(voice_mix_start(vm, 7999, source_read, &src2));
    TEST_ASSERT_FALSE(voice_mix_start(vm, 48001, source_read, &src2));
    TEST_ASSERT_FALSE(voice_mix_active(vm));
}

static void test_a_clip_during_the_rise_turns_the_ramp_around(void)
{
    create();
    start(48000, 4800);
    music(10000, 10000, 4800);
    voice_mix_process(vm, out, 4800);
    music(10000, 10000, 1440);
    voice_mix_process(vm, out, 1440); /* half way up again */
    const int16_t mid = out[2 * 1439];
    TEST_ASSERT_INT16_WITHIN(80, 6256, mid);
    start(48000, 4800);
    music(10000, 10000, 4800);
    voice_mix_process(vm, out, 4800);
    TEST_ASSERT_INT16_WITHIN(20, mid, out[0]); /* no jump */
    for (size_t i = 1; i < 4800; i++) {
        TEST_ASSERT_TRUE(out[2 * i] <= out[2 * (i - 1)]);
    }
    TEST_ASSERT_INT16_WITHIN(1, 2512, out[2 * 1500]); /* reached the floor in half the time */
}

static void test_block_size_does_not_matter(void)
{
    static int16_t a[2 * 9000];
    static int16_t b[2 * 9000];
    for (int i = 0; i < 2000; i++) {
        clip[i] = (int16_t)(8000.0 * sin(i * 0.05));
    }
    for (int pass = 0; pass < 2; pass++) {
        int16_t *dst = pass == 0 ? a : b;
        for (int i = 0; i < 9000; i++) {
            dst[2 * i] = (int16_t)(i * 3 - 12000);
            dst[2 * i + 1] = (int16_t)(12000 - i * 2);
        }
        voice_mix_destroy(vm);
        create();
        start(22050, 2000);
        if (pass == 0) {
            voice_mix_process(vm, dst, 9000);
        } else {
            size_t done = 0;
            size_t step = 1;
            while (done < 9000) {
                size_t n = step;
                if (n > 9000 - done) {
                    n = 9000 - done;
                }
                voice_mix_process(vm, dst + 2 * done, n);
                done += n;
                step = step * 3 % 331 + 1;
            }
        }
    }
    TEST_ASSERT_EQUAL_INT16_ARRAY(a, b, 2 * 9000);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_and_validation);
    RUN_TEST(test_idle_passes_music_through_bit_exact);
    RUN_TEST(test_same_rate_clip_comes_out_unchanged_on_both_channels);
    RUN_TEST(test_music_is_ducked_while_the_voice_speaks);
    RUN_TEST(test_zero_ramp_switches_at_once);
    RUN_TEST(test_slower_clip_is_interpolated);
    RUN_TEST(test_clip_length_at_other_rates);
    RUN_TEST(test_rate_change_keeps_the_clip_position);
    RUN_TEST(test_mix_saturates);
    RUN_TEST(test_voice_gain);
    RUN_TEST(test_stop_silences_the_voice_at_once);
    RUN_TEST(test_a_new_clip_replaces_the_running_one);
    RUN_TEST(test_a_clip_during_the_rise_turns_the_ramp_around);
    RUN_TEST(test_block_size_does_not_matter);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
