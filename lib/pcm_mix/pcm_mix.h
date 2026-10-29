/*
 * pcm_mix – decoder output to the I2S frames for the amplifier (pure C).
 *
 * Requirements: AUD-01 (mono into one speaker on the left output).
 * Tests:        test/test_pcm_mix/
 *
 * The MP3 decoder delivers interleaved int16 PCM with 1 or 2 channels. The
 * amplifier always takes stereo I2S frames {L, R}; the speaker is on L.
 *
 *   L = in[i]                              for 1 channel
 *   L = (in[2i] + in[2i+1]) / 2            for 2 channels, computed in 32 bit,
 *                                          truncating toward zero (C division),
 *                                          so there is no overflow and no DC bias
 *   R = 0  (PCM_RIGHT_SILENT)  or  R = L  (PCM_RIGHT_COPY)
 *
 * PCM_RIGHT_SILENT is the default for this build: nothing is connected to the
 * right output, and a speaker wired there by mistake stays silent instead of
 * hiding the mistake.
 *
 * Buffers: out holds frames * 2 samples. out may be the same buffer as in
 * when channels == 2 (in-place); otherwise they must not overlap.
 */
#ifndef PCM_MIX_H
#define PCM_MIX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PCM_RIGHT_SILENT = 0,
    PCM_RIGHT_COPY = 1,
} pcm_right_t;

/* Returns frames, or 0 (out untouched) if channels is not 1 or 2 or right is
 * not a pcm_right_t value. in and out may be NULL only if frames is 0. */
size_t pcm_mix_to_output(const int16_t *in, size_t frames, unsigned channels, pcm_right_t right, int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PCM_MIX_H */
