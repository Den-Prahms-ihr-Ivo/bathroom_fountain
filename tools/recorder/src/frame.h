/*
 * Audio frame protocol between the recorder firmware and tools/capture.py.
 *
 * One frame, all fields little-endian:
 *
 *   offset  size  field
 *   0       4     magic "AUD1"
 *   4       4     seq        frame counter, +1 per frame, wraps
 *   8       2     n          number of samples in this frame (1 … FRAME_MAX_SAMPLES)
 *   10      2     flags      FRAME_FLAG_* below
 *   12      3n    samples    signed 24-bit, little-endian, two's complement
 *   12+3n   2     crc        CRC-16/CCITT-FALSE over bytes 4 … 12+3n-1
 *
 * The PC side resynchronises on the magic, drops frames with a bad CRC and
 * detects lost frames from gaps in seq.
 */
#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

#define FRAME_MAGIC          "AUD1"
#define FRAME_HEADER_BYTES   12u
#define FRAME_CRC_BYTES      2u
#define FRAME_MAX_SAMPLES    1024u
#define FRAME_BYTES(n)       (FRAME_HEADER_BYTES + 3u * (n) + FRAME_CRC_BYTES)

#define FRAME_FLAG_I2S_OVERFLOW 0x0001u /* the I2S driver dropped data since the previous frame */

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final XOR. */
uint16_t frame_crc16(const uint8_t *data, size_t len);

/*
 * Encode one frame. samples24 hold signed 24-bit values in int32 (the I2S
 * word shifted right by 8). Returns the frame length in bytes, or 0 if n is
 * 0, n > FRAME_MAX_SAMPLES or out is too small.
 */
size_t frame_encode(uint8_t *out, size_t out_size, uint32_t seq, uint16_t flags,
                    const int32_t *samples24, uint16_t n);

#endif /* FRAME_H */
