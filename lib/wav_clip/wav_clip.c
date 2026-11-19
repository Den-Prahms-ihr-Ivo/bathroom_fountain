/*
 * wav_clip – read the header of a voice clip.
 * Contract in wav_clip.h, tests in test/test_wav_clip/.
 */
#include "wav_clip.h"

#include <stdbool.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

wav_clip_result_t wav_clip_parse(const uint8_t *head, size_t len, uint32_t file_size, wav_clip_info_t *out)
{
    if (head == NULL || out == NULL || len < 12) {
        return WAV_CLIP_ERR_TRUNCATED;
    }
    if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0) {
        return WAV_CLIP_ERR_NOT_WAV;
    }
    bool have_fmt = false;
    uint32_t rate = 0;
    size_t pos = 12;
    for (;;) {
        if (len < pos || len - pos < 8) {
            return WAV_CLIP_ERR_TRUNCATED;
        }
        const uint8_t *id = head + pos;
        const uint32_t size = rd32(head + pos + 4);
        const size_t body = pos + 8;

        if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) {
                return WAV_CLIP_ERR_FORMAT;
            }
            const uint32_t offset = (uint32_t)body;
            const uint32_t avail = file_size > offset ? file_size - offset : 0;
            const uint32_t bytes = (size == 0 || size > avail) ? avail : size;
            const uint32_t frames = bytes / 2u;
            if (frames == 0) {
                return WAV_CLIP_ERR_EMPTY;
            }
            out->sample_rate_hz = rate;
            out->data_offset = offset;
            out->frames = frames;
            out->duration_ms = (uint32_t)(((uint64_t)frames * 1000u + rate - 1u) / rate);
            return WAV_CLIP_OK;
        }
        if (memcmp(id, "fmt ", 4) == 0) {
            if (size < 16) {
                return WAV_CLIP_ERR_FORMAT;
            }
            if (len - body < 16) {
                return WAV_CLIP_ERR_TRUNCATED;
            }
            const uint8_t *f = head + body;
            const uint16_t tag = rd16(f);
            const uint16_t channels = rd16(f + 2);
            rate = rd32(f + 4);
            const uint16_t bits = rd16(f + 14);
            if (tag != 1 || channels != 1 || bits != 16 || rate < 8000 || rate > 48000) {
                return WAV_CLIP_ERR_FORMAT;
            }
            have_fmt = true;
        }
        /* next chunk: size plus one pad byte if odd */
        const uint64_t next = (uint64_t)body + size + (size & 1u);
        if (next > (uint64_t)len) {
            return WAV_CLIP_ERR_TRUNCATED;
        }
        pos = (size_t)next;
    }
}
