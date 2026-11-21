/*
 * bed_modes – the bedside's music modes.
 * Contract in bed_modes.h, tests in test/test_bed_modes/.
 */
#include "bed_modes.h"

#include <stdlib.h>
#include <string.h>

#define TWELVE_HOURS_MS (12u * 60u * 60u * 1000u)

typedef struct {
    bool active;
    uint32_t deadline; /* wrapping ms */
} timer_t_;

struct bed_modes {
    bed_modes_config_t cfg;
    bed_outputs_t out;

    timer_t_ mode_timer;   /* meditation / sleep timer */
    timer_t_ fade_timer;   /* closing fade of the current mode */
    timer_t_ candle_timer; /* sleep candles */
    uint32_t ref_ms;       /* time the state is up to date with */

    uint32_t night;        /* night of the last known first press (or saved) */
    bool have_first_ms;
    uint32_t first_ms;     /* now_ms of the last first press in this life */

    bool ringing;

    bed_cmd_t q[BED_CMDS];
    size_t q_head;
    size_t q_len;
};

/* ------------------------------------------------------------------------ */

void bed_modes_config_default(bed_modes_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->med_ms = 1200000;
    cfg->med_fade_ms = 30000;
    cfg->sleep_ms = 1800000;
    cfg->sleep_fade_ms = 120000;
    cfg->sleep_candle_ms = 300000;
    cfg->start_fade_ms = 3000;
    cfg->stop_fade_ms = 2000;
}

static bool config_valid(const bed_modes_config_t *c)
{
    return c->med_ms >= 60000 && c->med_ms <= 10800000
        && c->med_fade_ms <= 600000
        && c->sleep_ms <= 14400000
        && c->sleep_fade_ms <= 600000
        && c->sleep_candle_ms <= 7200000
        && c->start_fade_ms <= 10000
        && c->stop_fade_ms <= 10000;
}

bed_modes_t *bed_modes_create(const bed_modes_config_t *cfg, uint32_t saved_night, uint32_t now_ms)
{
    if (cfg == NULL || !config_valid(cfg)) {
        return NULL;
    }
    bed_modes_t *bm = calloc(1, sizeof(*bm));
    if (bm == NULL) {
        return NULL;
    }
    bm->cfg = *cfg;
    bm->out.mode = BED_MODE_NONE;
    bm->ref_ms = now_ms;
    bm->night = saved_night;
    return bm;
}

void bed_modes_destroy(bed_modes_t *bm)
{
    free(bm);
}

/* ------------------------------------------------------------------------ */
/* Command queue                                                            */
/* ------------------------------------------------------------------------ */

static void push(bed_modes_t *bm, const bed_cmd_t *c)
{
    if (bm->q_len == BED_CMDS) { /* full: drop the oldest */
        bm->q_head = (bm->q_head + 1) % BED_CMDS;
        bm->q_len--;
    }
    bm->q[(bm->q_head + bm->q_len) % BED_CMDS] = *c;
    bm->q_len++;
}

static void push_scene(bed_modes_t *bm, bed_scene_t scene, uint32_t fade_ms)
{
    bed_cmd_t c = {.type = BED_CMD_SCENE, .scene = scene, .fade_ms = fade_ms};
    push(bm, &c);
}

static void push_play(bed_modes_t *bm, bed_mode_t folder, bool resume, bool keep)
{
    bed_cmd_t c = {.type = BED_CMD_PLAY,
                   .folder = folder,
                   .resume = resume,
                   .keep = keep,
                   .fade_ms = bm->cfg.start_fade_ms};
    push(bm, &c);
}

static void push_simple(bed_modes_t *bm, bed_cmd_type_t type, uint32_t fade_ms)
{
    bed_cmd_t c = {.type = type, .fade_ms = fade_ms};
    push(bm, &c);
}

bool bed_modes_take_cmd(bed_modes_t *bm, bed_cmd_t *out)
{
    if (bm == NULL || out == NULL || bm->q_len == 0) {
        return false;
    }
    *out = bm->q[bm->q_head];
    bm->q_head = (bm->q_head + 1) % BED_CMDS;
    bm->q_len--;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Timers                                                                   */
/* ------------------------------------------------------------------------ */

static void timer_start(timer_t_ *t, uint32_t from_ms, uint32_t dur_ms)
{
    t->active = true;
    t->deadline = from_ms + dur_ms;
}

static void end_mode(bed_modes_t *bm)
{
    bm->out.mode = BED_MODE_NONE;
    bm->out.fading = false;
    bm->mode_timer.active = false;
    bm->fade_timer.active = false;
}

static void fade_over(bed_modes_t *bm)
{
    if (bm->out.mode == BED_MODE_MEDITATION) {
        bm->out.candles = false;
    }
    end_mode(bm);
}

/* The mode timer ran out at time t. */
static void mode_timer_fired(bed_modes_t *bm, uint32_t t)
{
    bm->mode_timer.active = false;
    uint32_t fade_ms;
    if (bm->out.mode == BED_MODE_MEDITATION) {
        fade_ms = bm->cfg.med_fade_ms;
        push_simple(bm, BED_CMD_STOP, fade_ms);
        push_scene(bm, BED_SCENE_NORMAL, fade_ms);
    } else if (bm->out.mode == BED_MODE_SLEEP) {
        fade_ms = bm->cfg.sleep_fade_ms;
        push_simple(bm, BED_CMD_STOP, fade_ms);
    } else {
        return;
    }
    if (fade_ms == 0) {
        fade_over(bm);
    } else {
        bm->out.fading = true;
        timer_start(&bm->fade_timer, t, fade_ms);
    }
}

/* Run every timer that is due by now_ms, in time order. */
static void catch_up(bed_modes_t *bm, uint32_t now_ms)
{
    for (;;) {
        const uint32_t span = now_ms - bm->ref_ms;
        timer_t_ *timers[3] = {&bm->mode_timer, &bm->fade_timer, &bm->candle_timer};
        int due = -1;
        uint32_t best = 0;
        for (int i = 0; i < 3; i++) {
            if (!timers[i]->active) {
                continue;
            }
            const uint32_t rem = timers[i]->deadline - bm->ref_ms; /* wrap-safe */
            if (rem <= span && (due < 0 || rem < best)) {
                due = i;
                best = rem;
            }
        }
        if (due < 0) {
            break;
        }
        const uint32_t t = timers[due]->deadline;
        bm->ref_ms = t;
        switch (due) {
        case 0:
            mode_timer_fired(bm, t);
            break;
        case 1:
            bm->fade_timer.active = false;
            fade_over(bm);
            break;
        default:
            bm->candle_timer.active = false;
            bm->out.candles = false;
            break;
        }
    }
    bm->ref_ms = now_ms;
}

void bed_modes_tick(bed_modes_t *bm, uint32_t now_ms)
{
    if (bm != NULL) {
        catch_up(bm, now_ms);
    }
}

/* ------------------------------------------------------------------------ */
/* Buttons                                                                  */
/* ------------------------------------------------------------------------ */

/* End whatever runs (no fade) and switch to mode, timers dropped. Candles are
 * set by the caller. */
static void switch_to(bed_modes_t *bm, bed_mode_t mode)
{
    end_mode(bm);
    bm->candle_timer.active = false;
    bm->out.mode = mode;
}

static bool is_continuation(const bed_modes_t *bm, uint32_t night, uint32_t now_ms)
{
    if (night != BED_NIGHT_UNKNOWN) {
        return bm->night != BED_NIGHT_UNKNOWN && night == bm->night;
    }
    return bm->have_first_ms && now_ms - bm->first_ms < TWELVE_HOURS_MS;
}

static void press_sleep(bed_modes_t *bm, uint32_t night, uint32_t now_ms)
{
    const bed_modes_config_t *c = &bm->cfg;

    if (bm->out.mode == BED_MODE_SLEEP && bm->out.fading) {
        bm->out.fading = false;
        bm->fade_timer.active = false;
        push_play(bm, BED_MODE_SLEEP, true, true);
    } else if (bm->out.mode == BED_MODE_SLEEP) {
        /* next sleep folder; switching is not extending: timer keeps running */
        push_simple(bm, BED_CMD_SLEEP_FOLDER_NEXT, 0);
        push_play(bm, BED_MODE_SLEEP, true, true);
        return;
    } else if (is_continuation(bm, night, now_ms)) {
        push_play(bm, BED_MODE_SLEEP, true, true);
        switch_to(bm, BED_MODE_SLEEP);
        bm->out.candles = false;
    } else {
        push_scene(bm, BED_SCENE_SLEEP, c->start_fade_ms);
        push_play(bm, BED_MODE_SLEEP, false, true);
        switch_to(bm, BED_MODE_SLEEP);
        bm->out.candles = c->sleep_candle_ms > 0;
        if (bm->out.candles) {
            timer_start(&bm->candle_timer, now_ms, c->sleep_candle_ms);
        }
        if (night != BED_NIGHT_UNKNOWN) {
            bm->night = night;
        }
        bm->have_first_ms = true;
        bm->first_ms = now_ms;
    }

    bm->mode_timer.active = false;
    if (c->sleep_ms > 0) {
        timer_start(&bm->mode_timer, now_ms, c->sleep_ms);
    }
}

bool bed_modes_button(bed_modes_t *bm, bed_button_t button, uint32_t night, uint32_t now_ms)
{
    if (bm == NULL) {
        return false;
    }
    catch_up(bm, now_ms);
    if (bm->ringing) {
        return false;
    }

    const bed_modes_config_t *c = &bm->cfg;
    bed_outputs_t *o = &bm->out;

    switch (button) {
    case BED_BTN_MEDITATION:
        if (o->mode != BED_MODE_MEDITATION || o->fading) {
            push_scene(bm, BED_SCENE_MEDITATION, c->start_fade_ms);
            push_play(bm, BED_MODE_MEDITATION, false, false);
            switch_to(bm, BED_MODE_MEDITATION);
            o->candles = true;
        }
        timer_start(&bm->mode_timer, now_ms, c->med_ms);
        return true;

    case BED_BTN_MOOD:
        if (o->mode == BED_MODE_MOOD) {
            push_simple(bm, BED_CMD_MOOD_FOLDER_NEXT, 0);
            push_play(bm, BED_MODE_MOOD, false, false);
        } else {
            push_scene(bm, BED_SCENE_MOOD, c->start_fade_ms);
            push_play(bm, BED_MODE_MOOD, false, false);
            switch_to(bm, BED_MODE_MOOD);
            o->candles = true;
        }
        return true;

    case BED_BTN_SLEEP:
        press_sleep(bm, night, now_ms);
        return true;

    case BED_BTN_NEXT:
        if (o->mode == BED_MODE_NONE) {
            return false;
        }
        push_simple(bm, BED_CMD_NEXT, 0);
        return true;

    case BED_BTN_STOP:
        if (o->mode != BED_MODE_NONE) {
            push_simple(bm, BED_CMD_STOP, c->stop_fade_ms);
            end_mode(bm);
        } else if (!o->candles) {
            return false;
        }
        bm->candle_timer.active = false;
        o->candles = false;
        return true;

    default:
        return false;
    }
}

void bed_modes_alarm(bed_modes_t *bm, bool ringing, uint32_t now_ms)
{
    if (bm == NULL) {
        return;
    }
    catch_up(bm, now_ms);
    if (ringing && !bm->ringing) {
        end_mode(bm);
        bm->candle_timer.active = false;
        bm->out.candles = false;
    }
    bm->ringing = ringing;
}

const bed_outputs_t *bed_modes_outputs(const bed_modes_t *bm)
{
    return bm ? &bm->out : NULL;
}

uint32_t bed_modes_sleep_night(const bed_modes_t *bm)
{
    return bm ? bm->night : BED_NIGHT_UNKNOWN;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

uint32_t bed_night_id(int64_t local_s)
{
    if (local_s < 43200) {
        return 0;
    }
    return (uint32_t)((local_s - 43200) / 86400);
}

static int lower(int ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch;
}

static bool has_prefix(const char *s, const char *prefix)
{
    for (; *prefix != '\0'; s++, prefix++) {
        if (*s == '\0' || lower((unsigned char)*s) != lower((unsigned char)*prefix)) {
            return false;
        }
    }
    return true;
}

/* Case-insensitive ASCII order, ties broken by strcmp(). */
static int name_cmp(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (;; pa++, pb++) {
        const int ca = lower(*pa);
        const int cb = lower(*pb);
        if (ca != cb) {
            return ca - cb;
        }
        if (ca == '\0') {
            break;
        }
    }
    return strcmp(a, b);
}

bool bed_next_folder(const char *const *names, size_t count, const char *current, const char *prefix, char *out,
                     size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (names == NULL || current == NULL || prefix == NULL || out == NULL) {
        return false;
    }

    const char *first = NULL; /* smallest of all with the prefix */
    const char *next = NULL;  /* smallest of those after current */
    for (size_t i = 0; i < count; i++) {
        const char *n = names[i];
        if (n == NULL || !has_prefix(n, prefix)) {
            continue;
        }
        if (first == NULL || name_cmp(n, first) < 0) {
            first = n;
        }
        if (name_cmp(n, current) > 0 && (next == NULL || name_cmp(n, next) < 0)) {
            next = n;
        }
    }
    const char *pick = (next != NULL) ? next : first;
    if (pick == NULL) {
        return false;
    }
    const size_t len = strlen(pick);
    if (len + 1 > out_size) {
        return false;
    }
    memcpy(out, pick, len + 1);
    return true;
}
