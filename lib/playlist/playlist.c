/*
 * playlist – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in playlist.h, the tests in test/test_playlist/.
 */
#include "playlist.h"

#include <string.h>

void playlist_config_default(playlist_config_t *cfg)
{
    if (cfg != NULL) {
        memset(cfg, 0, sizeof(*cfg)); /* TODO */
    }
}

playlist_t *playlist_create(const playlist_config_t *cfg)
{
    (void)cfg;
    return NULL; /* TODO */
}

void playlist_destroy(playlist_t *pl)
{
    (void)pl;
}

void playlist_clear(playlist_t *pl)
{
    (void)pl;
}

playlist_add_result_t playlist_add(playlist_t *pl, const char *name, bool is_dir)
{
    (void)pl;
    (void)name;
    (void)is_dir;
    return PLAYLIST_ERR_FINALIZED; /* TODO */
}

void playlist_finalize(playlist_t *pl)
{
    (void)pl;
}

size_t playlist_count(const playlist_t *pl)
{
    (void)pl;
    return 0;
}

const char *playlist_next(playlist_t *pl)
{
    (void)pl;
    return NULL;
}

void playlist_mark_bad(playlist_t *pl, const char *name)
{
    (void)pl;
    (void)name;
}
