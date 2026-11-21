/*
 * speaker_task – see speaker_task.h.
 */
#include "speaker_task.h"

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
#include "server_link.h"
#include "speaker_session.h"
#include "tas5825m.h"
#include "volume.h"

#define SPK_PERIOD_MS        20u
#define SPK_TASK_STACK       6144u
#define SPK_TASK_PRIO        5u
#define LINK_QUEUE_LEN       16
#define AMP_RETRY_MS         2000u
#define FAULT_POLL_MS        2000u
#define FOLDER_CHECK_MS      5000u
#define RESUME_NVS_NAMESPACE "resume"
#define INITIAL_VOLUME       12u /* until the bedside's first PLAY / VOLUME */

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
    uint32_t next_fault_poll;
    uint32_t folders_hash;    /* of the list sent last */
    uint32_t next_folder_check;
    spk_state_t last_state;
} s;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static speaker_status_t s_status;

/* ------------------------------------------------------------- folders --- */

static uint32_t list_hash(char names[][PLAYER_FOLDER_MAX + 1], size_t n)
{
    uint32_t h = 0x811c9dc5u ^ (uint32_t)n;
    for (size_t i = 0; i < n; i++) {
        for (const char *p = names[i]; *p != '\0'; p++) {
            h = (h ^ (uint8_t)*p) * 0x01000193u;
        }
        h = (h ^ '/') * 0x01000193u;
    }
    return h;
}

/* The root folders the bedside may name, one LINK_FOLDER frame each
 * (PROTOCOL: FOLDER); a card without such folders sends count 0. */
static void send_folders(void)
{
    char all[PLAYER_MAX_FOLDERS][PLAYER_FOLDER_MAX + 1];
    const size_t found = player_folders(all, PLAYER_MAX_FOLDERS); /* sorted */
    char names[PLAYER_MAX_FOLDERS][PLAYER_FOLDER_MAX + 1];
    size_t n = 0;
    for (size_t i = 0; i < found; i++) {
        if (cfg_folder_valid(all[i]) && strlen(all[i]) <= LINK_FOLDER_LEN) {
            memcpy(names[n++], all[i], sizeof(names[0]));
        }
    }
    for (uint8_t i = 0; i < (n ? n : 1); i++) {
        link_msg_t m = {.type = LINK_FOLDER};
        m.u.folder.index = i;
        m.u.folder.count = (uint8_t)n;
        if (n) {
            strncpy(m.u.folder.name, names[i], LINK_FOLDER_LEN); /* NUL-padded, no NUL when full */
        }
        espnow_link_send(&m, true);
    }
    s.folders_hash = list_hash(names, n);
    ESP_LOGI(TAG, "%u folders sent", (unsigned)n);
}

static void check_folders(uint32_t now)
{
    if ((int32_t)(now - s.next_folder_check) < 0) {
        return;
    }
    s.next_folder_check = now + FOLDER_CHECK_MS;
    char all[PLAYER_MAX_FOLDERS][PLAYER_FOLDER_MAX + 1];
    char names[PLAYER_MAX_FOLDERS][PLAYER_FOLDER_MAX + 1];
    const size_t found = player_folders(all, PLAYER_MAX_FOLDERS);
    size_t n = 0;
    for (size_t i = 0; i < found; i++) {
        if (cfg_folder_valid(all[i]) && strlen(all[i]) <= LINK_FOLDER_LEN) {
            memcpy(names[n++], all[i], sizeof(names[0]));
        }
    }
    if (list_hash(names, n) != s.folders_hash) {
        send_folders(); /* a music sync or a swapped card changed them */
        server_link_status_changed();
    }
}

/* -------------------------------------------------------------- player --- */

static bool load_resume(const char *folder, resume_point_t *rp)
{
    char key[16];
    uint8_t blob[RESUME_BLOB_MAX];
    size_t len = sizeof(blob);
    return s.resume_open && resume_key(folder, key, sizeof(key)) &&
           nvs_get_blob(s.resume_nvs, key, blob, &len) == ESP_OK && resume_decode(blob, len, rp);
}

static void save_resume(void)
{
    player_pos_t pos;
    player_position(&pos);
    const spk_outputs_t *o = speaker_session_outputs(s.ss);
    char key[16];
    if (!pos.playing || !pos.keep || strcmp(pos.folder, o->folder) != 0 || !s.resume_open ||
        !resume_key(pos.folder, key, sizeof(key))) {
        return; /* nothing of that folder is playing (or not a KEEP session) */
    }
    resume_point_t rp;
    uint8_t blob[RESUME_BLOB_MAX];
    size_t n = 0;
    if (resume_capture(&rp, pos.folder, pos.seed, pos.index, pos.track, pos.offset)) {
        n = resume_encode(&rp, blob, sizeof(blob));
    }
    if (n == 0) {
        ESP_LOGW(TAG, "resume point of %s not saved (track name too long?)", pos.folder);
        return;
    }
    if (nvs_set_blob(s.resume_nvs, key, blob, n) == ESP_OK && nvs_commit(s.resume_nvs) == ESP_OK) {
        ESP_LOGI(TAG, "resume point: %s/%s at %lu", pos.folder, pos.track, (unsigned long)pos.offset);
    }
}

static void start_folder(const spk_act_t *a)
{
    const bool kept = speaker_session_outputs(s.ss)->kept;
    resume_point_t rp;
    if (a->resume && load_resume(a->folder, &rp)) {
        player_play_folder(a->folder, true, &rp);
    } else {
        player_play_folder(a->folder, kept, NULL); /* no point yet: a fresh shuffle */
    }
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
    if (!tas5825m_is_powered(s.amp) || s.vol == NULL) {
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
    if ((int32_t)(now - s.next_fault_poll) >= 0) {
        s.next_fault_poll = now + FAULT_POLL_MS;
        tas5825m_faults_t f;
        if (tas5825m_read_faults(s.amp, &f) == TAS5825M_OK && tas5825m_faults_any(&f)) {
            ESP_LOGW(TAG, "amplifier fault: chan 0x%02X global1 0x%02X global2 0x%02X, clearing", f.chan, f.global1,
                     f.global2);
            tas5825m_clear_faults(s.amp);
        }
    }
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
            /* not yet: armed_alarm + speaker_session_ring() (Open items) */
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
            /* not yet: a ringing alarm is stopped first (it has priority) */
            if (!enroll_task_button_held()) {
                speaker_session_standby(s.ss, now);
            }
            break;
        default:
            break; /* EVT_BUTTON: snooze while ringing, with the alarm */
        }
    }
}

static void on_link_rx(const link_msg_t *msg)
{
    xQueueSend(s.link_q, msg, 0); /* espnow task: copy and go */
}

static void update_status(void)
{
    const spk_outputs_t *o = speaker_session_outputs(s.ss);
    speaker_status_t st = {
        .playing = o->state == SPK_ALARM ? STATUS_PLAY_ALARM
                 : (o->state == SPK_PLAYING || o->state == SPK_FADING_OUT) ? STATUS_PLAY_MUSIC
                                                                           : STATUS_PLAY_NONE,
        .kept = o->kept,
        .volume = o->volume,
        .link_up = espnow_link_peer_up(),
    };
    strlcpy(st.folder, o->state == SPK_IDLE ? "" : o->folder, sizeof(st.folder));
    portENTER_CRITICAL(&s_status_lock);
    const bool changed = st.playing != s_status.playing || st.kept != s_status.kept ||
                         st.volume != s_status.volume || st.link_up != s_status.link_up ||
                         strcmp(st.folder, s_status.folder) != 0;
    s_status = st;
    portEXIT_CRITICAL(&s_status_lock);
    if (changed) {
        server_link_status_changed();
    }
}

void speaker_task_status(speaker_status_t *out)
{
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

/* ---------------------------------------------------------------- task --- */

static void speaker_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SPK_PERIOD_MS));
        const uint32_t now = app_now_ms();
        handle_link(now);
        handle_events(now);
        if (player_take_ended()) {
            speaker_session_ended(s.ss, now); /* the folder had nothing playable */
        }
        speaker_session_tick(s.ss, now);
        run_actions();
        amp_follow(now);
        check_folders(now);
        update_status();
        espnow_link_set_heartbeat_state(0); /* armed_state_t, with the alarm */
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
    s.vol_step = 0xFF;           /* forces a volume object on the first tick */
    s.folders_hash = 0xFFFFFFFFu; /* the first check sends the list */
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
