/*
 * wav_clip – read the header of a voice clip (pure C, no file access).
 *
 * Requirements: COLD-09, COLD-12 (see the requirements doc).
 * Tests:        test/test_wav_clip/
 *
 * The voice prompts are 16-bit mono PCM WAV files. The player needs to know
 * where the samples start and how long the clip is, so the cold-shower
 * protocol can start each "3-2-1" clip exactly its own length before a phase
 * change. The caller reads the first bytes of the file (512 are enough for
 * files written by ffmpeg, Audacity and macOS afconvert) and passes them in
 * together with the file size.
 *
 * Accepted files
 *   "RIFF" <size> "WAVE", then chunks. Each chunk is a 4-character id, a
 *   little-endian uint32 size and that many bytes, followed by one pad byte
 *   if the size is odd. Chunks other than "fmt " and "data" are skipped
 *   ("LIST", "fact", ...). The RIFF size field is ignored.
 *   "fmt "   at least 16 bytes: format tag 1 (PCM), 1 channel, 16 bits per
 *            sample, sample rate 8000 ... 48000 Hz. Must come before "data".
 *   "data"   the samples. Its size field is clamped to the bytes that are
 *            really in the file (file_size - data_offset): tools that stream
 *            their output write 0 or 0xFFFFFFFF there. A value of 0 means
 *            "to the end of the file".
 *
 * Results
 *   WAV_CLIP_OK             out is filled
 *   WAV_CLIP_ERR_TRUNCATED  head ends before the "data" chunk header is
 *                           complete: read more bytes and call again
 *   WAV_CLIP_ERR_NOT_WAV    no RIFF/WAVE signature (checked first; fewer
 *                           than 12 bytes is TRUNCATED)
 *   WAV_CLIP_ERR_FORMAT     "data" before "fmt ", "fmt " shorter than 16
 *                           bytes, or not 16-bit mono PCM at 8000-48000 Hz
 *   WAV_CLIP_ERR_EMPTY      no complete sample in the file
 *   NULL head or out is WAV_CLIP_ERR_TRUNCATED / no write.
 * out is only written on WAV_CLIP_OK.
 */
#ifndef WAV_CLIP_H
#define WAV_CLIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t data_offset; /* byte offset of the first sample in the file      */
    uint32_t frames;      /* number of samples (a trailing odd byte is dropped) */
    uint32_t duration_ms; /* frames / sample_rate_hz, rounded up              */
} wav_clip_info_t;

typedef enum {
    WAV_CLIP_OK = 0,
    WAV_CLIP_ERR_TRUNCATED,
    WAV_CLIP_ERR_NOT_WAV,
    WAV_CLIP_ERR_FORMAT,
    WAV_CLIP_ERR_EMPTY,
} wav_clip_result_t;

wav_clip_result_t wav_clip_parse(const uint8_t *head, size_t len, uint32_t file_size, wav_clip_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* WAV_CLIP_H */
