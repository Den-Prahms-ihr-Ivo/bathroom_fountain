/*
 * resume_restore – capture and restore a resume point.
 * Contract in resume_restore.h, tests in test/test_resume_restore/.
 */
#include "resume_restore.h"

#include <stddef.h>
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

bool resume_capture(resume_point_t *out, const char *folder, uint32_t seed, uint32_t index, const char *track,
                    uint32_t offset)
{
    if (out == NULL || folder == NULL || track == NULL) {
        return false;
    }
    const size_t f = bounded_len(folder, RESUME_FOLDER_MAX);
    const size_t t = bounded_len(track, RESUME_TRACK_MAX);
    if (f > RESUME_FOLDER_MAX || t > RESUME_TRACK_MAX) {
        return false;
    }

    resume_point_t rp;
    memset(&rp, 0, sizeof(rp));
    memcpy(rp.folder, folder, f);
    memcpy(rp.track, track, t);
    rp.seed = seed;
    rp.index = index;
    rp.offset = offset;

    uint8_t blob[RESUME_BLOB_MAX];
    if (resume_encode(&rp, blob, sizeof(blob)) == 0) { /* the validity rules live there */
        return false;
    }
    *out = rp;
    return true;
}

const char *resume_restore(playlist_t *pl, const resume_point_t *rp, const char *folder, uint32_t *offset_out)
{
    if (pl == NULL || rp == NULL || folder == NULL || offset_out == NULL) {
        return NULL;
    }
    if (strcmp(folder, rp->folder) != 0 || rp->index == 0 || rp->index > RESUME_REPLAY_MAX) {
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
