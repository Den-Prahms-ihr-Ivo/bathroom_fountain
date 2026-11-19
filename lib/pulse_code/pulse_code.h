/*
 * pulse_code – the laser shot as a coded burst of light pulses, so that
 * sunlight, a lamp or a camera flash never counts as a hit (pure C).
 *
 * Requirements: GAME-20 ... GAME-23 (see the requirements doc).
 * Tests:        test/test_pulse_code/
 *
 * The gun never emits a continuous beam: one trigger pull is one burst of
 * about 20 ms. The burst carries the gun's id and a shot counter. The gun
 * turns the list from pulse_code_encode() into laser on/off times (RMT
 * transmit); the target feeds every light edge it sees into a decoder (GPIO
 * interrupt or RMT receive with a timestamp).
 *
 * Burst, with T = PULSE_CODE_UNIT_US (500 us)
 *   start   light on for 3 T, off for 1 T
 *   12 bits, most significant first; each bit is light on for 2 T (1) or
 *   1 T (0), followed by 1 T off. The off time after the last bit is not
 *   part of the list (the laser simply stays off).
 *   The 12 bits are  gun_id (4)  shot (4)  check (4)  with
 *   check = (gun_id XOR shot XOR 0xA) AND 0xF.
 *   So there are 13 on-pulses and the list has 25 segments, starting and
 *   ending with "on".
 *
 * Decoder
 *   pulse_decoder_edge(on, t_us) is called for every change of the light
 *   level with a microsecond timestamp (wrapping uint32). The decoder
 *   measures the time between edges:
 *     on  for 1250 ... 1900 us   start mark, begins a new burst (also in the
 *                                middle of one)
 *     on  for  250 ...  750 us   bit 0
 *     on  for  751 ... 1249 us   bit 1
 *     off for  250 ...  900 us   gap inside a burst
 *   Anything else (a shorter or longer pulse, a longer gap before the burst
 *   is complete, a bit without a start mark) throws the burst away. Two
 *   edges with the same level in a row are treated as a disturbance and
 *   also throw it away.
 *   When the falling edge of the twelfth bit arrives and the check matches,
 *   the call returns true and *out holds gun id and shot. A wrong check
 *   returns false like any other noise.
 */
#ifndef PULSE_CODE_H
#define PULSE_CODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PULSE_CODE_UNIT_US 500
#define PULSE_CODE_SEGMENTS 25

typedef struct {
    bool on;
    uint16_t us;
} pulse_seg_t;

typedef struct {
    uint8_t gun_id; /* 0 ... 15 */
    uint8_t shot;   /* 0 ... 15 */
} pulse_shot_t;

typedef struct {
    uint32_t last_us;
    uint16_t bits;
    uint8_t count;   /* bits received in this burst */
    bool started;    /* a start mark was seen */
    bool level;      /* light level after the last edge */
    bool have_edge;
} pulse_decoder_t;

/* Write the burst into segs. Returns PULSE_CODE_SEGMENTS, or 0 if segs is
 * NULL, max is too small, or gun_id or shot is above 15. */
size_t pulse_code_encode(uint8_t gun_id, uint8_t shot, pulse_seg_t *segs, size_t max);

/* Start state: light off, nothing received. */
void pulse_decoder_init(pulse_decoder_t *dec);

bool pulse_decoder_edge(pulse_decoder_t *dec, bool on, uint32_t t_us, pulse_shot_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PULSE_CODE_H */
