/*
 * crate_task – the holy crate's control loop (ESP-IDF glue around
 * lib/crate_controller): lid contact, battery ADC, LED PWM, amplifier,
 * volume encoder, player and deep sleep.
 */
#ifndef CRATE_TASK_H
#define CRATE_TASK_H

#include "status_json.h"

/* Call after events_init() and player_task_start(). */
void crate_task_start(void);

/* Snapshot for the status document (sd and tracks are filled by the caller). */
void crate_task_status(status_crate_t *out);

#endif /* CRATE_TASK_H */
