/*
 * voice_mix – a mono voice clip mixed over stereo music, with the music
 * ducked while the voice speaks (pure C).
 *
 * Requirements: COLD-10, COLD-11 (see the requirements doc).
 * Tests:        test/test_voice_mix/
 *
 * The player task runs every block of decoded music through
 * voice_mix_process() just before the volume stage. While no clip is active
 * and the duck gain is back at 1, the block passes through bit-exact.
 *
 * Clip source
 *   The clip is pulled through a callback: read(ctx, dst, max) writes up to
 *   max mono samples to dst and returns how many it wrote; 0 means the clip
 *   is over. The mixer never asks for more than VOICE_MIX_READ_MAX samples at
 *   once and never calls read again after it returned 0.
 *
 * Resampling
 *   The clip plays at its own speed whatever the output rate is: a clip of N
 *   samples at clip_rate_hz covers N * out_rate_hz / clip_rate_hz output
 *   frames (+-1). Between two clip samples the value is interpolated
 *   linearly; with equal rates the clip samples come out unchanged. The first
 *   output frame after voice_mix_start() carries the first clip sample.
 *
 * Mixing
 *   out = saturate(round(music * duck) + voice * voice_gain), per channel;
 *   the voice goes to left and right equally. Saturation clamps to
 *   -32768 ... 32767.
 *
 * Ducking
 *   duck is a linear amplitude factor. It is 1 at rest. From the frame where
 *   a clip starts it falls linearly to 10^(-duck_db/20) within duck_ramp_ms;
 *   from the frame after the last clip sample (or after voice_mix_stop()) it
 *   rises linearly back to 1 at the same rate (dB per ms is not the unit: the
 *   ramp is linear in amplitude and takes duck_ramp_ms for the full swing).
 *   A clip that starts during the rise turns the ramp around from the current
 *   value. duck_ramp_ms 0 switches at once. The gain never leaves the range
 *   [10^(-duck_db/20), 1] and moves monotonically within one ramp.
 *
 * voice_mix_active() is true from voice_mix_start() until the last clip
 * sample has been output or voice_mix_stop() was called. The rise of the
 * duck gain afterwards does not count as active.
 */
#ifndef VOICE_MIX_H
#define VOICE_MIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VOICE_MIX_READ_MAX 256

typedef struct {
    uint32_t out_rate_hz;  /* 8000 ... 48000                        default 48000 */
    float duck_db;         /* 0 ... 40, attenuation of the music    default 12.0  */
    uint16_t duck_ramp_ms; /* 0 ... 1000                            default 60    */
    float voice_gain_db;   /* -20 ... +12                           default 0.0   */
} voice_mix_config_t;

typedef size_t (*voice_mix_read_fn)(void *ctx, int16_t *dst, size_t max_samples);

typedef struct voice_mix voice_mix_t;

void voice_mix_config_default(voice_mix_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is outside the ranges above. */
voice_mix_t *voice_mix_create(const voice_mix_config_t *cfg);

/* Accepts NULL. */
void voice_mix_destroy(voice_mix_t *vm);

/* The music changed its sample rate (next track). Takes effect with the next
 * processed frame; a running clip keeps its position. Returns false and
 * changes nothing if the rate is outside 8000 ... 48000. */
bool voice_mix_set_out_rate(voice_mix_t *vm, uint32_t out_rate_hz);

/* Start a clip. A clip that is still playing is replaced (its read callback
 * is not called again). Returns false and changes nothing if read is NULL or
 * clip_rate_hz is outside 8000 ... 48000. */
bool voice_mix_start(voice_mix_t *vm, uint32_t clip_rate_hz, voice_mix_read_fn read, void *ctx);

/* Silence the voice at once; the music comes back up. No effect when idle. */
void voice_mix_stop(voice_mix_t *vm);

bool voice_mix_active(const voice_mix_t *vm);

/* Mix in place. stereo holds frames * 2 samples (L, R, L, R, ...). */
void voice_mix_process(voice_mix_t *vm, int16_t *stereo, size_t frames);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_MIX_H */
