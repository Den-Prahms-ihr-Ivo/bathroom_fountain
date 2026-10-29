/*
 * Tests for lib/clap_detector – requirements CLAP-02 … CLAP-06, CLAP-13.
 *
 * Every test builds a synthetic "scene" (background noise, claps, echoes,
 * reverb tails, rattles), renders it to 16 kHz PCM block by block and feeds it
 * to the detector.
 *
 * Signal model
 *   clap    burst of white noise, 1 ms linear attack, exponential decay with a
 *           15 ms time constant, 200 ms long. Level = peak envelope in dBFS.
 *   noise   steady white noise. Level = RMS in dBFS.
 *   tail    white noise with an exponential decay (reverb). Level = start
 *           envelope in dBFS.
 * Noise samples come from a hash of (seed, sample index), so any block of the
 * scene can be rendered on its own and every run is deterministic.
 *
 * Timing tolerances: onsets within ±15 ms of the true clap start;
 * confirmation within 50 ms after the end of the quiet-after window.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "clap_detector.h"

#define FS            16000u
#define MS(ms)        ((uint32_t)((uint64_t)(ms) * FS / 1000u))
#define ONSET_TOL_MS  15u
#define CONFIRM_TOL_MS 50u
#define MAX_EVENTS    8u

#define ROOM_DBFS   (-70.0f) /* quiet bathroom                     */
#define SHOWER_DBFS (-40.0f) /* loud steady background             */
#define CLAP_DBFS   (-10.0f) /* deliberate clap at 1–3 m           */

/* ------------------------------------------------------------------------ */
/* Scene synthesis                                                          */
/* ------------------------------------------------------------------------ */

typedef enum { EV_NOISE, EV_CLAP, EV_TAIL } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint32_t start; /* sample index           */
    uint32_t len;   /* samples                */
    float amp;      /* linear amplitude       */
    float tau;      /* decay constant, samples */
    uint32_t seed;
} ev_t;

typedef struct {
    ev_t ev[24];
    int n;
    uint32_t total; /* samples */
} scene_t;

static float dbfs_to_amp(float dbfs)
{
    return 32768.0f * powf(10.0f, dbfs / 20.0f);
}

/* Deterministic uniform noise in [-1, 1), addressable by sample index. */
static float urand(uint32_t seed, uint32_t i)
{
    uint32_t x = (i * 0x9E3779B1u) ^ seed;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (float)((double)x / 2147483648.0 - 1.0);
}

static void scene_init(scene_t *s, uint32_t total_ms)
{
    memset(s, 0, sizeof(*s));
    s->total = MS(total_ms);
}

static ev_t *scene_add(scene_t *s, ev_kind_t kind, uint32_t start_ms, uint32_t len_ms)
{
    TEST_ASSERT_TRUE_MESSAGE(s->n < (int)(sizeof(s->ev) / sizeof(s->ev[0])), "scene full");
    ev_t *e = &s->ev[s->n];
    e->kind = kind;
    e->start = MS(start_ms);
    e->len = MS(len_ms);
    e->seed = 0x1234567u + 7919u * (uint32_t)s->n;
    s->n++;
    return e;
}

/* Steady noise from from_ms to to_ms at the given RMS level. */
static void add_noise(scene_t *s, uint32_t from_ms, uint32_t to_ms, float rms_dbfs)
{
    ev_t *e = scene_add(s, EV_NOISE, from_ms, to_ms - from_ms);
    e->amp = dbfs_to_amp(rms_dbfs) * sqrtf(3.0f); /* uniform: rms = amp / sqrt(3) */
}

static void add_clap(scene_t *s, uint32_t at_ms, float peak_dbfs)
{
    ev_t *e = scene_add(s, EV_CLAP, at_ms, 200);
    e->amp = dbfs_to_amp(peak_dbfs);
    e->tau = (float)MS(15);
}

static void add_tail(scene_t *s, uint32_t at_ms, float start_dbfs, float tau_ms, uint32_t len_ms)
{
    ev_t *e = scene_add(s, EV_TAIL, at_ms, len_ms);
    e->amp = dbfs_to_amp(start_dbfs);
    e->tau = tau_ms * (float)FS / 1000.0f;
}

static float ev_value(const ev_t *e, uint32_t i)
{
    if (i < e->start || i >= e->start + e->len) {
        return 0.0f;
    }
    const uint32_t t = i - e->start;
    const float u = urand(e->seed, i);
    switch (e->kind) {
    case EV_NOISE:
        return e->amp * u;
    case EV_CLAP: {
        const uint32_t attack = MS(1);
        const float env = (t < attack) ? (float)(t + 1) / (float)attack
                                       : expf(-(float)(t - attack) / e->tau);
        return e->amp * env * u;
    }
    case EV_TAIL:
        return e->amp * expf(-(float)t / e->tau) * u;
    }
    return 0.0f;
}

static void render(const scene_t *s, uint32_t from, size_t n, int16_t *out)
{
    for (size_t k = 0; k < n; k++) {
        float v = 0.0f;
        for (int j = 0; j < s->n; j++) {
            v += ev_value(&s->ev[j], from + (uint32_t)k);
        }
        v = roundf(v);
        if (v > 32767.0f) {
            v = 32767.0f;
        } else if (v < -32768.0f) {
            v = -32768.0f;
        }
        out[k] = (int16_t)v;
    }
}

/* ------------------------------------------------------------------------ */
/* Test fixture                                                             */
/* ------------------------------------------------------------------------ */

static clap_config_t cfg;
static clap_detector_t *det;

void setUp(void)
{
    clap_config_default(&cfg);
    cfg.sample_rate_hz = FS;
    det = NULL;
}

void tearDown(void)
{
    clap_detector_destroy(det);
    det = NULL;
}

static void create_detector(void)
{
    det = clap_detector_create(&cfg);
    TEST_ASSERT_NOT_NULL_MESSAGE(det, "clap_detector_create returned NULL for a valid config");
}

/* Feed the whole scene in blocks of `block` samples. Returns the total number
 * of events reported; the first `max` of them are copied to out. */
static size_t run_scene(const scene_t *s, size_t block, clap_event_t *out, size_t max)
{
    int16_t pcm[1024];
    size_t total_events = 0;
    TEST_ASSERT_TRUE(block > 0 && block <= sizeof(pcm) / sizeof(pcm[0]));

    for (uint32_t pos = 0; pos < s->total;) {
        const size_t n = (s->total - pos < block) ? (s->total - pos) : block;
        render(s, pos, n, pcm);
        clap_event_t tmp[MAX_EVENTS];
        const size_t k = clap_detector_process(det, pcm, n, tmp, MAX_EVENTS);
        TEST_ASSERT_TRUE_MESSAGE(k <= MAX_EVENTS, "process returned more events than max_events");
        for (size_t j = 0; j < k; j++) {
            if (total_events < max) {
                out[total_events] = tmp[j];
            }
            total_events++;
        }
        pos += (uint32_t)n;
    }
    return total_events;
}

static size_t run(const scene_t *s, clap_event_t *out)
{
    return run_scene(s, 160, out, MAX_EVENTS);
}

static void assert_near_ms(uint32_t expected_ms, uint64_t actual_sample, uint32_t tol_ms, const char *what)
{
    TEST_ASSERT_INT32_WITHIN_MESSAGE((int32_t)MS(tol_ms), (int32_t)MS(expected_ms),
                                     (int32_t)actual_sample, what);
}

static void quiet_room(scene_t *s, uint32_t total_ms)
{
    scene_init(s, total_ms);
    add_noise(s, 0, total_ms, ROOM_DBFS);
}

/* ------------------------------------------------------------------------ */
/* Configuration                                                            */
/* ------------------------------------------------------------------------ */

static void test_defaults_match_requirements(void)
{
    clap_config_t d;
    memset(&d, 0xA5, sizeof(d));
    clap_config_default(&d);
    TEST_ASSERT_EQUAL_UINT32(16000, d.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT16(150, d.min_gap_ms);       /* CLAP-02 */
    TEST_ASSERT_EQUAL_UINT16(600, d.max_gap_ms);       /* CLAP-02 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 6.0f, d.max_level_diff_db);   /* CLAP-03 */
    TEST_ASSERT_EQUAL_UINT16(300, d.quiet_before_ms);  /* CLAP-04 */
    TEST_ASSERT_EQUAL_UINT16(400, d.quiet_after_ms);   /* CLAP-04 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.5f, d.quiet_gap_factor);    /* CLAP-04 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 12.0f, d.min_drop_db);        /* CLAP-05 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 12.0f, d.onset_threshold_db); /* CLAP-06 */
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -40.0f, d.min_peak_dbfs);     /* CLAP-13 */
}

static void test_create_accepts_defaults_and_rejects_invalid_config(void)
{
    create_detector();

    TEST_ASSERT_NULL(clap_detector_create(NULL));

    clap_config_t bad = cfg;
    bad.sample_rate_hz = 0;
    TEST_ASSERT_NULL_MESSAGE(clap_detector_create(&bad), "sample_rate_hz == 0");

    bad = cfg;
    bad.min_gap_ms = bad.max_gap_ms;
    TEST_ASSERT_NULL_MESSAGE(clap_detector_create(&bad), "min_gap_ms >= max_gap_ms");

    bad = cfg;
    bad.onset_threshold_db = 0.0f;
    TEST_ASSERT_NULL_MESSAGE(clap_detector_create(&bad), "onset_threshold_db <= 0");

    bad = cfg;
    bad.min_drop_db = -1.0f;
    TEST_ASSERT_NULL_MESSAGE(clap_detector_create(&bad), "min_drop_db <= 0");
}

static void test_counts_samples_and_reset_clears_counter(void)
{
    create_detector();
    int16_t zeros[1000] = {0};
    size_t left = 12345;
    while (left > 0) {
        const size_t n = left < 1000 ? left : 1000;
        TEST_ASSERT_EQUAL_size_t(0, clap_detector_process(det, zeros, n, NULL, 0));
        left -= n;
    }
    TEST_ASSERT_EQUAL_UINT32(12345, (uint32_t)clap_detector_samples_processed(det));

    clap_detector_reset(det);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)clap_detector_samples_processed(det));
}

/* ------------------------------------------------------------------------ */
/* Basic detection                                                          */
/* ------------------------------------------------------------------------ */

static void test_silence_never_triggers(void)
{
    create_detector();
    scene_t s;
    scene_init(&s, 5000);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

static void test_steady_loud_noise_never_triggers(void) /* CLAP-06 */
{
    create_detector();
    scene_t s;
    scene_init(&s, 10000);
    add_noise(&s, 0, 10000, SHOWER_DBFS);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

static void test_double_clap_reported_once_with_accurate_onsets(void) /* CLAP-02 */
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS);

    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));
    assert_near_ms(1500, ev[0].first_onset_sample, ONSET_TOL_MS, "first onset");
    assert_near_ms(1800, ev[0].second_onset_sample, ONSET_TOL_MS, "second onset");
}

static void test_confirmation_waits_for_quiet_after_window(void) /* CLAP-04, NFR-06 */
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS);

    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));

    const float gap_ms = (float)(ev[0].second_onset_sample - ev[0].first_onset_sample) * 1000.0f / (float)FS;
    float w_after_ms = cfg.quiet_gap_factor * gap_ms;
    if (w_after_ms < (float)cfg.quiet_after_ms) {
        w_after_ms = (float)cfg.quiet_after_ms;
    }
    const uint64_t earliest = ev[0].second_onset_sample + (uint64_t)(w_after_ms * (float)FS / 1000.0f);
    TEST_ASSERT_TRUE_MESSAGE(ev[0].confirmed_sample >= earliest, "confirmed before the quiet-after window ended");
    TEST_ASSERT_TRUE_MESSAGE(ev[0].confirmed_sample <= earliest + MS(CONFIRM_TOL_MS), "confirmation too late");
}

static void test_two_separate_double_claps_give_two_events(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 7000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS);
    add_clap(&s, 5000, CLAP_DBFS);
    add_clap(&s, 5300, CLAP_DBFS);

    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(2, run(&s, ev));
    assert_near_ms(1500, ev[0].first_onset_sample, ONSET_TOL_MS, "pair 1");
    assert_near_ms(5000, ev[1].first_onset_sample, ONSET_TOL_MS, "pair 2");
}

/* ------------------------------------------------------------------------ */
/* CLAP-02: gap                                                             */
/* ------------------------------------------------------------------------ */

static void test_gap_too_short_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1600, CLAP_DBFS); /* 100 ms < 150 ms */
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

static void test_gap_too_long_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 4000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 2300, CLAP_DBFS); /* 800 ms > 600 ms */
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

static void test_gaps_just_inside_limits_accepted(void)
{
    create_detector();
    clap_event_t ev[MAX_EVENTS];
    scene_t s;

    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1670, CLAP_DBFS); /* 170 ms */
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, run(&s, ev), "170 ms gap");

    clap_detector_reset(det);
    quiet_room(&s, 4000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 2070, CLAP_DBFS); /* 570 ms */
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, run(&s, ev), "570 ms gap");
}

/* ------------------------------------------------------------------------ */
/* CLAP-03: similar loudness                                                */
/* ------------------------------------------------------------------------ */

static void test_bounce_with_much_weaker_second_impact_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS - 10.0f);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

static void test_slightly_unequal_claps_accepted(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS - 3.0f);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));
}

/* ------------------------------------------------------------------------ */
/* CLAP-04: quiet before / after, scaled with the gap                       */
/* ------------------------------------------------------------------------ */

static void test_triple_clap_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 4000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1780, CLAP_DBFS);
    add_clap(&s, 2060, CLAP_DBFS);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

/* The knock is 400 ms before the pair: outside a fixed 300 ms window but
 * inside max(300, 1.5 * 300) = 450 ms. */
static void test_impact_shortly_before_pair_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 4000);
    add_clap(&s, 1100, CLAP_DBFS);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

static void test_walking_footsteps_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 6000);
    for (uint32_t t = 1500; t <= 4000; t += 500) {
        add_clap(&s, t, CLAP_DBFS);
    }
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

/* ------------------------------------------------------------------------ */
/* CLAP-05: level must settle between the claps                             */
/* ------------------------------------------------------------------------ */

/* Gap, loudness and quiet windows all pass; only the missing drop between
 * the impacts (rattle stays within about 8 dB of the first peak) rejects it. */
static void test_rattle_between_impacts_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 4000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_noise(&s, 1500, 1950, -24.0f); /* rattle that never settles */
    add_clap(&s, 1800, CLAP_DBFS + 2.0f);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

static void test_reverberant_double_clap_detected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 4000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_tail(&s, 1500, -26.0f, 150.0f, 1500);
    add_clap(&s, 1800, CLAP_DBFS);
    add_tail(&s, 1800, -26.0f, 150.0f, 1500);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));
}

static void test_early_reflections_do_not_add_onsets(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3500);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1525, CLAP_DBFS - 6.0f); /* tile reflection 25 ms later */
    add_clap(&s, 1800, CLAP_DBFS);
    add_clap(&s, 1825, CLAP_DBFS - 6.0f);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));
    assert_near_ms(1500, ev[0].first_onset_sample, ONSET_TOL_MS, "first onset");
    assert_near_ms(1800, ev[0].second_onset_sample, ONSET_TOL_MS, "second onset");
}

static void test_single_clap_with_reflection_rejected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1525, CLAP_DBFS - 6.0f);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

/* ------------------------------------------------------------------------ */
/* CLAP-06: adaptive background                                             */
/* ------------------------------------------------------------------------ */

static void test_double_clap_in_shower_noise_detected(void)
{
    create_detector();
    scene_t s;
    scene_init(&s, 3000);
    add_noise(&s, 0, 3000, SHOWER_DBFS);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));
}

static void test_shower_turning_on_does_not_trigger_and_claps_still_work(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 8000);
    add_noise(&s, 2000, 8000, SHOWER_DBFS); /* shower starts at 2 s */
    add_clap(&s, 6000, CLAP_DBFS);
    add_clap(&s, 6300, CLAP_DBFS);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));
    assert_near_ms(6000, ev[0].first_onset_sample, ONSET_TOL_MS, "first onset");
}

/* ------------------------------------------------------------------------ */
/* CLAP-13: absolute level                                                  */
/* ------------------------------------------------------------------------ */

static void test_quiet_double_impact_rejected(void)
{
    create_detector();
    scene_t s;
    scene_init(&s, 3000);
    add_noise(&s, 0, 3000, -85.0f);
    add_clap(&s, 1500, -55.0f); /* 30 dB above background, but too quiet */
    add_clap(&s, 1800, -55.0f);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(0, run(&s, ev));
}

/* ------------------------------------------------------------------------ */
/* Robustness                                                               */
/* ------------------------------------------------------------------------ */

static void test_clipped_full_scale_claps_detected(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, 0.0f);
    add_clap(&s, 1800, 0.0f);
    clap_event_t ev[MAX_EVENTS];
    TEST_ASSERT_EQUAL_size_t(1, run(&s, ev));
}

static void test_result_independent_of_block_size(void)
{
    scene_t s;
    scene_init(&s, 7000);
    add_noise(&s, 0, 7000, SHOWER_DBFS);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS);
    add_clap(&s, 5000, CLAP_DBFS);
    add_clap(&s, 5300, CLAP_DBFS);

    clap_event_t ref[MAX_EVENTS];
    create_detector();
    const size_t n_ref = run_scene(&s, 160, ref, MAX_EVENTS);
    TEST_ASSERT_EQUAL_size_t(2, n_ref);

    const size_t blocks[] = {1, 7, 1000};
    for (size_t b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
        clap_detector_reset(det);
        clap_event_t ev[MAX_EVENTS];
        const size_t n = run_scene(&s, blocks[b], ev, MAX_EVENTS);
        TEST_ASSERT_EQUAL_size_t(n_ref, n);
        for (size_t i = 0; i < n; i++) {
            TEST_ASSERT_EQUAL_UINT32((uint32_t)ref[i].first_onset_sample, (uint32_t)ev[i].first_onset_sample);
            TEST_ASSERT_EQUAL_UINT32((uint32_t)ref[i].second_onset_sample, (uint32_t)ev[i].second_onset_sample);
            TEST_ASSERT_EQUAL_UINT32((uint32_t)ref[i].confirmed_sample, (uint32_t)ev[i].confirmed_sample);
        }
    }
}

static void test_zero_capacity_event_buffer_is_safe(void)
{
    create_detector();
    scene_t s;
    quiet_room(&s, 3000);
    add_clap(&s, 1500, CLAP_DBFS);
    add_clap(&s, 1800, CLAP_DBFS);

    int16_t pcm[160];
    for (uint32_t pos = 0; pos < s.total; pos += 160) {
        render(&s, pos, 160, pcm);
        TEST_ASSERT_EQUAL_size_t(0, clap_detector_process(det, pcm, 160, NULL, 0));
    }
    TEST_ASSERT_EQUAL_UINT32(s.total, (uint32_t)clap_detector_samples_processed(det));
}

/* ------------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_match_requirements);
    RUN_TEST(test_create_accepts_defaults_and_rejects_invalid_config);
    RUN_TEST(test_counts_samples_and_reset_clears_counter);
    RUN_TEST(test_silence_never_triggers);
    RUN_TEST(test_steady_loud_noise_never_triggers);
    RUN_TEST(test_double_clap_reported_once_with_accurate_onsets);
    RUN_TEST(test_confirmation_waits_for_quiet_after_window);
    RUN_TEST(test_two_separate_double_claps_give_two_events);
    RUN_TEST(test_gap_too_short_rejected);
    RUN_TEST(test_gap_too_long_rejected);
    RUN_TEST(test_gaps_just_inside_limits_accepted);
    RUN_TEST(test_bounce_with_much_weaker_second_impact_rejected);
    RUN_TEST(test_slightly_unequal_claps_accepted);
    RUN_TEST(test_triple_clap_rejected);
    RUN_TEST(test_impact_shortly_before_pair_rejected);
    RUN_TEST(test_walking_footsteps_rejected);
    RUN_TEST(test_rattle_between_impacts_rejected);
    RUN_TEST(test_reverberant_double_clap_detected);
    RUN_TEST(test_early_reflections_do_not_add_onsets);
    RUN_TEST(test_single_clap_with_reflection_rejected);
    RUN_TEST(test_double_clap_in_shower_noise_detected);
    RUN_TEST(test_shower_turning_on_does_not_trigger_and_claps_still_work);
    RUN_TEST(test_quiet_double_impact_rejected);
    RUN_TEST(test_clipped_full_scale_claps_detected);
    RUN_TEST(test_result_independent_of_block_size);
    RUN_TEST(test_zero_capacity_event_buffer_is_safe);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
