/*
 * cold_session – one cold-shower run as a record for the server: when it
 * started, how long each phase really lasted, how it ended (pure C).
 *
 * Requirements: COLD-15 ... COLD-17 (see the requirements doc).
 * Tests:        test/test_cold_session/
 *
 * The control task feeds it what cold_protocol does: begin() on start, every
 * change of outputs.phase, next() presses, and the end. end() returns the
 * finished record; cold_session_to_json() turns it into the event the device
 * publishes (docs/PROTOCOL.md, event "cold_session"). The record then waits in
 * lib/record_outbox until the server has stored it.
 *
 * Time: now_ms is the usual wrapping millisecond counter. utc is seconds
 * since 1970, or 0 while the clock is not set (no SNTP yet).
 *
 * Timing rules
 *   begin() opens the countdown. Countdown time counts for no phase.
 *   phase(p) closes the running phase and opens p: the time from the previous
 *   change (or begin) to now is added to the phase that was running.
 *   Reporting the phase that is already running changes nothing. IDLE and
 *   COUNTDOWN count for nothing.
 *   end() closes the running phase the same way.
 *   Durations are kept in ms and rounded to whole seconds (half up) in the
 *   record.
 *
 * Start time
 *   start_utc is the utc given to begin(). If that was 0 and end() is given a
 *   utc, start_utc = that utc minus the seconds from begin() to end()
 *   (rounded half up). Otherwise it stays 0 ("unknown").
 *
 * What counts as a session
 *   end() returns false and writes nothing if the cold phase never began (a
 *   press by mistake, aborted during the countdown or the warm phase). Every
 *   run that reached the cold phase gives a record, finished or aborted.
 *   After end(), and before the first begin(), phase(), next() and end() do
 *   nothing (end returns false).
 */
#ifndef COLD_SESSION_H
#define COLD_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cold_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COLD_SESSION_JSON_MAX 256 /* every record fits */

typedef enum {
    COLD_SESSION_HYPE = 0,
    COLD_SESSION_ZEN,
} cold_session_profile_t;

typedef struct {
    uint32_t id;        /* given to begin(): unique per device, see record_outbox */
    uint32_t start_utc; /* 0 = unknown */
    uint8_t profile;    /* cold_session_profile_t */
    uint16_t planned_warm_s;
    uint16_t planned_cold_s;
    uint16_t planned_after_s;
    uint32_t warm_s;    /* time really spent in each phase */
    uint32_t cold_s;
    uint32_t after_s;
    cold_end_t end;
    bool skipped;       /* next() was pressed at least once */
} cold_record_t;

typedef struct cold_session cold_session_t;

/* Returns NULL only when memory runs out. */
cold_session_t *cold_session_create(void);

/* Accepts NULL. */
void cold_session_destroy(cold_session_t *cs);

/* Starts a new session; a session still open is dropped. Returns false (and
 * changes nothing) if plan is NULL, id is 0 or profile is not HYPE or ZEN. */
bool cold_session_begin(cold_session_t *cs, uint32_t id, uint8_t profile, const cold_profile_t *plan, uint32_t utc,
                        uint32_t now_ms);

void cold_session_phase(cold_session_t *cs, cold_phase_t phase, uint32_t now_ms);

void cold_session_next(cold_session_t *cs);

bool cold_session_end(cold_session_t *cs, cold_end_t end, uint32_t utc, uint32_t now_ms, cold_record_t *out);

/*
 * The record as one line of JSON, NUL-terminated, members in this order:
 *   {"type":"cold_session","id":17,"start":1795000000,"profile":"hype",
 *    "planned":{"warm":120,"cold":90,"after":300},
 *    "actual":{"warm":118,"cold":90,"after":12},"end":"aborted","skipped":true}
 * "start" is null when unknown; "end" is "finished" or "aborted".
 * Returns the length, or -1 (out empty if out_size > 0) if out is too small
 * or rec is NULL. No library: this module only writes digits and fixed text.
 */
int cold_session_to_json(const cold_record_t *rec, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* COLD_SESSION_H */
