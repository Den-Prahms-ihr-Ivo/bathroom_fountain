/*
 * pcm_mix – decoder output to the I2S frames for the amplifier.
 *
 * The contract is in pcm_mix.h, the tests in test/test_pcm_mix/.
 */
#include "pcm_mix.h"

#include <stdbool.h>

size_t pcm_mix_to_output(const int16_t *in, size_t frames, unsigned channels, pcm_right_t right, int16_t *out)
{
    if ((channels != 1 && channels != 2) || (right != PCM_RIGHT_SILENT && right != PCM_RIGHT_COPY)) {
        return 0;
    }
    if (frames == 0 || in == NULL || out == NULL) {
        return 0;
    }

    const bool copy = (right == PCM_RIGHT_COPY);
    for (size_t i = 0; i < frames; i++) {
        /* Both input samples are read before frame i is written, so stereo
         * in-place (out == in) is safe: frame i only overwrites itself. */
        int16_t l;
        if (channels == 1) {
            l = in[i];
        } else {
            l = (int16_t)(((int32_t)in[2 * i] + (int32_t)in[2 * i + 1]) / 2); /* truncates toward zero */
        }
        out[2 * i] = l;
        out[2 * i + 1] = copy ? l : 0;
    }
    return frames;
}
