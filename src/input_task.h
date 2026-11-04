/*
 * input_task – push button and rotary encoder to events (BEH-04, AUD-05,
 * NFR-10).
 *
 * Every 10 ms:
 *   button (PIN_BUTTON, to GND, pull-up) through the button module
 *     short press → EVT_BUTTON
 *     long press (3 s) → Hue pairing (press the bridge's link button first)
 *   encoder (PIN_ENC_A / PIN_ENC_B) through the PCNT unit
 *     whole detents since the last poll → EVT_VOLUME (positive = louder)
 *
 * ENC_COUNTS_PER_DETENT is 4 for the usual EC11 (one quadrature cycle per
 * click). If one click moves the volume by 2 steps, or only every second
 * click counts, change it to 2. If the direction is reversed, swap the A and
 * B wires or define ENC_REVERSE.
 */
#ifndef INPUT_TASK_H
#define INPUT_TASK_H

#include <stdbool.h>

/* Starts the task (core 1). events_init() must have run. */
void input_task_start(void);

/* Raw PCNT count of the encoder (4 per click on an EC11), for the console's
 * "io" command. Returns false if the board has no encoder or it failed. */
bool input_encoder_raw(int *count);

#endif /* INPUT_TASK_H */
