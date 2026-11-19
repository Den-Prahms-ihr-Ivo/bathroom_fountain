/*
 * cold_protocol – the timed cold-shower sequence.
 * Contract in cold_protocol.h, tests in test/test_cold_protocol/.
 *
 * Every phase is planned when it begins: its length and up to two cues
 * (WARNING, then TO_COLD or DONE), as offsets from the phase's planned start.
 * update() walks forward through cues and phase ends in time order, so a late
 * call ends up exactly where regular ticks would have.
 */
#include "cold_protocol.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define CUE_SLOTS 2 /* [0] WARNING, [1] TO_COLD / DONE */

typedef struct {
    bool planned;
    bool issued;
    uint32_t offset_ms; /* from phase start */
    cold_cue_t cue;
} planned_cue_t;

struct cold_protocol {
    cold_config_t cfg;
    cold_profile_t profile;     /* used for phases that begin from now on */
    cold_profile_t new_profile; /* waiting for the next phase change */
    bool profile_waiting;
    uint32_t clip_ms[COLD_CUE_COUNT];

    uint32_t phase_start;
    uint32_t phase_len;
    cold_phase_t start_scene_phase; /* first timed phase whose scene start() issued */
    planned_cue_t cues[CUE_SLOTS];

    cold_outputs_t out;

    bool cue_pending;
    cold_cue_t cue;
    bool scene_pending;
    cold_scene_t scene;
    bool end_pending;
    cold_end_t end;
};

void cold_config_default(cold_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->warning_lead_ms = 20000;
    }
}

void cold_profile_default(cold_profile_t *profile)
{
    if (profile != NULL) {
        profile->warm_s = 120;
        profile->cold_s = 90;
        profile->after_s = 300;
        profile->voice = true;
        profile->music = true;
        profile->calm = true;
    }
}

bool cold_profile_valid(const cold_profile_t *p)
{
    return p != NULL && p->warm_s <= 1800 && p->cold_s >= 15 && p->cold_s <= 600 && p->after_s >= 30 &&
           p->after_s <= 1800;
}

cold_protocol_t *cold_protocol_create(const cold_config_t *cfg, uint32_t now_ms)
{
    (void)now_ms;
    if (cfg == NULL || cfg->warning_lead_ms < 5000 || cfg->warning_lead_ms > 60000) {
        return NULL;
    }
    cold_protocol_t *cp = calloc(1, sizeof(*cp));
    if (cp == NULL) {
        return NULL;
    }
    cp->cfg = *cfg;
    cold_profile_default(&cp->profile);
    cp->out.phase = COLD_IDLE;
    return cp;
}

void cold_protocol_destroy(cold_protocol_t *cp)
{
    free(cp);
}

/* ---- helpers ----------------------------------------------------------- */

static uint32_t clip_len(const cold_protocol_t *cp, cold_cue_id_t id)
{
    return cp->profile.voice ? cp->clip_ms[id] : 0;
}

static void issue_cue(cold_protocol_t *cp, cold_cue_id_t id, bool beep)
{
    cp->cue.id = id;
    cp->cue.beep = beep;
    cp->cue_pending = true;
}

static void issue_scene(cold_protocol_t *cp, cold_scene_t scene)
{
    cp->scene = scene;
    cp->scene_pending = true;
}

static cold_phase_t first_timed_phase(const cold_profile_t *p)
{
    return p->warm_s > 0 ? COLD_WARM : COLD_COLD;
}

static cold_scene_t scene_of(cold_phase_t phase)
{
    switch (phase) {
    case COLD_WARM:
        return COLD_SCENE_WARM;
    case COLD_COLD:
        return COLD_SCENE_COLD;
    default:
        return COLD_SCENE_AFTER;
    }
}

static void plan_cues(cold_protocol_t *cp, cold_cue_id_t final_id)
{
    memset(cp->cues, 0, sizeof(cp->cues));
    const uint32_t len = cp->phase_len;

    uint32_t final_len = clip_len(cp, final_id);
    if (final_len > len) {
        final_len = 0; /* does not fit: a beep at the change */
    }
    cp->cues[1].planned = true;
    cp->cues[1].offset_ms = len - final_len;
    cp->cues[1].cue.id = final_id;
    cp->cues[1].cue.beep = final_len == 0;

    const uint32_t lead = cp->cfg.warning_lead_ms;
    const uint32_t warn_len = clip_len(cp, COLD_CUE_WARNING);
    if (len > lead + warn_len && warn_len + final_len <= lead) {
        cp->cues[0].planned = true;
        cp->cues[0].offset_ms = len - lead;
        cp->cues[0].cue.id = COLD_CUE_WARNING;
        cp->cues[0].cue.beep = warn_len == 0;
    }
}

/* Begin a timed phase (WARM, COLD, AFTER) at start_ms. */
static void begin_phase(cold_protocol_t *cp, cold_phase_t phase, uint32_t start_ms, bool scene)
{
    cp->out.phase = phase;
    cp->phase_start = start_ms;
    memset(cp->cues, 0, sizeof(cp->cues));
    switch (phase) {
    case COLD_WARM:
        cp->phase_len = (uint32_t)cp->profile.warm_s * 1000u;
        cp->out.music = COLD_MUSIC_NONE;
        plan_cues(cp, COLD_CUE_TO_COLD);
        break;
    case COLD_COLD:
        cp->phase_len = (uint32_t)cp->profile.cold_s * 1000u;
        cp->out.music = cp->profile.music ? COLD_MUSIC_HYPE : COLD_MUSIC_NONE;
        plan_cues(cp, COLD_CUE_DONE);
        break;
    default: /* COLD_AFTER: no cues */
        cp->phase_len = (uint32_t)cp->profile.after_s * 1000u;
        cp->out.music = COLD_MUSIC_CALM;
        break;
    }
    if (scene) {
        issue_scene(cp, scene_of(phase));
    }
}

static void finish(cold_protocol_t *cp, cold_end_t reason)
{
    cp->out.phase = COLD_IDLE;
    cp->out.music = COLD_MUSIC_NONE;
    cp->out.remaining_ms = 0;
    cp->profile_waiting = false;
    memset(cp->cues, 0, sizeof(cp->cues));
    cp->end = reason;
    cp->end_pending = true;
}

/* Leave the current phase at start_ms and begin the following one. */
static void change_phase(cold_protocol_t *cp, uint32_t start_ms)
{
    if (cp->profile_waiting) {
        cp->profile = cp->new_profile;
        cp->profile_waiting = false;
    }
    switch (cp->out.phase) {
    case COLD_COUNTDOWN: {
        const cold_phase_t first = first_timed_phase(&cp->profile);
        begin_phase(cp, first, start_ms, first != cp->start_scene_phase);
        break;
    }
    case COLD_WARM:
        begin_phase(cp, COLD_COLD, start_ms, true);
        break;
    case COLD_COLD:
        if (cp->profile.calm) {
            begin_phase(cp, COLD_AFTER, start_ms, true);
        } else {
            finish(cp, COLD_END_FINISHED);
        }
        break;
    default:
        finish(cp, COLD_END_FINISHED);
        break;
    }
}

static void update(cold_protocol_t *cp, uint32_t now_ms)
{
    while (cp->out.phase != COLD_IDLE) {
        const uint32_t elapsed = now_ms - cp->phase_start;
        int next_cue = -1;
        for (int i = 0; i < CUE_SLOTS; i++) {
            const planned_cue_t *c = &cp->cues[i];
            if (c->planned && !c->issued && c->offset_ms <= elapsed &&
                (next_cue < 0 || c->offset_ms < cp->cues[next_cue].offset_ms)) {
                next_cue = i;
            }
        }
        if (next_cue >= 0) {
            planned_cue_t *c = &cp->cues[next_cue];
            c->issued = true;
            issue_cue(cp, c->cue.id, c->cue.beep);
            continue;
        }
        if (elapsed >= cp->phase_len) {
            change_phase(cp, cp->phase_start + cp->phase_len);
            continue;
        }
        break;
    }
    if (cp->out.phase != COLD_IDLE) {
        cp->out.remaining_ms = cp->phase_len - (now_ms - cp->phase_start);
    } else {
        cp->out.remaining_ms = 0;
    }
}

/* ---- API --------------------------------------------------------------- */

bool cold_protocol_start(cold_protocol_t *cp, const cold_profile_t *profile, const uint32_t clip_ms[COLD_CUE_COUNT],
                         uint32_t now_ms)
{
    if (cp == NULL || cp->out.phase != COLD_IDLE || !cold_profile_valid(profile)) {
        return false;
    }
    cp->cue_pending = cp->scene_pending = cp->end_pending = false;
    cp->profile = *profile;
    cp->profile_waiting = false;
    for (int i = 0; i < COLD_CUE_COUNT; i++) {
        cp->clip_ms[i] = clip_ms != NULL ? clip_ms[i] : 0;
    }
    const cold_phase_t first = first_timed_phase(&cp->profile);
    cp->start_scene_phase = first;
    const uint32_t go_len = clip_len(cp, COLD_CUE_GO);
    issue_cue(cp, COLD_CUE_GO, go_len == 0);
    if (go_len > 0) {
        cp->out.phase = COLD_COUNTDOWN;
        cp->out.music = COLD_MUSIC_NONE;
        cp->phase_start = now_ms;
        cp->phase_len = go_len;
        memset(cp->cues, 0, sizeof(cp->cues));
        issue_scene(cp, scene_of(first));
    } else {
        begin_phase(cp, first, now_ms, true);
    }
    update(cp, now_ms);
    return true;
}

bool cold_protocol_set_profile(cold_protocol_t *cp, const cold_profile_t *profile)
{
    if (cp == NULL || cp->out.phase == COLD_IDLE || !cold_profile_valid(profile)) {
        return false;
    }
    cp->new_profile = *profile;
    cp->profile_waiting = true;
    return true;
}

bool cold_protocol_next(cold_protocol_t *cp, uint32_t now_ms)
{
    if (cp == NULL) {
        return false;
    }
    update(cp, now_ms);
    switch (cp->out.phase) {
    case COLD_IDLE:
        return false;
    case COLD_COUNTDOWN:
        issue_cue(cp, COLD_CUE_GO, true);
        break;
    case COLD_WARM:
        issue_cue(cp, COLD_CUE_TO_COLD, true);
        break;
    case COLD_COLD:
        issue_cue(cp, COLD_CUE_DONE, true);
        break;
    default:
        break;
    }
    change_phase(cp, now_ms);
    update(cp, now_ms);
    return true;
}

bool cold_protocol_abort(cold_protocol_t *cp, uint32_t now_ms)
{
    if (cp == NULL) {
        return false;
    }
    update(cp, now_ms);
    if (cp->out.phase == COLD_IDLE) {
        return false;
    }
    cp->cue_pending = false;
    cp->scene_pending = false;
    finish(cp, COLD_END_ABORTED);
    return true;
}

void cold_protocol_tick(cold_protocol_t *cp, uint32_t now_ms)
{
    if (cp != NULL) {
        update(cp, now_ms);
    }
}

const cold_outputs_t *cold_protocol_outputs(const cold_protocol_t *cp)
{
    return cp ? &cp->out : NULL;
}

bool cold_protocol_take_cue(cold_protocol_t *cp, cold_cue_t *out)
{
    if (cp == NULL || out == NULL || !cp->cue_pending) {
        return false;
    }
    *out = cp->cue;
    cp->cue_pending = false;
    return true;
}

bool cold_protocol_take_scene(cold_protocol_t *cp, cold_scene_t *out)
{
    if (cp == NULL || out == NULL || !cp->scene_pending) {
        return false;
    }
    *out = cp->scene;
    cp->scene_pending = false;
    return true;
}

bool cold_protocol_take_end(cold_protocol_t *cp, cold_end_t *out)
{
    if (cp == NULL || out == NULL || !cp->end_pending) {
        return false;
    }
    *out = cp->end;
    cp->end_pending = false;
    return true;
}
