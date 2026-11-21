/*
 * resume_point – where a kept music session stopped, as bytes for NVS.
 * Contract in resume_point.h, tests in test/test_resume_point/.
 */
#include "resume_point.h"

#include <string.h>

#define RP_MAGIC0 0x52u
#define RP_MAGIC1 0x50u
#define RP_VERSION 1u

/* CRC-32 (IEEE 802.3, reflected, as zlib's crc32()). Bitwise: the blob is
 * small and written rarely, no table needed. */
static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool char_ok(uint8_t c)
{
    return c >= 0x20 && c <= 0x7E && c != '/';
}

/* Length of a valid name of 1 ... max characters in field (max + 1 bytes),
 * or 0 if it is empty, has no NUL within the field, or a bad character. */
static size_t name_len(const char *field, size_t max)
{
    size_t n = 0;
    while (n <= max && field[n] != '\0') {
        if (!char_ok((uint8_t)field[n])) {
            return 0;
        }
        n++;
    }
    return (n > max) ? 0 : n;
}

static bool bytes_ok(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!char_ok(p[i])) {
            return false;
        }
    }
    return true;
}

size_t resume_encode(const resume_point_t *rp, uint8_t *out, size_t out_size)
{
    if (rp == NULL || out == NULL || rp->index == 0) {
        return 0;
    }
    const size_t f = name_len(rp->folder, RESUME_FOLDER_MAX);
    const size_t t = name_len(rp->track, RESUME_TRACK_MAX);
    if (f == 0 || t == 0) {
        return 0;
    }
    const size_t len = 21 + f + t;
    if (out_size < len) {
        return 0;
    }

    out[0] = RP_MAGIC0;
    out[1] = RP_MAGIC1;
    out[2] = RP_VERSION;
    out[3] = (uint8_t)f;
    memcpy(&out[4], rp->folder, f);
    out[4 + f] = (uint8_t)t;
    memcpy(&out[5 + f], rp->track, t);
    put_u32(&out[5 + f + t], rp->seed);
    put_u32(&out[9 + f + t], rp->index);
    put_u32(&out[13 + f + t], rp->offset);
    put_u32(&out[17 + f + t], crc32_ieee(out, 17 + f + t));
    return len;
}

bool resume_decode(const uint8_t *blob, size_t len, resume_point_t *out)
{
    if (blob == NULL || out == NULL || len < 5) {
        return false;
    }
    if (blob[0] != RP_MAGIC0 || blob[1] != RP_MAGIC1 || blob[2] != RP_VERSION) {
        return false;
    }
    const size_t f = blob[3];
    if (f == 0 || f > RESUME_FOLDER_MAX || len < 5 + f) {
        return false;
    }
    const size_t t = blob[4 + f];
    if (t == 0 || t > RESUME_TRACK_MAX || len != 21 + f + t) {
        return false;
    }
    if (get_u32(&blob[17 + f + t]) != crc32_ieee(blob, 17 + f + t)) {
        return false;
    }
    const uint32_t index = get_u32(&blob[9 + f + t]);
    if (index == 0 || !bytes_ok(&blob[4], f) || !bytes_ok(&blob[5 + f], t)) {
        return false;
    }

    resume_point_t rp;
    memset(&rp, 0, sizeof(rp));
    memcpy(rp.folder, &blob[4], f);
    memcpy(rp.track, &blob[5 + f], t);
    rp.seed = get_u32(&blob[5 + f + t]);
    rp.index = index;
    rp.offset = get_u32(&blob[13 + f + t]);
    *out = rp;
    return true;
}
