/*
 * mp3_decoder – see mp3_decoder.h.
 */
#include "mp3_decoder.h"

#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "third_party/minimp3/minimp3.h"
#pragma GCC diagnostic pop

_Static_assert(sizeof(mp3dec_t) <= MP3_DECODER_STATE_BYTES, "raise MP3_DECODER_STATE_BYTES");

void mp3_decoder_init(mp3_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
    mp3dec_init((mp3dec_t *)d->state);
}

void mp3_decoder_reset(mp3_decoder_t *d)
{
    mp3dec_init((mp3dec_t *)d->state);
}

int mp3_decoder_decode(mp3_decoder_t *d, const uint8_t *buf, size_t len, int16_t *pcm, mp3_frame_info_t *info)
{
    mp3dec_frame_info_t fi;
    memset(&fi, 0, sizeof(fi));
    const int samples = mp3dec_decode_frame((mp3dec_t *)d->state, buf, (int)len, pcm, &fi);
    info->frame_bytes = fi.frame_bytes;
    info->channels = fi.channels;
    info->hz = fi.hz;
    info->bitrate_kbps = fi.bitrate_kbps;
    return samples;
}
