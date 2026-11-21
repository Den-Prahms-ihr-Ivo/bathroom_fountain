/*
 * resume_restore – capture and find again where a kept session stopped.
 * Contract in resume_restore.h, tests in test/test_resume_restore/.
 */
#include "resume_restore.h"

#include <stdio.h>
#include <string.h>

/* strlen, but stops after max + 1 characters (too long anyway). */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n <= max && s[n] != '\0') {
        n++;
    }
    return n;
}

bool resume_key(const char *folder, char *out, size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (folder == NULL || folder[0] == '\0' || out == NULL || out_size < 11) {
        return false;
    }
    uint32_t h = 0x811c9dc5u;
    for (const unsigned char *p = (const unsigned char *)folder; *p != '\0'; p++) {
        h ^= *p;
        h *= 0x01000193u;
    }
    snprintf(out, out_size, "rp%08x", (unsigned)h);
    return true;
}

bool resume_capture(resume_point_t *rp, const char *folder, uint32_t seed, uint32_t index, const char *track,
                    uint32_t offset)
{
    if (rp == NULL || folder == NULL || track == NULL) {
        return false;
    }
    const size_t f = bounded_len(folder, RESUME_FOLDER_MAX);
    const size_t t = bounded_len(track, RESUME_TRACK_MAX);
    if (f > RESUME_FOLDER_MAX || t > RESUME_TRACK_MAX) {
        return false;
    }

    resume_point_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.folder, folder, f);
    memcpy(tmp.track, track, t);
    tmp.seed = seed;
    tmp.index = index;
    tmp.offset = offset;

    uint8_t blob[RESUME_BLOB_MAX];
    if (resume_encode(&tmp, blob, sizeof(blob)) == 0) { /* the validity rules live there */
        return false;
    }
    *rp = tmp;
    return true;
}

const char *resume_restore(playlist_t *pl, const resume_point_t *rp, const char *folder, uint32_t *offset_out)
{
    if (pl == NULL || rp == NULL || folder == NULL || offset_out == NULL) {
        return NULL;
    }
    if (strcmp(folder, rp->folder) != 0 || rp->index == 0 || rp->index > RESUME_REPLAY_MAX ||
        playlist_count(pl) == 0) {
        return NULL;
    }

    const char *name = NULL;
    for (uint32_t i = 0; i < rp->index; i++) {
        name = playlist_next(pl);
        if (name == NULL) {
            return NULL;
        }
    }
    if (strcmp(name, rp->track) != 0) {
        return NULL;
    }
    *offset_out = rp->offset;
    return name;
}
