/*
 * folder_list – the speaker's SD card folders on the bedside, put together
 * from LINK_FOLDER frames (pure C, no radio).
 *
 * Requirements: BED-43, BED-50 (see the requirements doc).
 * Tests:        test/test_folder_list/
 *
 * The speaker sends its folders one frame each (index 0 ... count-1) after
 * every scan and on HELLO; frames may arrive out of order or twice (retries).
 *
 *   Reception  A frame with index 0 and a valid name starts a reception of
 *              `count` folders, unless one with the same count is already
 *              running (a retried index 0 does not restart it). A frame with
 *              index > 0 belongs to the running reception: it is refused if
 *              none runs or its count differs. A frame seen again overwrites
 *              its name.
 *   Complete   Once the frames 0 ... min(count, FOLDER_LIST_MAX) - 1 are all
 *              in, the list replaces the current one and the reception ends.
 *              Until then the old list stays as it is.
 *   Overflow   A frame with a valid name and FOLDER_LIST_MAX <= index < count
 *              is always accepted and ignored (also with no reception
 *              running, e.g. after the first FOLDER_LIST_MAX completed).
 *   Empty card A frame with count 0 and index 0 replaces the list with an
 *              empty one at once and drops any running reception.
 *   Refused    (false, nothing changes) NULL, index >= count (other than the
 *              empty-card frame), a name that is empty or not
 *              cfg_folder_valid(), or an index > 0 without a matching
 *              reception.
 *
 * ready: a complete list has arrived since init (possibly an empty one).
 * folder_list_has() compares case-sensitively against the complete list.
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
    /* the complete list */
    bool ready;
    uint8_t count;
    char names[FOLDER_LIST_MAX][LINK_FOLDER_LEN + 1];
    /* the reception in progress */
    bool rx_running;
    uint8_t rx_count;
    uint32_t rx_have; /* bit i: frame i is in */
    char rx_names[FOLDER_LIST_MAX][LINK_FOLDER_LEN + 1];
} folder_list_t;

/* Empty, not ready, no reception. Accepts NULL. */
void folder_list_init(folder_list_t *fl);

/* true if the frame was accepted (see above). */
bool folder_list_feed(folder_list_t *fl, const link_folder_t *f);

/* false / 0 / NULL for fl NULL. */
bool folder_list_ready(const folder_list_t *fl);
size_t folder_list_count(const folder_list_t *fl);

/* Name i of the complete list, NULL if i >= count. */
const char *folder_list_name(const folder_list_t *fl, size_t i);

/* false for NULL arguments. */
bool folder_list_has(const folder_list_t *fl, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* FOLDER_LIST_H */
