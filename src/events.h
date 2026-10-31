/*
 * events – the one queue from the input side (mic_task, input_task) to
 * control_task.
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
} event_type_t;

typedef struct {
    event_type_t type;
    uint32_t time_ms;  /* app_now_ms() when posted */
    int32_t detents;   /* EVT_VOLUME only */
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
