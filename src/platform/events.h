/*
 * events – the one queue from the input side (mic_task, input_task,
 * expander_io, server commands) to the app's control loop.
 */
#ifndef EVENTS_H
#define EVENTS_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"

typedef enum {
    EVT_CLAP = 0,  /* confirmed double clap */
    EVT_BUTTON,    /* short press */
    EVT_VOLUME,    /* encoder turned by `detents` (positive = louder) */
    EVT_LONG_PRESS, /* button held 3 s: the app decides (pairing, Hue pairing, ...) */
    EVT_COLD_PRESS, /* bath: cold button short press (expander_io)          */
    EVT_COLD_HOLD,  /* bath: cold button held 2 s = abort                    */
    EVT_COLD_SWITCH,/* bath: hype/zen switch moved; detents 1 = hype, 0 = zen */
    EVT_REMOTE,     /* a server command for the control loop; detents = remote_cmd_t */
} event_type_t;

/* EVT_REMOTE codes (cmd/... topics that change what the control loop does). */
typedef enum {
    REMOTE_MODE_AMBIENT = 1, /* cmd/mode {"mode":"ambient"} */
    REMOTE_MODE_NORMAL,      /* cmd/mode {"mode":"normal"}  */
    REMOTE_COLD_START,       /* cmd/cold/start              */
    REMOTE_COLD_NEXT,        /* cmd/cold/next               */
    REMOTE_COLD_ABORT,       /* cmd/cold/abort              */
    REMOTE_VOICE_GO,         /* cmd/voice/test {"cue":...}: REMOTE_VOICE_GO + cold_cue_id_t */
} remote_cmd_t;

typedef struct {
    event_type_t type;
    uint32_t time_ms;  /* app_now_ms() when posted */
    int32_t detents;   /* EVT_VOLUME: detents; EVT_COLD_SWITCH: 1/0; EVT_REMOTE: remote_cmd_t */
} event_t;

/* The firmware's millisecond clock (wraps after ~49 days, which every lib/
 * module handles). */
static inline uint32_t app_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Create the queue. Call once before any task posts or receives. */
bool events_init(void);

/* Never blocks; a full queue drops the event (logged) and returns false. */
bool events_post(event_type_t type, int32_t detents);

/* Wait up to timeout_ms for the next event. */
bool events_receive(event_t *ev, uint32_t timeout_ms);

#endif /* EVENTS_H */
