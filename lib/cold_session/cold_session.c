/*
 * cold_session – one cold-shower run as a record for the server.
 * Contract in cold_session.h, tests in test/test_cold_session/.
 */
#include "cold_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cold_session {
    bool open;
    bool cold_reached;
    bool skipped;
    uint32_t id;
    uint8_t profile;
    cold_profile_t plan;
    uint32_t start_utc;
    uint32_t begin_ms;
    cold_phase_t phase;
    uint32_t phase_since_ms;
    uint64_t warm_ms;
    uint64_t cold_ms;
    uint64_t after_ms;
};

cold_session_t *cold_session_create(void)
{
    return calloc(1, sizeof(cold_session_t));
}

void cold_session_destroy(cold_session_t *cs)
{
    free(cs);
}

bool cold_session_begin(cold_session_t *cs, uint32_t id, uint8_t profile, const cold_profile_t *plan, uint32_t utc,
                        uint32_t now_ms)
{
    if (cs == NULL || plan == NULL || id == 0 || (profile != COLD_SESSION_HYPE && profile != COLD_SESSION_ZEN)) {
        return false;
    }
    memset(cs, 0, sizeof(*cs));
    cs->open = true;
    cs->id = id;
    cs->profile = profile;
    cs->plan = *plan;
    cs->start_utc = utc;
    cs->begin_ms = now_ms;
    cs->phase = COLD_COUNTDOWN;
    cs->phase_since_ms = now_ms;
    return true;
}

/* Add the time since the last change to the running phase. */
static void close_phase(cold_session_t *cs, uint32_t now_ms)
{
    const uint32_t spent = now_ms - cs->phase_since_ms;
    switch (cs->phase) {
    case COLD_WARM:
        cs->warm_ms += spent;
        break;
    case COLD_COLD:
        cs->cold_ms += spent;
        break;
    case COLD_AFTER:
        cs->after_ms += spent;
        break;
    default:
        break;
    }
    cs->phase_since_ms = now_ms;
}

void cold_session_phase(cold_session_t *cs, cold_phase_t phase, uint32_t now_ms)
{
    if (cs == NULL || !cs->open || phase == cs->phase) {
        return;
    }
    close_phase(cs, now_ms);
    cs->phase = phase;
    if (phase == COLD_COLD) {
        cs->cold_reached = true;
    }
}

void cold_session_next(cold_session_t *cs)
{
    if (cs != NULL && cs->open) {
        cs->skipped = true;
    }
}

static uint32_t round_s(uint64_t ms)
{
    const uint64_t s = (ms + 500u) / 1000u;
    return s > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)s;
}

bool cold_session_end(cold_session_t *cs, cold_end_t end, uint32_t utc, uint32_t now_ms, cold_record_t *out)
{
    if (cs == NULL || !cs->open) {
        return false;
    }
    close_phase(cs, now_ms);
    cs->open = false;
    if (!cs->cold_reached || out == NULL) {
        return false;
    }
    cold_record_t r;
    memset(&r, 0, sizeof(r));
    r.id = cs->id;
    r.profile = cs->profile;
    r.planned_warm_s = cs->plan.warm_s;
    r.planned_cold_s = cs->plan.cold_s;
    r.planned_after_s = cs->plan.after_s;
    r.warm_s = round_s(cs->warm_ms);
    r.cold_s = round_s(cs->cold_ms);
    r.after_s = round_s(cs->after_ms);
    r.end = end;
    r.skipped = cs->skipped;
    r.start_utc = cs->start_utc;
    if (r.start_utc == 0 && utc != 0) {
        const uint32_t run_s = round_s((uint32_t)(now_ms - cs->begin_ms));
        r.start_utc = utc > run_s ? utc - run_s : 0;
    }
    *out = r;
    return true;
}

int cold_session_to_json(const cold_record_t *rec, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (rec == NULL) {
        return -1;
    }
    char start[16];
    if (rec->start_utc == 0) {
        strcpy(start, "null");
    } else {
        snprintf(start, sizeof(start), "%lu", (unsigned long)rec->start_utc);
    }
    char buf[COLD_SESSION_JSON_MAX + 32];
    const int n = snprintf(buf, sizeof(buf),
                           "{\"type\":\"cold_session\",\"id\":%lu,\"start\":%s,\"profile\":\"%s\","
                           "\"planned\":{\"warm\":%u,\"cold\":%u,\"after\":%u},"
                           "\"actual\":{\"warm\":%lu,\"cold\":%lu,\"after\":%lu},\"end\":\"%s\",\"skipped\":%s}",
                           (unsigned long)rec->id, start, rec->profile == COLD_SESSION_ZEN ? "zen" : "hype",
                           (unsigned)rec->planned_warm_s, (unsigned)rec->planned_cold_s,
                           (unsigned)rec->planned_after_s, (unsigned long)rec->warm_s, (unsigned long)rec->cold_s,
                           (unsigned long)rec->after_s, rec->end == COLD_END_ABORTED ? "aborted" : "finished",
                           rec->skipped ? "true" : "false");
    if (n < 0 || (size_t)n + 1 > out_size || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    memcpy(out, buf, (size_t)n + 1);
    return n;
}
