/*
 * pulse_code – the laser shot as a coded burst of light pulses.
 * Contract in pulse_code.h, tests in test/test_pulse_code/.
 */
#include "pulse_code.h"

#include <string.h>

#define T PULSE_CODE_UNIT_US
#define BITS 12

#define START_MIN 1250u
#define START_MAX 1900u
#define ZERO_MIN 250u
#define ZERO_MAX 750u
#define ONE_MIN 751u
#define ONE_MAX 1249u
#define GAP_MIN 250u
#define GAP_MAX 900u

static uint8_t check_of(uint8_t gun_id, uint8_t shot)
{
    return (uint8_t)((gun_id ^ shot ^ 0xAu) & 0xFu);
}

size_t pulse_code_encode(uint8_t gun_id, uint8_t shot, pulse_seg_t *segs, size_t max)
{
    if (segs == NULL || max < PULSE_CODE_SEGMENTS || gun_id > 15 || shot > 15) {
        return 0;
    }
    const uint16_t word = (uint16_t)((gun_id << 8) | (shot << 4) | check_of(gun_id, shot));
    size_t n = 0;
    segs[n].on = true;
    segs[n++].us = 3 * T;
    for (int b = BITS - 1; b >= 0; b--) {
        segs[n].on = false;
        segs[n++].us = T;
        segs[n].on = true;
        segs[n++].us = ((word >> b) & 1u) ? 2 * T : T;
    }
    return n;
}

void pulse_decoder_init(pulse_decoder_t *dec)
{
    if (dec != NULL) {
        memset(dec, 0, sizeof(*dec));
        dec->level = false;
    }
}

static void drop_burst(pulse_decoder_t *dec)
{
    dec->started = false;
    dec->count = 0;
    dec->bits = 0;
}

bool pulse_decoder_edge(pulse_decoder_t *dec, bool on, uint32_t t_us, pulse_shot_t *out)
{
    if (dec == NULL) {
        return false;
    }
    const uint32_t dt = t_us - dec->last_us;
    const bool had_edge = dec->have_edge;
    dec->last_us = t_us;
    dec->have_edge = true;

    if (on == dec->level) {
        drop_burst(dec); /* two edges with the same level: disturbance */
        return false;
    }
    dec->level = on;

    if (on) {
        /* rising edge: dt was dark time */
        if (dec->started && had_edge && (dt < GAP_MIN || dt > GAP_MAX)) {
            drop_burst(dec);
        }
        return false;
    }

    /* falling edge: dt was the length of the light pulse */
    if (!had_edge) {
        drop_burst(dec);
        return false;
    }
    if (dt >= START_MIN && dt <= START_MAX) {
        dec->started = true;
        dec->count = 0;
        dec->bits = 0;
        return false;
    }
    int bit;
    if (dt >= ZERO_MIN && dt <= ZERO_MAX) {
        bit = 0;
    } else if (dt >= ONE_MIN && dt <= ONE_MAX) {
        bit = 1;
    } else {
        drop_burst(dec);
        return false;
    }
    if (!dec->started) {
        return false;
    }
    dec->bits = (uint16_t)((dec->bits << 1) | (uint16_t)bit);
    dec->count++;
    if (dec->count < BITS) {
        return false;
    }
    const uint8_t gun_id = (uint8_t)((dec->bits >> 8) & 0xF);
    const uint8_t shot = (uint8_t)((dec->bits >> 4) & 0xF);
    const uint8_t check = (uint8_t)(dec->bits & 0xF);
    drop_burst(dec);
    if (check != check_of(gun_id, shot)) {
        return false;
    }
    if (out != NULL) {
        out->gun_id = gun_id;
        out->shot = shot;
    }
    return true;
}
