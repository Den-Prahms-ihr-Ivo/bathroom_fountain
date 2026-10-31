/*
 * control_task – owns mode_controller, volume, outputs and the tas5825m
 * driver; turns events into output changes (BEH-*, AUD-05/06/07).
 *
 * Every cycle (an event, or 20 ms without one):
 *   clap / button → mode_controller_trigger(); encoder → volume_turn()
 *   mode_controller_tick(now)
 *   outputs_want_from_mode() → outputs_plan() → carry out each action in
 *     order, outputs_done() on success, stop at the first failure
 *   Hue command → hue_task (latest wins)
 *   volume_take_save() → NVS
 *   every 2 s while the amplifier is on: read its fault registers, log and
 *     clear any fault
 *
 * Music start/stop goes to player_task (and PIN_LED_MUSIC on the DevKit).
 * The amplifier runs through amp_io: real I2C on the Louder, logging on the
 * DevKit. A failed amplifier action is retried every 2 s, not every cycle.
 */
#ifndef CONTROL_TASK_H
#define CONTROL_TASK_H

/* Starts the task (core 1). Settings and events must be ready. */
void control_task_start(void);

#endif /* CONTROL_TASK_H */
