/*
 * speaker_task – see speaker_task.h.
 */
#include "speaker_task.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "amp_io.h"
#include "amp_volume.h"
#include "enroll_task.h"
#include "espnow_link.h"
#include "events.h"
#include "fade_curve.h"
#include "node_config.h"
#include "player_task.h"
#include "resume_point.h"
#include "resume_restore.h"
#include "speaker_session.h"
#include "tas5825m.h"
#include "volume.h"

#define SPK_PERIOD_MS    20u
#define SPK_TASK_STACK   6144u
#define SPK_TASK_PRIO    5u
#define LINK_QUEUE_LEN   16
#define AMP_RETRY_MS     2000u
#define SD_ROOT          "/sdcard" /* player_task's MOUNT_POINT */
#define RESUME_NVS_NAMESPACE "resume"
#define INITIAL_VOLUME   12u       /* until the bedside's first PLAY / VOLUME */

static const char *TAG = "speaker";

static struct {
    speaker_session_t *ss;
    tas5825m_t *amp;
    volume_t *vol;            /* gain of outputs.volume (lib/volume curve) */
    uint8_t vol_step;
    QueueHandle_t link_q;
    nvs_handle_t resume_nvs;
    bool resume_open;
    int last_code;
    uint32_t amp_retry_at;
} s;

/* ------------------------------------------------------------- folders --- */

/* All folders in the card's root, one LINK_FOLDER frame each (PROTOCOL:
 * FOLDER). A card without folders: one frame with count 0. */
static void send_folders(void)
{
    char names[32][LINK_FOLDER_LEN + 1];
    uint8_t count = 0;
    DIR *d = opendir(SD_ROOT);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && count < 32) {
            if (e->d_type == DT_DIR && e->d_name[0] != '\0' && strlen(e->d_name) <= LINK_FOLDER_LEN &&
                cfg_folder_valid(e->d_name)) {
                strcpy(names[count++], e->d_name);
            }
        }
        closedir(d);
    }
    /* TODO: sort by name so the bedside's next-folder order is stable */
    for (uint8_t i = 0; i < (count ? count : 1); i++) {
        link_msg_t m = {.type = LINK_FOLDER};
        m.u.folder.index = i;
        m.u.folder.count = count;
        if (count) {
            strcpy(m.u.folder.name, names[i]);
        }
        espnow_link_send(&m, true);
    }
    ESP_LOGI(TAG, "%u folders sent", (unsigned)count);
}

/* -------------------------------------------------------------- player --- */

static void save_resume(void)
{
    const spk_outputs_t *o = speaker_session_outputs(s.ss);
    char key[16];
    if (!s.resume_open || !resume_key(o->folder, key, sizeof(key))) {
        return;
    }
    /* TODO(player): ask the player for seed, index, track and byte offset of
     * what plays now, then:
     *   resume_point_t rp;
     *   resume_capture(&rp, o->folder, seed, index, track, offset);
     *   uint8_t blob[RESUME_BLOB_MAX];
     *   size_t n = resume_encode(&rp, blob, sizeof(blob));
     *   nvs_set_blob(s.resume_nvs, key, blob, n); nvs_commit(s.resume_nvs);  */
    ESP_LOGI(TAG, "SAVE %s (resume point not captured yet)", o->folder);
}

static void start_folder(const spk_act_t *a)
{
    /* TODO(player): player_task has no folder API yet. Needed:
     *   player_play_folder(folder, rp_or_NULL) – playlist of SD_ROOT/folder
     *   (lib/playlist with a seed), resume_restore() when rp is given, seek
     *   to rp.offset; reports "nothing to play" -> speaker_session_ended().
     * Resume point: nvs_get_blob(resume_key(folder)) + resume_decode(). */
    ESP_LOGI(TAG, "START %s%s (plays the card's root until the folder API exists)", a->folder,
             a->resume ? " resume" : "");
    player_play(true);
}

static void run_actions(void)
{
    spk_act_t a;
    while (speaker_session_take(s.ss, &a)) {
        switch (a.type) {
        case SPK_ACT_START:
            start_folder(&a);
            break;
        case SPK_ACT_SAVE:
            save_resume();
            break;
        case SPK_ACT_HALT:
            player_play(false);
            break;
        case SPK_ACT_NEXT:
            player_next();
            break;
        }
    }
}

/* ----------------------------------------------------------- amplifier --- */

static void amp_follow(uint32_t now)
{
    const spk_outputs_t *o = speaker_session_outputs(s.ss);
    if (s.amp == NULL || (int32_t)(now - s.amp_retry_at) < 0) {
        return;
    }
    if (o->volume != s.vol_step) { /* lib/volume knows the curve: make one at that step */
        volume_config_t vc;
        volume_config_default(&vc);
        volume_t *v = volume_create(&vc, true, o->volume);
        if (v != NULL) {
            volume_destroy(s.vol);
            s.vol = v;
            s.vol_step = o->volume;
        }
    }
    const bool on = tas5825m_is_powered(s.amp);
    if (o->amp_on && !on) {
        if (tas5825m_power_up(s.amp) != TAS5825M_OK) {
            s.amp_retry_at = now + AMP_RETRY_MS;
            return;
        }
        s.last_code = -1;
    } else if (!o->amp_on && on) {
        tas5825m_power_down(s.amp);
        return;
    }
    if (!tas5825m_is_powered(s.amp)) {
        return;
    }
    const float fade = fade_curve_gain(speaker_session_level(s.ss, now), FADE_CURVE_FLOOR_DB_DEFAULT);
    const uint8_t code = amp_volume_code_for(volume_gain(s.vol), fade);
    if ((int)code != s.last_code) {
        if (tas5825m_set_volume(s.amp, code) == TAS5825M_OK) {
            s.last_code = code;
        } else {
            s.amp_retry_at = now + AMP_RETRY_MS;
        }
    }
    /* TODO: fault polling as in crate_task / control_task */
}

/* -------------------------------------------------------------- inputs --- */

static void handle_link(uint32_t now)
{
    link_msg_t m;
    while (xQueueReceive(s.link_q, &m, 0) == pdTRUE) {
        switch (m.type) {
        case LINK_AUDIO:
            speaker_session_audio(s.ss, &m.u.audio, now);
            break;
        case LINK_HELLO:
            if (m.u.hello.node == LINK_NODE_BEDSIDE) {
                send_folders();
            }
            break;
        case LINK_RING:
            /* TODO(alarm): armed_alarm + speaker_session_ring() */
            break;
        default:
            break;
        }
    }
}

static void handle_events(uint32_t now)
{
    event_t ev;
    while (events_receive(&ev, 0)) {
        switch (ev.type) {
        case EVT_LONG_PRESS:
            /* TODO(alarm): ringing -> stop the alarm first (it has priority) */
            if (!enroll_task_button_held()) {
                speaker_session_standby(s.ss, now);
            }
            break;
        case EVT_BUTTON: /* TODO(alarm): snooze while ringing */
        default:
            break;
        }
    }
}

static void on_link_rx(const link_msg_t *msg)
{
    xQueueSend(s.link_q, msg, 0); /* espnow task: copy and go */
}

/* ---------------------------------------------------------------- task --- */

static void speaker_task(void *arg)
{
    (void)arg;
    send_folders(); /* the bedside may already be up */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SPK_PERIOD_MS));
        const uint32_t now = app_now_ms();
        handle_link(now);
        handle_events(now);
        speaker_session_tick(s.ss, now);
        run_actions();
        amp_follow(now);
        espnow_link_set_heartbeat_state(0); /* TODO: armed_state_t */
    }
}

void speaker_task_start(void)
{
    const uint32_t now = app_now_ms();
    s.ss = speaker_session_create(INITIAL_VOLUME, now);
    s.amp = tas5825m_create(amp_io_get());
    s.link_q = xQueueCreate(LINK_QUEUE_LEN, sizeof(link_msg_t));
    s.resume_open = nvs_open(RESUME_NVS_NAMESPACE, NVS_READWRITE, &s.resume_nvs) == ESP_OK;
    s.last_code = -1;
    s.vol_step = 0xFF; /* forces a volume object on the first tick */
    if (s.ss == NULL || s.link_q == NULL) {
        ESP_LOGE(TAG, "init failed");
        return;
    }
    if (s.amp == NULL) {
        ESP_LOGE(TAG, "amplifier driver not created: no sound");
    }
    espnow_link_start(LINK_NODE_SPEAKER, LINK_CAP_AUDIO, "speaker", on_link_rx);
    xTaskCreate(speaker_task, "speaker", SPK_TASK_STACK, NULL, SPK_TASK_PRIO, NULL);
}
