/*
 * folder_list – the speaker's folders on the bedside.
 * Contract in folder_list.h, tests in test/test_folder_list/.
 */
#include "folder_list.h"

#include <string.h>

#include "node_config.h"

void folder_list_init(folder_list_t *fl)
{
    if (fl != NULL) {
        memset(fl, 0, sizeof(*fl));
    }
}

static size_t rx_needed(const folder_list_t *fl)
{
    return (fl->rx_count < FOLDER_LIST_MAX) ? fl->rx_count : FOLDER_LIST_MAX;
}

static void copy_name(char *dst, const char *src)
{
    size_t n = 0;
    while (n < LINK_FOLDER_LEN && src[n] != '\0') {
        n++;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool folder_list_feed(folder_list_t *fl, const link_folder_t *f)
{
    if (fl == NULL || f == NULL) {
        return false;
    }

    if (f->count == 0 && f->index == 0) { /* empty card */
        fl->ready = true;
        fl->count = 0;
        fl->rx_running = false;
        return true;
    }
    if (f->index >= f->count) {
        return false;
    }

    char name[LINK_FOLDER_LEN + 1];
    copy_name(name, f->name);
    if (name[0] == '\0' || !cfg_folder_valid(name)) {
        return false;
    }

    if (f->index >= FOLDER_LIST_MAX) {
        return true; /* beyond what we keep: accepted, ignored, also after completion */
    }

    if (f->index == 0) {
        if (!fl->rx_running || fl->rx_count != f->count) {
            fl->rx_running = true;
            fl->rx_count = f->count;
            fl->rx_have = 0;
        }
    } else if (!fl->rx_running || fl->rx_count != f->count) {
        return false;
    }
    memcpy(fl->rx_names[f->index], name, sizeof(name));
    fl->rx_have |= (uint32_t)1u << f->index;

    const size_t need = rx_needed(fl);
    const uint32_t all = (need >= 32) ? 0xFFFFFFFFu : (((uint32_t)1u << need) - 1u);
    if ((fl->rx_have & all) == all) {
        memcpy(fl->names, fl->rx_names, need * sizeof(fl->names[0]));
        fl->count = (uint8_t)need;
        fl->ready = true;
        fl->rx_running = false;
    }
    return true;
}

bool folder_list_ready(const folder_list_t *fl)
{
    return fl != NULL && fl->ready;
}

size_t folder_list_count(const folder_list_t *fl)
{
    return (fl != NULL && fl->ready) ? fl->count : 0;
}

const char *folder_list_name(const folder_list_t *fl, size_t i)
{
    if (fl == NULL || !fl->ready || i >= fl->count) {
        return NULL;
    }
    return fl->names[i];
}

bool folder_list_has(const folder_list_t *fl, const char *name)
{
    if (fl == NULL || name == NULL || !fl->ready) {
        return false;
    }
    for (size_t i = 0; i < fl->count; i++) {
        if (strcmp(fl->names[i], name) == 0) {
            return true;
        }
    }
    return false;
}
