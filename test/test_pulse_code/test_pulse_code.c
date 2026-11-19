/*
 * Tests for lib/pulse_code – requirements GAME-20 ... GAME-23.
 * The helper plays a segment list into the decoder as edges.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "pulse_code.h"

static pulse_seg_t segs[PULSE_CODE_SEGMENTS + 4];
static pulse_decoder_t dec;
static pulse_shot_t shot;
static uint32_t t;
static int hits;
static uint32_t rng;

void setUp(void)
{
    memset(segs, 0, sizeof(segs));
    memset(&dec, 0xEE, sizeof(dec));
    pulse_decoder_init(&dec);
    memset(&shot, 0xEE, sizeof(shot));
    t = 1000000u;
    hits = 0;
    rng = 99u;
}

void tearDown(void) {}

static void edge(bool on)
{
    if (pulse_decoder_edge(&dec, on, t, &shot)) {
        hits++;
    }
}

/* Light on for `on_us`, then off; the off edge is sent, time stops there. */
static void pulse(uint32_t on_us)
{
    edge(true);
    t += on_us;
    edge(false);
}

static void gap(uint32_t us)
{
    t += us;
}

/* Play a segment list; scale in percent stretches every duration. */
static void play(const pulse_seg_t *s, size_t n, uint32_t scale)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i].on) {
            edge(true);
        }
        t += (uint32_t)s[i].us * scale / 100u;
        if (s[i].on) {
            edge(false);
        }
    }
}

static void fire(uint8_t id, uint8_t n, uint32_t scale)
{
    TEST_ASSERT_EQUAL_size_t_MESSAGE(PULSE_CODE_SEGMENTS, pulse_code_encode(id, n, segs, PULSE_CODE_SEGMENTS),
                                     "encode failed for a valid shot");
    play(segs, PULSE_CODE_SEGMENTS, scale);
}

static void test_encode_layout(void)
{
    TEST_ASSERT_EQUAL_size_t(25, pulse_code_encode(0x5, 0xC, segs, PULSE_CODE_SEGMENTS + 4));
    /* bits: 0101 1100 0011  (check = 5 ^ C ^ A = 3) */
    const int bits[12] = {0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1};
    TEST_ASSERT_TRUE(segs[0].on);
    TEST_ASSERT_EQUAL_UINT16(1500, segs[0].us);
    for (int b = 0; b < 12; b++) {
        TEST_ASSERT_FALSE(segs[1 + 2 * b].on);
        TEST_ASSERT_EQUAL_UINT16(500, segs[1 + 2 * b].us);
        TEST_ASSERT_TRUE(segs[2 + 2 * b].on);
        TEST_ASSERT_EQUAL_UINT16(bits[b] ? 1000 : 500, segs[2 + 2 * b].us);
    }
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, segs[25].us, "nothing written past the burst");
    uint32_t total = 0;
    for (int i = 0; i < 25; i++) {
        total += segs[i].us;
    }
    TEST_ASSERT_TRUE_MESSAGE(total <= 21000, "a burst is short: at most 21 ms");
}

static void test_encode_errors(void)
{
    TEST_ASSERT_EQUAL_size_t(25, pulse_code_encode(15, 15, segs, PULSE_CODE_SEGMENTS));
    TEST_ASSERT_EQUAL_size_t(0, pulse_code_encode(16, 0, segs, PULSE_CODE_SEGMENTS));
    TEST_ASSERT_EQUAL_size_t(0, pulse_code_encode(0, 16, segs, PULSE_CODE_SEGMENTS));
    TEST_ASSERT_EQUAL_size_t(0, pulse_code_encode(1, 1, NULL, PULSE_CODE_SEGMENTS));
    TEST_ASSERT_EQUAL_size_t(0, pulse_code_encode(1, 1, segs, PULSE_CODE_SEGMENTS - 1));
}

static void test_every_shot_decodes(void)
{
    for (uint8_t id = 0; id < 16; id++) {
        for (uint8_t n = 0; n < 16; n++) {
            hits = 0;
            fire(id, n, 100);
            TEST_ASSERT_EQUAL_INT(1, hits);
            TEST_ASSERT_EQUAL_UINT8(id, shot.gun_id);
            TEST_ASSERT_EQUAL_UINT8(n, shot.shot);
            gap(40000); /* next trigger pull */
        }
    }
}

static void test_timing_tolerance(void)
{
    fire(7, 9, 85); /* everything 15 % short */
    TEST_ASSERT_EQUAL_INT(1, hits);
    gap(30000);
    fire(7, 10, 118); /* everything 18 % long */
    TEST_ASSERT_EQUAL_INT(2, hits);
    TEST_ASSERT_EQUAL_UINT8(10, shot.shot);
    gap(30000);
    /* jitter of +-100 us on every edge */
    pulse_code_encode(3, 4, segs, PULSE_CODE_SEGMENTS);
    for (int i = 0; i < PULSE_CODE_SEGMENTS; i++) {
        rng = rng * 1664525u + 1013904223u;
        const int jitter = (int)((rng >> 16) % 201u) - 100;
        if (segs[i].on) {
            edge(true);
        }
        t += (uint32_t)((int)segs[i].us + jitter);
        if (segs[i].on) {
            edge(false);
        }
    }
    TEST_ASSERT_EQUAL_INT(3, hits);
    TEST_ASSERT_EQUAL_UINT8(3, shot.gun_id);
    TEST_ASSERT_EQUAL_UINT8(4, shot.shot);
}

static void test_boundaries_of_the_pulse_classes(void)
{
    /* build bursts by hand: start mark, then twelve bits of the given width; all-0 and all-1 have check 0xA/... so use id 0 shot 0: bits 0000 0000 1010 */
    const int bits[12] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0};
    const uint32_t zero_w[] = {250, 750};
    const uint32_t one_w[] = {751, 1249};
    const uint32_t start_w[] = {1250, 1900};
    const uint32_t gap_w[] = {250, 900};
    for (int v = 0; v < 2; v++) {
        hits = 0;
        pulse(start_w[v]);
        for (int b = 0; b < 12; b++) {
            gap(gap_w[v]);
            pulse(bits[b] ? one_w[v] : zero_w[v]);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, "limits are inclusive");
        TEST_ASSERT_EQUAL_UINT8(0, shot.gun_id);
        TEST_ASSERT_EQUAL_UINT8(0, shot.shot);
        gap(50000);
    }
    /* one pulse just outside a class spoils the burst */
    const uint32_t bad_pulse[] = {249, 1901};
    for (int v = 0; v < 2; v++) {
        hits = 0;
        pulse(1500);
        for (int b = 0; b < 12; b++) {
            gap(500);
            pulse(b == 5 ? bad_pulse[v] : (bits[b] ? 1000u : 500u));
        }
        TEST_ASSERT_EQUAL_INT(0, hits);
        gap(50000);
    }
    /* a gap that is too long or too short spoils it too */
    const uint32_t bad_gap[] = {249, 901};
    for (int v = 0; v < 2; v++) {
        hits = 0;
        pulse(1500);
        for (int b = 0; b < 12; b++) {
            gap(b == 7 ? bad_gap[v] : 500u);
            pulse(bits[b] ? 1000u : 500u);
        }
        TEST_ASSERT_EQUAL_INT(0, hits);
        gap(50000);
    }
    fire(1, 2, 100); /* and the decoder still works afterwards */
    TEST_ASSERT_EQUAL_INT(1, hits);
}

static void test_wrong_check_is_not_a_hit(void)
{
    pulse_code_encode(6, 3, segs, PULSE_CODE_SEGMENTS);
    for (int flip = 0; flip < 12; flip++) {
        pulse_seg_t bad[PULSE_CODE_SEGMENTS];
        memcpy(bad, segs, sizeof(bad));
        bad[2 + 2 * flip].us = bad[2 + 2 * flip].us == 500 ? 1000 : 500; /* one bit flipped */
        hits = 0;
        play(bad, PULSE_CODE_SEGMENTS, 100);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, hits, "a single wrong bit must be caught");
        gap(40000);
    }
    play(segs, PULSE_CODE_SEGMENTS, 100);
    TEST_ASSERT_EQUAL_INT(1, hits);
}

static void test_other_light_is_ignored(void)
{
    fire(2, 2, 100);
    TEST_ASSERT_EQUAL_INT(1, hits); /* the decoder works at all */
    hits = 0;
    gap(100000);
    pulse(3000000); /* sunlight through the blinds: one long pulse */
    gap(200000);
    for (int i = 0; i < 200; i++) { /* a 100 Hz lamp */
        pulse(5000);
        gap(5000);
    }
    for (int i = 0; i < 50; i++) { /* a camera flash train */
        pulse(120);
        gap(2000);
    }
    for (int i = 0; i < 3000; i++) { /* a PWM-dimmed LED lamp at 1 kHz: bit-sized pulses but no start mark */
        pulse(500);
        gap(500);
    }
    for (int i = 0; i < 2000; i++) { /* random flicker */
        rng = rng * 1664525u + 1013904223u;
        pulse(100 + (rng >> 8) % 2500u);
        rng = rng * 1664525u + 1013904223u;
        gap(100 + (rng >> 8) % 2500u);
    }
    TEST_ASSERT_EQUAL_INT(0, hits);
    gap(20000);
    fire(2, 3, 100);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, hits, "a real shot afterwards still counts");
}

static void test_a_new_start_mark_restarts_the_burst(void)
{
    pulse_code_encode(9, 1, segs, PULSE_CODE_SEGMENTS);
    play(segs, 11, 100); /* start + five bits, then the beam left the target */
    gap(500);
    fire(9, 2, 100);     /* the next burst begins with its start mark */
    TEST_ASSERT_EQUAL_INT(1, hits);
    TEST_ASSERT_EQUAL_UINT8(9, shot.gun_id);
    TEST_ASSERT_EQUAL_UINT8(2, shot.shot);
    /* bits without a start mark are nothing */
    gap(40000);
    hits = 0;
    play(segs + 2, PULSE_CODE_SEGMENTS - 2, 100);
    TEST_ASSERT_EQUAL_INT(0, hits);
}

static void test_repeated_edges_are_a_disturbance(void)
{
    pulse_code_encode(4, 4, segs, PULSE_CODE_SEGMENTS);
    play(segs, 9, 100);
    edge(false); /* a second falling edge without a rising one */
    gap(500);
    play(segs + 9, PULSE_CODE_SEGMENTS - 9, 100);
    TEST_ASSERT_EQUAL_INT(0, hits);
    gap(40000);
    fire(4, 5, 100);
    TEST_ASSERT_EQUAL_INT(1, hits);
}

static void test_timestamp_wrap_and_null_arguments(void)
{
    t = 0xFFFFFFFFu - 9000u; /* the counter wraps in the middle of the burst */
    fire(12, 13, 100);
    TEST_ASSERT_EQUAL_INT(1, hits);
    TEST_ASSERT_EQUAL_UINT8(12, shot.gun_id);
    TEST_ASSERT_EQUAL_UINT8(13, shot.shot);

    TEST_ASSERT_FALSE(pulse_decoder_edge(NULL, true, t, &shot));
    pulse_decoder_init(NULL);
    /* out may be NULL: the return value still says "hit" */
    gap(40000);
    pulse_code_encode(1, 1, segs, PULSE_CODE_SEGMENTS);
    bool hit = false;
    for (size_t i = 0; i < PULSE_CODE_SEGMENTS; i++) {
        if (segs[i].on) {
            pulse_decoder_edge(&dec, true, t, NULL);
        }
        t += segs[i].us;
        if (segs[i].on) {
            hit = pulse_decoder_edge(&dec, false, t, NULL) || hit;
        }
    }
    TEST_ASSERT_TRUE(hit);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_layout);
    RUN_TEST(test_encode_errors);
    RUN_TEST(test_every_shot_decodes);
    RUN_TEST(test_timing_tolerance);
    RUN_TEST(test_boundaries_of_the_pulse_classes);
    RUN_TEST(test_wrong_check_is_not_a_hit);
    RUN_TEST(test_other_light_is_ignored);
    RUN_TEST(test_a_new_start_mark_restarts_the_burst);
    RUN_TEST(test_repeated_edges_are_a_disturbance);
    RUN_TEST(test_timestamp_wrap_and_null_arguments);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
