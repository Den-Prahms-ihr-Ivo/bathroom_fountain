/*
 * bed_task – the bedside's control loop (skeleton).
 *
 * Every 20 ms:
 *   buttons   5 GPIOs (meditation, mood, sleep, next, stop) through
 *             lib/button -> bed_modes_button(night = bed_night_id(local)).
 *             Stop held 3 s -> enroll_task_button_held() (server pairing).
 *   encoder   EVT_VOLUME from input_task -> mode_volume (slot of the playing
 *             mode) -> LINK_AUDIO VOLUME to the speaker; saved per mode.
 *   commands  bed_modes_take_cmd() -> SCENE (Hue, TODO), folder steps
 *             (cfg_bed mood_folder / sleep_folder, saved), PLAY / STOP /
 *             NEXT -> bed_audio_from_cmd() -> espnow_link_send().
 *   candles   bed_modes_outputs()->candles -> PIN_CANDLES.
 *   link      LINK_FOLDER frames -> folder_list.
 *   night     bed_modes_sleep_night() saved in NVS when it changes.
 *
 * TODO: alarm (alarm_schedule, armed_alarm, alarm_ring -> bed_modes_alarm),
 *       light ramp, display, game, status_json_bedside, events.
 */
#ifndef BED_TASK_H
#define BED_TASK_H

#include <stddef.h>

#include "bed_modes.h"

#ifdef __cplusplus
extern "C" {
#endif

void bed_task_start(void);

/* cmd/button {"name":"sleep"}: like pressing it. Any task. */
void bed_task_press(bed_button_t button);

#ifdef __cplusplus
}
#endif

#endif /* BED_TASK_H */
