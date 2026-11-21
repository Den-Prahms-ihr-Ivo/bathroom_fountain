/*
 * folder_list – the bedside's copy of the folders on the speaker's SD card,
 * put together from LINK_FOLDER frames (pure C, no hardware).
 *
 * Requirements: BED-43, BED-50 (see the requirements doc).
 * Tests:        test/test_folder_list/
 *
 * The speaker sends one acked LINK_FOLDER frame per folder, index 0 ...
 * count-1, after every card scan and after a HELLO (docs/PROTOCOL.md). Acks
 * and retries can bring frames twice or out of order.
 *
 * Two lists: the *current* list (what the functions below return) and the
 * one being received. The current list only changes when a new list is
 * complete, so readers never see half a list.
 *
 *   index 0          starts a new list of `count` folders; a list that was
 *                    being received is dropped. Exception: while a list
 *                    with the same count is being received, an index-0
 *                    frame only replaces that name (a retried frame must
 *                    not throw away the rest). count 0: the current list
 *                    becomes empty at once (and is ready); a list being
 *                    received is dropped.
 *   index 1 ...      accepted only for the list being received, with the
 *                    same count and index < count. A frame for an index
 *                    already received replaces that name.
 *   complete         when indexes 0 ... min(count, FOLDER_LIST_MAX) - 1 have
 *                    all arrived: the list becomes the current one, in index
 *                    order.
 *   index >= FOLDER_LIST_MAX (and < count): always accepted (true) and
 *                    ignored, whatever the state; not needed for completion.
 *   name             must pass cfg_folder_valid() and not be "", else the
 *                    frame is refused (false) and changes nothing.
 *   refused (false)  NULL arguments, index >= count, no list being received
 *                    (index > 0 without an index-0 frame since the last
 *                    completion), count differing from the list being
 *                    received, bad name.
 *
 * Plain struct so it needs no heap; its fields are private: use the
 * functions. folder_list_init() before first use.
 */
#ifndef FOLDER_LIST_H
#define FOLDER_LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "link_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FOLDER_LIST_MAX 32

typedef struct {
    /* private */
    char cur[FOLDER_LIST_MAX][LINK_FOLDER_LEN + 1];
    size_t cur_count;
    bool ready;
    char rx[FOLDER_LIST_MAX][LINK_FOLDER_LEN + 1];
    uint32_t rx_have; /* bit per index received */
    uint8_t rx_count; /* count of the list being received */
    bool rx_active;
} folder_list_t;

/* Empty, not ready, nothing being received. Accepts NULL. */
void folder_list_init(folder_list_t *fl);

bool folder_list_feed(folder_list_t *fl, const link_folder_t *frame);

/* True once a complete list (also an empty one) has arrived since init. */
bool folder_list_ready(const folder_list_t *fl);

/* Folders in the current list (at most FOLDER_LIST_MAX); 0 for NULL. */
size_t folder_list_count(const folder_list_t *fl);

/* Name at i of the current list, NULL if i is out of range or fl NULL. */
const char *folder_list_name(const folder_list_t *fl, size_t i);

/* Exact (case-sensitive) match in the current list; false for NULL. */
bool folder_list_has(const folder_list_t *fl, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* FOLDER_LIST_H */
