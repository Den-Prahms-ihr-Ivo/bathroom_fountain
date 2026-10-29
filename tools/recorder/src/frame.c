#include "frame.h"

#include <string.h>

uint16_t frame_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}

size_t frame_encode(uint8_t *out, size_t out_size, uint32_t seq, uint16_t flags,
                    const int32_t *samples24, uint16_t n)
{
    if (n == 0 || n > FRAME_MAX_SAMPLES || out == NULL || samples24 == NULL) {
        return 0;
    }
    const size_t len = FRAME_BYTES(n);
    if (out_size < len) {
        return 0;
    }
    memcpy(out, FRAME_MAGIC, 4);
    put_u32(out + 4, seq);
    put_u16(out + 8, n);
    put_u16(out + 10, flags);
    uint8_t *p = out + FRAME_HEADER_BYTES;
    for (uint16_t i = 0; i < n; i++) {
        const uint32_t v = (uint32_t)samples24[i];
        *p++ = (uint8_t)(v & 0xFFu);
        *p++ = (uint8_t)((v >> 8) & 0xFFu);
        *p++ = (uint8_t)((v >> 16) & 0xFFu);
    }
    put_u16(p, frame_crc16(out + 4, len - 4 - FRAME_CRC_BYTES));
    return len;
}
