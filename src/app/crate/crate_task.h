/*
 * crate_task – the holy crate's control loop (ESP-IDF glue around
 * lib/crate_controller): lid contact, battery ADC, LED PWM, amplifier,
 * volume encoder, player and deep sleep.
 */
#ifndef CRATE_TASK_H
#define CRATE_TASK_H

/* Call after events_init() and player_task_start(). */
void crate_task_start(void);

#endif /* CRATE_TASK_H */
