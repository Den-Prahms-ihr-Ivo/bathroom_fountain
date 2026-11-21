/*
 * control_task – owns mode_controller, the per-mode volume, outputs, the
 * tas5825m driver and the cold shower; turns events into output changes
 * (BEH-*, AUD-05/06/07/13/14, COLD-*).
 *
 * Every cycle (an event, or 20 ms without one):
 *   clap / button → mode_controller_trigger(); encoder → mode_volume_turn()
 *     on the slot bath_volume_mode() picks (cold run → cold volume)
 *   cold button / cmd/cold/... → cold_protocol (docs/ARCHITECTURE.md, Cold
 *     shower): start from Normal (enters Ambient first) or Ambient, next,
 *     abort (button held 2 s, or the main button during a run)
 *   mode_controller_tick, cold_protocol_tick
 *   outputs_want_from_mode() → outputs_plan() → carry out each action in
 *     order, outputs_done() on success, stop at the first failure
 *   music: Ambient plays the default folder ("A", or the root on a v1 card);
 *     during a run the protocol's music: none, hype "B", calm "C" (empty →
 *     "A")
 *   Hue: mode_controller's AMBIENT / NORMAL, or during a run the profile's
 *     scenes by id (hue_task_recall_id; "" → scene_ambient); a mode_controller
 *     command during a run is dropped
 *   voice: the protocol's cues → player_voice_cue (clip or beep)
 *   cold_session: one record per run that reached the cold phase →
 *     server_link_record (kept until the server acknowledges it)
 *   mode_volume_take_save() → NVS
 *   every 2 s while the amplifier is on: read its fault registers, log and
 *     clear any fault
 *
 * The amplifier runs through amp_io: real I2C on the Louder, logging on the
 * DevKit. A failed amplifier action is retried every 2 s, not every cycle.
 */
#ifndef CONTROL_TASK_H
#define CONTROL_TASK_H

#include <stdbool.h>
#include <stdint.h>

#include "node_config.h"
#include "status_json.h"

/* The bath settings group (loaded from NVS on the first call). The pointer
 * is what server_link's store writes (under cfg_store_lock). */
cfg_bath_t *control_task_bath(void);

/* Starts the task (core 1). Settings and events must be ready. */
void control_task_start(void);

/* Snapshot for the status document (any task). */
typedef struct {
    status_bath_mode_t mode;
    uint8_t volume_ambient;
    uint8_t volume_cold;
    bool cold_running;
    const char *cold_phase;   /* "countdown", "warm", "cold", "after" */
    uint32_t cold_remaining_s;
    const char *cold_profile; /* "hype" / "zen" */
    const char *cold_switch;  /* "hype" / "zen", or "none" without expander */
} control_status_t;

void control_task_status(control_status_t *out);

#endif /* CONTROL_TASK_H */
