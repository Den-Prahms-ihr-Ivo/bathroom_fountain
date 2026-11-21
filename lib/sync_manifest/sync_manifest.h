/*
 * sync_manifest – the file list of cmd/music/sync and the rules for which
 * files a sync may write or delete on the SD card (pure C, uses cJSON).
 *
 * Requirements: SRV-11 (music library), docs/PROTOCOL.md (Commands:
 *               music/sync; HTTPS).
 * Tests:        test/test_sync_manifest/
 *
 * Document
 *   {"base":"https://host/files/","files":[{"path":"B/track01.mp3",
 *    "size":4812345,"sha256":"<64 hex digits>"}, ...]}
 *   base   string, starts with "https://", ends with '/', shorter than
 *          SYNC_BASE_MAX characters
 *   files  array (may be empty: "nothing should be on the card")
 *
 * sync_manifest_parse()
 *   NULL with *err = SYNC_MF_ERR_FORMAT if the text is not a JSON object,
 *   base is missing or not as above, or files is missing or not an array;
 *   SYNC_MF_ERR_MEMORY when memory runs out. Otherwise a manifest whose
 *   files are the valid entries in document order. An entry is skipped
 *   (counted in sync_manifest_skipped()) if it is not an object, path is not
 *   a valid path (sync_path_valid), size is not an integer 0 ... 4294967295,
 *   sha256 is not 64 hex digits (either case), or the path was already
 *   listed. Entries beyond SYNC_MAX_FILES are skipped too.
 *
 * Valid path (what a sync may write)
 *   "name" or "folder/name" (one level at most), and:
 *   folder  1 ... 23 characters of A-Z a-z 0-9 '_' '-'
 *   name    1 ... SYNC_NAME_MAX characters 0x20 ... 0x7E, none of
 *           / \ : * ? " < > | ; does not start with '.' or ' ', does not
 *           end with '.' or ' '; ends in ".mp3" or ".wav" (any case) with at
 *           least one character before it
 *
 * Managed file (what a sync may delete when it is not listed)
 *   a file name (no folder part) ending in ".mp3", ".wav" or ".part", any
 *   case. Everything else on the card is never touched.
 *
 * URL of a file: base + path with every byte outside A-Z a-z 0-9 - . _ ~ /
 * written as %XX (upper-case hex).
 */
#ifndef SYNC_MANIFEST_H
#define SYNC_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_BASE_MAX   160  /* including the NUL */
#define SYNC_FOLDER_MAX 23
#define SYNC_NAME_MAX   95
#define SYNC_PATH_MAX   (SYNC_FOLDER_MAX + 1 + SYNC_NAME_MAX) /* 119, without NUL */
#define SYNC_MAX_FILES  256

typedef enum {
    SYNC_MF_OK = 0,
    SYNC_MF_ERR_FORMAT,
    SYNC_MF_ERR_MEMORY,
} sync_mf_err_t;

typedef struct {
    char path[SYNC_PATH_MAX + 1];
    uint32_t size;
    uint8_t sha256[32];
} sync_file_t;

typedef struct sync_manifest sync_manifest_t;

/* json need not be NUL-terminated. err may be NULL. */
sync_manifest_t *sync_manifest_parse(const char *json, size_t len, sync_mf_err_t *err);

/* Accepts NULL. */
void sync_manifest_destroy(sync_manifest_t *m);

/* 0 / NULL for NULL. */
const char *sync_manifest_base(const sync_manifest_t *m);
size_t sync_manifest_count(const sync_manifest_t *m);
size_t sync_manifest_skipped(const sync_manifest_t *m);
const sync_file_t *sync_manifest_file(const sync_manifest_t *m, size_t index); /* NULL when out of range */

/* Is this path listed (exact, case-sensitive)? false for NULL. */
bool sync_manifest_has(const sync_manifest_t *m, const char *path);

/* base + encoded path into out. false (out = "" if out_size > 0) if an
 * argument is NULL or it does not fit. */
bool sync_manifest_url(const sync_manifest_t *m, const char *path, char *out, size_t out_size);

bool sync_path_valid(const char *path);
bool sync_name_managed(const char *name);

/* 64 hex digits (either case) -> 32 bytes. false (out untouched) otherwise. */
bool sync_hex_sha256(const char *hex, uint8_t out[32]);

#ifdef __cplusplus
}
#endif

#endif /* SYNC_MANIFEST_H */
