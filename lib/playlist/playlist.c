/*
 * playlist – which file plays next.
 *
 * The contract is in playlist.h, the tests in test/test_playlist/.
 *
 * Tracks are stored in add order in one fixed name buffer; finalize() sorts an
 * index array. Shuffle builds one permutation of the playable tracks per
 * round (Fisher–Yates with a small seeded PRNG).
 */
#include "playlist.h"

#include <stdlib.h>
#include <string.h>

#define MAX_TRACKS_LIMIT   1024u
#define NAME_LEN_MIN       8u
#define NAME_LEN_MAX       256u
#define NONE               UINT16_MAX

struct playlist {
    playlist_config_t cfg;
    char *names;        /* max_tracks * max_name_len, track i at i * max_name_len */
    uint16_t *order;    /* sorted track indices (natural order) */
    uint16_t *round;    /* shuffle: this round's order */
    bool *bad;
    uint16_t n;         /* tracks accepted */
    uint16_t n_bad;
    bool finalized;
    uint16_t pos;       /* next position in order (sequential) or round (shuffle) */
    uint16_t round_len;
    uint16_t last;      /* track returned last, NONE if none yet */
    uint32_t rng;
};

void playlist_config_default(playlist_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->max_tracks = 256;
        cfg->max_name_len = 96;
        cfg->shuffle = false;
        cfg->seed = 1;
    }
}

static const char *name_of(const playlist_t *pl, uint16_t i)
{
    return pl->names + (size_t)i * pl->cfg.max_name_len;
}

/* splitmix32: any seed works, including 0. */
static uint32_t next_random(playlist_t *pl)
{
    uint32_t z = (pl->rng += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

/* Uniform-enough index in [0, k). */
static uint16_t random_below(playlist_t *pl, uint16_t k)
{
    return (uint16_t)(((uint64_t)next_random(pl) * k) >> 32);
}

playlist_t *playlist_create(const playlist_config_t *cfg)
{
    if (cfg == NULL || cfg->max_tracks == 0 || cfg->max_tracks > MAX_TRACKS_LIMIT
        || cfg->max_name_len < NAME_LEN_MIN || cfg->max_name_len > NAME_LEN_MAX) {
        return NULL;
    }

    playlist_t *pl = calloc(1, sizeof(*pl));
    if (pl == NULL) {
        return NULL;
    }
    pl->cfg = *cfg;
    pl->names = malloc((size_t)cfg->max_tracks * cfg->max_name_len);
    pl->order = malloc(cfg->max_tracks * sizeof(pl->order[0]));
    pl->round = malloc(cfg->max_tracks * sizeof(pl->round[0]));
    pl->bad = malloc(cfg->max_tracks * sizeof(pl->bad[0]));
    if (pl->names == NULL || pl->order == NULL || pl->round == NULL || pl->bad == NULL) {
        playlist_destroy(pl);
        return NULL;
    }
    playlist_clear(pl);
    return pl;
}

void playlist_destroy(playlist_t *pl)
{
    if (pl != NULL) {
        free(pl->names);
        free(pl->order);
        free(pl->round);
        free(pl->bad);
        free(pl);
    }
}

void playlist_clear(playlist_t *pl)
{
    if (pl == NULL) {
        return;
    }
    memset(pl->bad, 0, pl->cfg.max_tracks * sizeof(pl->bad[0]));
    pl->n = 0;
    pl->n_bad = 0;
    pl->finalized = false;
    pl->pos = 0;
    pl->round_len = 0;
    pl->last = NONE;
    pl->rng = pl->cfg.seed;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool has_mp3_extension(const char *name, size_t len)
{
    static const char ext[] = ".mp3";
    const size_t ext_len = sizeof(ext) - 1;
    if (len <= ext_len) {
        return false; /* needs at least one character before ".mp3" */
    }
    for (size_t i = 0; i < ext_len; i++) {
        if (lower(name[len - ext_len + i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

playlist_add_result_t playlist_add(playlist_t *pl, const char *name, bool is_dir)
{
    if (pl == NULL || name == NULL || pl->finalized) {
        return PLAYLIST_ERR_FINALIZED;
    }
    if (is_dir) {
        return PLAYLIST_SKIPPED_DIR;
    }
    if (name[0] == '.') {
        return PLAYLIST_SKIPPED_HIDDEN;
    }
    const size_t len = strlen(name);
    if (!has_mp3_extension(name, len)) {
        return PLAYLIST_SKIPPED_NOT_AUDIO;
    }
    if (len >= pl->cfg.max_name_len) {
        return PLAYLIST_SKIPPED_TOO_LONG; /* never truncate: it could not be opened */
    }
    if (pl->n >= pl->cfg.max_tracks) {
        return PLAYLIST_SKIPPED_FULL;
    }
    memcpy(pl->names + (size_t)pl->n * pl->cfg.max_name_len, name, len + 1);
    pl->n++;
    return PLAYLIST_ADDED;
}

/*
 * Natural order: case-insensitive, digit runs by numeric value (of any
 * length), ties broken by strcmp().
 */
static int natural_compare(const char *a, const char *b)
{
    const char *pa = a;
    const char *pb = b;
    while (*pa != '\0' && *pb != '\0') {
        if (is_digit(*pa) && is_digit(*pb)) {
            while (*pa == '0') {
                pa++;
            }
            while (*pb == '0') {
                pb++;
            }
            size_t la = 0;
            size_t lb = 0;
            while (is_digit(pa[la])) {
                la++;
            }
            while (is_digit(pb[lb])) {
                lb++;
            }
            if (la != lb) {
                return (la < lb) ? -1 : 1; /* more significant digits = larger */
            }
            const int c = memcmp(pa, pb, la);
            if (c != 0) {
                return c;
            }
            pa += la;
            pb += lb;
        } else {
            const char ca = lower(*pa);
            const char cb = lower(*pb);
            if (ca != cb) {
                return ((unsigned char)ca < (unsigned char)cb) ? -1 : 1;
            }
            pa++;
            pb++;
        }
    }
    if (*pa != *pb) {
        return (*pa == '\0') ? -1 : 1; /* shorter first */
    }
    return strcmp(a, b);
}

/* Fresh shuffle round of the playable tracks; never starts with the last one. */
static void build_round(playlist_t *pl)
{
    uint16_t m = 0;
    for (uint16_t i = 0; i < pl->n; i++) {
        if (!pl->bad[i]) {
            pl->round[m++] = i;
        }
    }
    for (uint16_t i = m; i > 1; i--) {
        const uint16_t j = random_below(pl, i);
        const uint16_t t = pl->round[i - 1];
        pl->round[i - 1] = pl->round[j];
        pl->round[j] = t;
    }
    if (m >= 2 && pl->round[0] == pl->last) {
        const uint16_t j = (uint16_t)(1 + random_below(pl, (uint16_t)(m - 1)));
        pl->round[0] = pl->round[j];
        pl->round[j] = pl->last;
    }
    pl->round_len = m;
    pl->pos = 0;
}

void playlist_finalize(playlist_t *pl)
{
    if (pl == NULL) {
        return;
    }
    /* Insertion sort: at most 1024 short names, once per scan, and stable. */
    for (uint16_t i = 0; i < pl->n; i++) {
        uint16_t j = i;
        while (j > 0 && natural_compare(name_of(pl, pl->order[j - 1]), name_of(pl, i)) > 0) {
            pl->order[j] = pl->order[j - 1];
            j--;
        }
        pl->order[j] = i;
    }
    pl->finalized = true;
    pl->pos = 0;
    pl->last = NONE;
    if (pl->cfg.shuffle) {
        build_round(pl);
    }
}

size_t playlist_count(const playlist_t *pl)
{
    return (pl != NULL) ? (size_t)(pl->n - pl->n_bad) : 0;
}

const char *playlist_next(playlist_t *pl)
{
    if (pl == NULL || !pl->finalized || playlist_count(pl) == 0) {
        return NULL;
    }

    uint16_t track;
    if (pl->cfg.shuffle) {
        do {
            if (pl->pos >= pl->round_len) {
                build_round(pl);
            }
            track = pl->round[pl->pos++];
        } while (pl->bad[track]); /* marked bad during this round */
    } else {
        do {
            if (pl->pos >= pl->n) {
                pl->pos = 0; /* loop (AUD-02) */
            }
            track = pl->order[pl->pos++];
        } while (pl->bad[track]);
    }
    pl->last = track;
    return name_of(pl, track);
}

void playlist_mark_bad(playlist_t *pl, const char *name)
{
    if (pl == NULL || name == NULL) {
        return;
    }
    for (uint16_t i = 0; i < pl->n; i++) {
        if (!pl->bad[i] && strcmp(name_of(pl, i), name) == 0) {
            pl->bad[i] = true;
            pl->n_bad++;
            return;
        }
    }
}
