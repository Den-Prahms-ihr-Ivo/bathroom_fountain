/*
 * speaker_session – the speaker's music session.
 * Contract in speaker_session.h, tests in test/test_speaker_session/.
 */
#include "speaker_session.h"

#include <stdlib.h>
#include <string.h>

#include "node_config.h"

#define VOLUME_MAX 32u /* volume steps, as on the link */

struct speaker_session {
    spk_outputs_t out;

    /* fade: level goes from `from` to `to` over dur_ms from start_ms */
    float from;
    float to;
    uint32_t start_ms;
    uint32_t dur_ms;

    spk_act_t q[SPK_ACTS];
    size_t q_head;
    size_t q_len;
};

static uint8_t clamp_volume(uint8_t v)
{
    return (v > VOLUME_MAX) ? VOLUME_MAX : v;
}

speaker_session_t *speaker_session_create(uint8_t volume, uint32_t now_ms)
{
    speaker_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->out.state = SPK_IDLE;
    s->out.volume = clamp_volume(volume);
    s->start_ms = now_ms;
    return s;
}

void speaker_session_destroy(speaker_session_t *s)
{
    free(s);
}

/* ---- queue ------------------------------------------------------------- */

static void push(speaker_session_t *s, spk_act_type_t type, const char *folder, bool resume)
{
    if (s->q_len == SPK_ACTS) { /* full: drop the oldest */
        s->q_head = (s->q_head + 1) % SPK_ACTS;
        s->q_len--;
    }
    spk_act_t *a = &s->q[(s->q_head + s->q_len) % SPK_ACTS];
    memset(a, 0, sizeof(*a));
    a->type = type;
    if (folder != NULL) {
        strcpy(a->folder, folder);
    }
    a->resume = resume;
    s->q_len++;
}

bool speaker_session_take(speaker_session_t *s, spk_act_t *out)
{
    if (s == NULL || out == NULL || s->q_len == 0) {
        return false;
    }
    *out = s->q[s->q_head];
    s->q_head = (s->q_head + 1) % SPK_ACTS;
    s->q_len--;
    return true;
}

/* ---- fade -------------------------------------------------------------- */

static float level_at(const speaker_session_t *s, uint32_t now_ms)
{
    if (s->out.state != SPK_PLAYING && s->out.state != SPK_FADING_OUT) {
        return 0.0f;
    }
    const uint32_t elapsed = now_ms - s->start_ms; /* wrap-safe */
    if (s->dur_ms == 0 || elapsed >= s->dur_ms) {
        return s->to;
    }
    const double t = (double)elapsed / (double)s->dur_ms;
    return (float)((double)s->from + ((double)s->to - (double)s->from) * t);
}

static void fade(speaker_session_t *s, float to, uint16_t fade_ds, uint32_t now_ms)
{
    s->from = level_at(s, now_ms);
    s->to = to;
    s->start_ms = now_ms;
    s->dur_ms = (uint32_t)fade_ds * 100u;
}

static void go_idle(speaker_session_t *s, spk_state_t state)
{
    s->out.state = state;
    s->out.folder[0] = '\0';
    s->out.kept = false;
    s->out.amp_on = false;
}

/* End whatever plays now: SAVE if a kept session plays, HALT. */
static void halt_now(speaker_session_t *s, spk_state_t next)
{
    if (s->out.state == SPK_PLAYING && s->out.kept) {
        push(s, SPK_ACT_SAVE, NULL, false);
    }
    if (s->out.state == SPK_PLAYING || s->out.state == SPK_FADING_OUT) {
        push(s, SPK_ACT_HALT, NULL, false);
    }
    go_idle(s, next);
}

static void catch_up(speaker_session_t *s, uint32_t now_ms)
{
    if (s->out.state == SPK_FADING_OUT && now_ms - s->start_ms >= s->dur_ms) {
        push(s, SPK_ACT_HALT, NULL, false);
        go_idle(s, SPK_IDLE);
    }
}

void speaker_session_tick(speaker_session_t *s, uint32_t now_ms)
{
    if (s != NULL) {
        catch_up(s, now_ms);
    }
}

/* ---- LINK_AUDIO -------------------------------------------------------- */

static bool on_play(speaker_session_t *s, const link_audio_t *m, uint32_t now_ms)
{
    char folder[LINK_FOLDER_LEN + 1];
    memcpy(folder, m->folder, LINK_FOLDER_LEN);
    folder[LINK_FOLDER_LEN] = '\0';
    if (folder[0] == '\0' || !cfg_folder_valid(folder) || m->volume > VOLUME_MAX) {
        return false;
    }

    const bool resume = (m->flags & LINK_AUDIO_FLAG_RESUME) != 0;
    const bool keep = (m->flags & LINK_AUDIO_FLAG_KEEP) != 0;
    const bool active = s->out.state == SPK_PLAYING || s->out.state == SPK_FADING_OUT;

    if (resume && active && strcmp(folder, s->out.folder) == 0) {
        fade(s, 1.0f, m->fade_ds, now_ms); /* plays on from where it is */
        s->out.state = SPK_PLAYING;
    } else {
        if (s->out.state == SPK_PLAYING && s->out.kept) {
            push(s, SPK_ACT_SAVE, NULL, false);
        }
        push(s, SPK_ACT_START, folder, resume);
        s->out.state = SPK_PLAYING;
        strcpy(s->out.folder, folder);
        s->from = 0.0f;
        s->to = 1.0f;
        s->start_ms = now_ms;
        s->dur_ms = (uint32_t)m->fade_ds * 100u;
    }
    s->out.kept = keep;
    s->out.amp_on = true;
    s->out.volume = m->volume;
    return true;
}

static bool on_stop(speaker_session_t *s, const link_audio_t *m, uint32_t now_ms)
{
    if (s->out.state != SPK_PLAYING && s->out.state != SPK_FADING_OUT) {
        return false;
    }
    if (s->out.state == SPK_PLAYING && s->out.kept) {
        push(s, SPK_ACT_SAVE, NULL, false);
    }
    if (m->fade_ds == 0) {
        push(s, SPK_ACT_HALT, NULL, false);
        go_idle(s, SPK_IDLE);
        return true;
    }
    fade(s, 0.0f, m->fade_ds, now_ms);
    s->out.state = SPK_FADING_OUT;
    return true;
}

bool speaker_session_audio(speaker_session_t *s, const link_audio_t *m, uint32_t now_ms)
{
    if (s == NULL || m == NULL) {
        return false;
    }
    catch_up(s, now_ms);

    if (m->cmd == LINK_AUDIO_VOLUME) {
        if (m->volume > VOLUME_MAX) {
            return false;
        }
        s->out.volume = m->volume;
        return true;
    }
    if (s->out.state == SPK_ALARM) {
        return false;
    }
    switch (m->cmd) {
    case LINK_AUDIO_PLAY:
        return on_play(s, m, now_ms);
    case LINK_AUDIO_STOP:
        return on_stop(s, m, now_ms);
    case LINK_AUDIO_NEXT:
        if (s->out.state != SPK_PLAYING && s->out.state != SPK_FADING_OUT) {
            return false;
        }
        push(s, SPK_ACT_NEXT, NULL, false);
        return true;
    default:
        return false;
    }
}

/* ---- alarm, standby, ended --------------------------------------------- */

void speaker_session_ring(speaker_session_t *s, bool ringing, uint32_t now_ms)
{
    if (s == NULL) {
        return;
    }
    catch_up(s, now_ms);
    const bool was = s->out.state == SPK_ALARM;
    if (ringing && !was) {
        halt_now(s, SPK_ALARM);
    } else if (!ringing && was) {
        go_idle(s, SPK_IDLE);
    }
}

bool speaker_session_standby(speaker_session_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return false;
    }
    catch_up(s, now_ms);
    if (s->out.state == SPK_IDLE || s->out.state == SPK_ALARM) {
        return false;
    }
    halt_now(s, SPK_IDLE);
    return true;
}

bool speaker_session_ended(speaker_session_t *s, uint32_t now_ms)
{
    if (s == NULL) {
        return false;
    }
    catch_up(s, now_ms);
    if (s->out.state == SPK_IDLE || s->out.state == SPK_ALARM) {
        return false;
    }
    go_idle(s, SPK_IDLE);
    return true;
}

const spk_outputs_t *speaker_session_outputs(const speaker_session_t *s)
{
    return (s != NULL) ? &s->out : NULL;
}

float speaker_session_level(const speaker_session_t *s, uint32_t now_ms)
{
    return (s != NULL) ? level_at(s, now_ms) : 0.0f;
}
