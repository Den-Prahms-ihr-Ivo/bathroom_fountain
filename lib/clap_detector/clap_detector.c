/*
 * clap_detector – streaming double-clap detector.
 *
 * The contract is in clap_detector.h, the tests in test/test_clap_detector/.
 *
 * How it works
 * ------------
 * The input is cut into fixed 5 ms frames aligned to absolute sample indices
 * (so results do not depend on block size). For each frame:
 *
 *   level       RMS in dBFS.
 *   background  follows the level down immediately and up slowly (BG_RISE_DB_PER_S),
 *               so steady noise like a shower is absorbed within a few seconds.
 *   onset       level >= background + onset_threshold_db AND level rose by at
 *               least onset_threshold_db over the quietest of the previous
 *               RISE_FRAMES frames. The "sharp rise" part keeps a background
 *               that is still being adapted to from producing onsets.
 *               After an onset no new onset is accepted for EVENT_MS (the clap's
 *               own decay and early reflections); the event's peak is the
 *               highest level in that window.
 *
 * The last three onsets are kept. Each new onset B forms a candidate pair with
 * the previous onset A if the gap fits; the pair is cancelled by any onset up
 * to B + W_after and otherwise validated against the remaining rules when that
 * window has passed.
 */
#include "clap_detector.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MS          5u
#define EVENT_MS          50u    /* energy this soon after an onset belongs to it */
#define RISE_FRAMES       3u     /* onset rise is measured against these frames   */
#define BG_RISE_DB_PER_S  10.0f
#define LEVEL_FLOOR_DBFS  (-120.0f)
#define MAX_ONSETS        3u     /* onset before A, A, B */

typedef struct {
    uint64_t sample;
    float peak_db;
    float min_after_db; /* lowest level after this onset's event, until the next onset */
} onset_t;

struct clap_detector {
    clap_config_t cfg;

    /* Derived from cfg at create. */
    uint32_t frame_len;
    uint32_t event_len;
    float bg_rise_per_frame;

    /* Frame accumulation. */
    uint64_t samples;     /* total samples processed */
    uint64_t frame_start;
    uint64_t sum_sq;
    uint32_t frame_fill;

    /* Level tracking. */
    bool started;
    float background_db;
    float history_db[RISE_FRAMES];
    uint32_t history_pos;

    /* Onsets, oldest first; onsets[n_onsets - 1] is the latest. */
    onset_t onsets[MAX_ONSETS];
    uint32_t n_onsets;
    uint64_t event_end; /* first sample after the latest onset's event */

    /* Candidate pair (onsets[n-2], onsets[n-1]) waiting for its quiet-after window. */
    bool pending;
    uint64_t confirm_at;
};

void clap_config_default(clap_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->sample_rate_hz = 16000;
        cfg->min_gap_ms = 150;          /* CLAP-02 */
        cfg->max_gap_ms = 600;          /* CLAP-02 */
        cfg->max_level_diff_db = 6.0f;  /* CLAP-03 */
        cfg->quiet_before_ms = 300;     /* CLAP-04 */
        cfg->quiet_after_ms = 400;      /* CLAP-04 */
        cfg->quiet_gap_factor = 1.5f;   /* CLAP-04 */
        cfg->min_drop_db = 12.0f;       /* CLAP-05 */
        cfg->onset_threshold_db = 12.0f; /* CLAP-06 */
        cfg->min_peak_dbfs = -40.0f;    /* CLAP-13 */
    }
}

static bool config_valid(const clap_config_t *cfg)
{
    return cfg->sample_rate_hz != 0
        && cfg->min_gap_ms < cfg->max_gap_ms
        && cfg->max_level_diff_db >= 0.0f
        && cfg->quiet_gap_factor >= 0.0f
        && cfg->min_drop_db > 0.0f
        && cfg->onset_threshold_db > 0.0f;
}

static uint32_t ms_to_samples(const clap_detector_t *det, uint32_t ms)
{
    return (uint32_t)((uint64_t)ms * det->cfg.sample_rate_hz / 1000u);
}

/* max(min_ms, quiet_gap_factor * gap) in samples, rounded up. */
static uint64_t quiet_window(const clap_detector_t *det, uint32_t min_ms, uint64_t gap)
{
    const float min_samples = (float)min_ms * (float)det->cfg.sample_rate_hz / 1000.0f;
    const float scaled = det->cfg.quiet_gap_factor * (float)gap;
    return (uint64_t)ceilf(scaled > min_samples ? scaled : min_samples);
}

static bool gap_in_range(const clap_detector_t *det, uint64_t gap)
{
    const uint64_t gap_x1000 = gap * 1000u;
    return gap_x1000 >= (uint64_t)det->cfg.min_gap_ms * det->cfg.sample_rate_hz
        && gap_x1000 <= (uint64_t)det->cfg.max_gap_ms * det->cfg.sample_rate_hz;
}

/* CLAP-03/04/05/13 for the pair (A, B) = the last two onsets. */
static bool pair_valid(const clap_detector_t *det)
{
    const clap_config_t *c = &det->cfg;
    const onset_t *a = &det->onsets[det->n_onsets - 2];
    const onset_t *b = &det->onsets[det->n_onsets - 1];
    const uint64_t gap = b->sample - a->sample;

    if (a->peak_db < c->min_peak_dbfs || b->peak_db < c->min_peak_dbfs) {
        return false; /* CLAP-13 */
    }
    if (fabsf(a->peak_db - b->peak_db) > c->max_level_diff_db) {
        return false; /* CLAP-03 */
    }
    if (a->peak_db - a->min_after_db < c->min_drop_db) {
        return false; /* CLAP-05 */
    }
    if (det->n_onsets == MAX_ONSETS) {
        const uint64_t before = a->sample - det->onsets[0].sample;
        if (before <= quiet_window(det, c->quiet_before_ms, gap)) {
            return false; /* CLAP-04, quiet before; quiet after is the pending window */
        }
    }
    return true;
}

static void add_onset(clap_detector_t *det, uint64_t sample, float level_db)
{
    if (det->n_onsets == MAX_ONSETS) {
        memmove(&det->onsets[0], &det->onsets[1], (MAX_ONSETS - 1) * sizeof(det->onsets[0]));
        det->n_onsets--;
    }
    onset_t *o = &det->onsets[det->n_onsets++];
    o->sample = sample;
    o->peak_db = level_db;
    o->min_after_db = level_db;
    det->event_end = sample + det->event_len;

    /* Any onset inside the quiet-after window cancels the pending pair. */
    if (det->pending && sample <= det->confirm_at) {
        det->pending = false;
    }

    if (det->n_onsets >= 2) {
        const uint64_t gap = sample - det->onsets[det->n_onsets - 2].sample;
        if (gap_in_range(det, gap)) {
            det->pending = true;
            det->confirm_at = sample + quiet_window(det, det->cfg.quiet_after_ms, gap);
        }
    }
}

/* Handle one complete frame; returns true and fills *ev if a pair is confirmed. */
static bool process_frame(clap_detector_t *det, float level_db, clap_event_t *ev)
{
    const uint64_t start = det->frame_start;
    const uint64_t end = start + det->frame_len;

    if (!det->started) {
        det->started = true;
        det->background_db = level_db;
        for (uint32_t i = 0; i < RISE_FRAMES; i++) {
            det->history_db[i] = level_db;
        }
    }

    const bool in_event = det->n_onsets > 0 && start < det->event_end;
    if (in_event) {
        onset_t *o = &det->onsets[det->n_onsets - 1];
        if (level_db > o->peak_db) {
            o->peak_db = level_db;
        }
    } else {
        float recent_min = det->history_db[0];
        for (uint32_t i = 1; i < RISE_FRAMES; i++) {
            if (det->history_db[i] < recent_min) {
                recent_min = det->history_db[i];
            }
        }
        const float thr = det->cfg.onset_threshold_db;
        if (level_db >= det->background_db + thr && level_db - recent_min >= thr) {
            add_onset(det, start, level_db);
        } else if (det->n_onsets > 0) {
            onset_t *o = &det->onsets[det->n_onsets - 1];
            if (level_db < o->min_after_db) {
                o->min_after_db = level_db;
            }
        }
    }

    /* Background: follow down immediately, up slowly (CLAP-06). */
    if (level_db < det->background_db) {
        det->background_db = level_db;
    } else {
        const float rise = level_db - det->background_db;
        det->background_db += (rise < det->bg_rise_per_frame) ? rise : det->bg_rise_per_frame;
    }

    det->history_db[det->history_pos] = level_db;
    det->history_pos = (det->history_pos + 1u) % RISE_FRAMES;

    if (det->pending && end >= det->confirm_at) {
        det->pending = false;
        if (pair_valid(det)) {
            ev->first_onset_sample = det->onsets[det->n_onsets - 2].sample;
            ev->second_onset_sample = det->onsets[det->n_onsets - 1].sample;
            ev->confirmed_sample = end;
            return true;
        }
    }
    return false;
}

clap_detector_t *clap_detector_create(const clap_config_t *cfg)
{
    if (cfg == NULL || !config_valid(cfg)) {
        return NULL;
    }

    clap_detector_t *det = calloc(1, sizeof(*det));
    if (det == NULL) {
        return NULL;
    }

    det->cfg = *cfg;
    det->frame_len = ms_to_samples(det, FRAME_MS);
    if (det->frame_len == 0) {
        det->frame_len = 1;
    }
    det->event_len = ms_to_samples(det, EVENT_MS);
    det->bg_rise_per_frame = BG_RISE_DB_PER_S * (float)det->frame_len / (float)cfg->sample_rate_hz;
    return det;
}

void clap_detector_destroy(clap_detector_t *det)
{
    free(det);
}

void clap_detector_reset(clap_detector_t *det)
{
    if (det == NULL) {
        return;
    }
    const clap_config_t cfg = det->cfg;
    const uint32_t frame_len = det->frame_len;
    const uint32_t event_len = det->event_len;
    const float bg_rise_per_frame = det->bg_rise_per_frame;

    memset(det, 0, sizeof(*det));
    det->cfg = cfg;
    det->frame_len = frame_len;
    det->event_len = event_len;
    det->bg_rise_per_frame = bg_rise_per_frame;
}

size_t clap_detector_process(clap_detector_t *det,
                             const int16_t *pcm, size_t n_samples,
                             clap_event_t *events, size_t max_events)
{
    if (det == NULL || pcm == NULL) {
        return 0;
    }

    size_t n_events = 0;
    for (size_t i = 0; i < n_samples; i++) {
        const int32_t s = pcm[i];
        det->sum_sq += (uint64_t)(s * s);
        det->frame_fill++;
        det->samples++;

        if (det->frame_fill == det->frame_len) {
            const float mean_sq = (float)det->sum_sq / (float)det->frame_len;
            float level_db = 10.0f * log10f(mean_sq / (32768.0f * 32768.0f));
            if (!(level_db > LEVEL_FLOOR_DBFS)) { /* also catches -inf for silence */
                level_db = LEVEL_FLOOR_DBFS;
            }

            clap_event_t ev;
            if (process_frame(det, level_db, &ev) && n_events < max_events && events != NULL) {
                events[n_events++] = ev;
            }

            det->frame_start += det->frame_len;
            det->sum_sq = 0;
            det->frame_fill = 0;
        }
    }
    return n_events;
}

uint64_t clap_detector_samples_processed(const clap_detector_t *det)
{
    return (det != NULL) ? det->samples : 0;
}
