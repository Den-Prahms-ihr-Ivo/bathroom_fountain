/*
 * bed_task – the bedside's control loop (skeleton).
 *
 * Every 20 ms:
 *   buttons   5 GPIOs (meditation, mood, sleep, next, stop) through
 *             lib/button -> bed_modes_button(night = bed_night_id(local)).
 *             Stop held 3 s -> enroll_task_button_held() (server pairing).
 *   encoder   EVT_VOLUME from input_task -> mode_volume (slot of the playing
 *             mode) -> LINK_AUDIO VOLUME to the speaker; saved per mode.
 *   commands  bed_modes_take_cmd() -> SCENE (Hue), folder steps
 *             (cfg_bed mood_folder / sleep_folder, saved), PLAY / STOP /
 *             NEXT -> bed_audio_from_cmd() -> espnow_link_send().
 *   candles   bed_modes_outputs()->candles -> PIN_CANDLES.
 *   link      LINK_FOLDER frames -> folder_list.
 *   night     bed_modes_sleep_night() saved in NVS when it changes.
 *   Hue       SCENE commands -> hue_task_recall_id (bed_audio_scene picks the id)
 *   server    mode events (by button / server / timer), status snapshot,
 *             folder changes reported back as settings.
 *
 * Not yet (docs/ARCHITECTURE.md, Open items): alarm (alarm_schedule,
 * armed_alarm, alarm_ring -> bed_modes_alarm), light ramp, display, game,
 * RTC, remaining_s of the running mode (bed_modes has no query for it).
 */
#ifndef BED_TASK_H
#define BED_TASK_H

#include <stddef.h>

#include "bed_audio.h"
#include "bed_modes.h"
#include "node_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bed settings group (loaded from NVS on the first call); server_link's
 * store writes it under cfg_store_lock. */
cfg_bed_t *bed_task_bed(void);

void bed_task_start(void);

/* The bed group changed (server_link on_settings): new timers are applied
 * when no mode runs; the time zone then too. */
void bed_task_settings_changed(void);

typedef struct {
    bed_mode_t mode;
    bool fading;
    bool candles;
    uint8_t volume[BED_VOL_COUNT]; /* meditation, mood, sleep */
    bool speaker_up;
} bed_status_t;

/* Snapshot for the status document (any task). */
void bed_task_status(bed_status_t *out);

/* cmd/button {"name":"sleep"}: like pressing it. Any task. */
void bed_task_press(bed_button_t button);

#ifdef __cplusplus
}
#endif

#endif /* BED_TASK_H */
