/*
 * resume_point – where a kept music session stopped, as bytes for NVS
 * (pure C, no file system).
 *
 * Requirements: BED-46 (see the requirements doc).
 * Tests:        test/test_resume_point/
 *
 * The speaker plays sleep music with LINK_AUDIO_FLAG_KEEP. When such a
 * session stops (STOP, another PLAY, the alarm, standby), the speaker notes
 * the folder, the playlist's shuffle seed, how many tracks the playlist has
 * handed out since playlist_finalize() (the playing track is the index-th),
 * the track's file name and the byte offset in it, and saves that blob in
 * NVS. A PLAY with LINK_AUDIO_FLAG_RESUME for the same folder rebuilds the
 * playlist with the same seed, calls playlist_next() index times, and if the
 * name it gets equals `track`, opens that file at `offset` (the MP3 decoder
 * finds the next frame); otherwise it starts a fresh shuffle. The offset is
 * taken when the stop arrives, before any fade-out, so the resumed part is
 * the part that was still heard at full volume.
 *
 * Blob (multi-byte fields little-endian)
 *   0      'R' 'P'          magic (0x52 0x50)
 *   2      1                version
 *   3      folder length F  1 ... RESUME_FOLDER_MAX
 *   4      folder           F bytes, no NUL
 *   4+F    track length N   1 ... RESUME_TRACK_MAX
 *   5+F    track            N bytes, no NUL
 *   5+F+N  seed u32, index u32, offset u32
 *   17+F+N crc u32          CRC-32 (IEEE 802.3, as zlib's crc32()) of
 *                           every byte before it
 *   Length 21 + F + N, at most RESUME_BLOB_MAX.
 *
 * Valid resume point: folder and track 1 ... their maximum characters, each
 * character 0x20 ... 0x7E and not '/' ; index >= 1. seed and offset take any
 * value.
 *
 * resume_encode() returns the blob length, or 0 (out untouched) if rp or out
 * is NULL, rp is not valid, or out_size is too small.
 * resume_decode() returns false (out untouched) if blob or out is NULL, len
 * is not exactly the length the two length bytes give, magic, version or
 * CRC do not match, or the result would not be valid. folder and track come
 * back NUL-terminated.
 */
#ifndef RESUME_POINT_H
#define RESUME_POINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RESUME_FOLDER_MAX 23
#define RESUME_TRACK_MAX 95
#define RESUME_BLOB_MAX (21 + RESUME_FOLDER_MAX + RESUME_TRACK_MAX)

typedef struct {
    char folder[RESUME_FOLDER_MAX + 1];
    char track[RESUME_TRACK_MAX + 1];
    uint32_t seed;
    uint32_t index;  /* playlist_next() calls since finalize; the playing track is the index-th */
    uint32_t offset; /* byte offset in the track file */
} resume_point_t;

size_t resume_encode(const resume_point_t *rp, uint8_t *out, size_t out_size);

bool resume_decode(const uint8_t *blob, size_t len, resume_point_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RESUME_POINT_H */
