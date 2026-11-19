/*
 * mp3_decoder – a thin wrapper around minimp3 (third_party/minimp3, CC0), so
 * the rest of the firmware does not see its macros.
 *
 * mp3_decoder_decode() decodes at most one frame from buf. info.frame_bytes
 * is how many bytes to drop from the front of buf afterwards:
 *   frame_bytes > 0, samples > 0   a frame was decoded
 *   frame_bytes > 0, samples == 0  junk or a tag was skipped
 *   frame_bytes == 0               more data needed (or none left)
 * samples is per channel; pcm holds samples * channels interleaved values.
 */
#ifndef MP3_DECODER_H
#define MP3_DECODER_H

#include <stddef.h>
#include <stdint.h>

#define MP3_DECODER_MAX_SAMPLES (1152 * 2) /* per frame, all channels */
#define MP3_DECODER_STATE_BYTES 6800       /* >= sizeof(mp3dec_t) */

typedef struct {
    int frame_bytes;
    int channels;
    int hz;
    int bitrate_kbps;
} mp3_frame_info_t;

typedef struct {
    _Alignas(8) uint8_t state[MP3_DECODER_STATE_BYTES];
} mp3_decoder_t;

void mp3_decoder_init(mp3_decoder_t *d);
void mp3_decoder_reset(mp3_decoder_t *d);
int mp3_decoder_decode(mp3_decoder_t *d, const uint8_t *buf, size_t len, int16_t *pcm, mp3_frame_info_t *info);

#endif /* MP3_DECODER_H */
