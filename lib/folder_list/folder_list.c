/*
 * folder_list – the bedside's copy of the speaker's folders.
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

static void copy_name(char *dst, const char *src)
{
    size_t n = 0;
    while (n < LINK_FOLDER_LEN && src[n] != '\0') {
        n++;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool folder_list_feed(folder_list_t *fl, const link_folder_t *frame)
{
    if (fl == NULL || frame == NULL) {
        return false;
    }

    if (frame->count == 0 && frame->index == 0) { /* empty card */
        fl->ready = true;
        fl->cur_count = 0;
        fl->rx_active = false;
        return true;
    }
    if (frame->index >= frame->count) {
        return false;
    }

    char name[LINK_FOLDER_LEN + 1];
    copy_name(name, frame->name);
    if (name[0] == '\0' || !cfg_folder_valid(name)) {
        return false;
    }
    if (frame->index >= FOLDER_LIST_MAX) {
        return true; /* beyond what we keep, whatever the state */
    }

    const bool same = fl->rx_active && fl->rx_count == frame->count;
    if (frame->index == 0) {
        if (!same) {
            fl->rx_active = true;
            fl->rx_count = frame->count;
            fl->rx_have = 0;
        }
    } else if (!same) {
        return false;
    }

    memcpy(fl->rx[frame->index], name, sizeof(name));
    fl->rx_have |= (uint32_t)1u << frame->index;

    const size_t need = (fl->rx_count < FOLDER_LIST_MAX) ? fl->rx_count : FOLDER_LIST_MAX;
    const uint32_t all = (need >= 32) ? 0xFFFFFFFFu : (((uint32_t)1u << need) - 1u);
    if ((fl->rx_have & all) == all) {
        memcpy(fl->cur, fl->rx, need * sizeof(fl->cur[0]));
        fl->cur_count = need;
        fl->ready = true;
        fl->rx_active = false;
    }
    return true;
}

bool folder_list_ready(const folder_list_t *fl)
{
    return fl != NULL && fl->ready;
}

size_t folder_list_count(const folder_list_t *fl)
{
    return (fl != NULL && fl->ready) ? fl->cur_count : 0;
}

const char *folder_list_name(const folder_list_t *fl, size_t i)
{
    if (fl == NULL || !fl->ready || i >= fl->cur_count) {
        return NULL;
    }
    return fl->cur[i];
}

bool folder_list_has(const folder_list_t *fl, const char *name)
{
    if (fl == NULL || name == NULL || !fl->ready) {
        return false;
    }
    for (size_t i = 0; i < fl->cur_count; i++) {
        if (strcmp(fl->cur[i], name) == 0) {
            return true;
        }
    }
    return false;
}
