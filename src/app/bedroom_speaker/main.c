/*
 * bedroom_speaker – bedroom speaker node (Louder-ESP32-S3 Plus): plays sleep sounds, music and the alarm; follower of the bedside node.
 *
 * LINK_AUDIO from the bedside -> speaker_session -> player (folders, KEEP
 * sessions with resume points) and amplifier; folder list to the bedside;
 * pairing, status, settings and music sync over server_link. The alarm sound
 * is not there yet (speaker_task.h).
 *
 * Tasks: speaker_task, player_task (microSD -> MP3 -> I2S), input_task
 * (button), espnow (link_peer), enroll (pairing), server_link + esp-mqtt,
 * Wi-Fi, music_sync while a sync runs.
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "board_pins.h"
#include "enroll_task.h"
#include "events.h"
#include "input_task.h"
#include "music_sync.h"
#include "player_task.h"
#include "server_link.h"
#include "speaker_task.h"
#include "status_json.h"
#include "wifi.h"

static const char *TAG = "bedroom_speaker";

static void nvs_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static size_t build_status(const status_common_t *c, char *out, size_t out_size)
{
    static char folders[STATUS_MAX_FOLDERS][PLAYER_FOLDER_MAX + 1];
    speaker_status_t st;
    speaker_task_status(&st);
    status_speaker_t sp;
    memset(&sp, 0, sizeof(sp));
    sp.playing = st.playing;
    sp.folder = st.folder;
    sp.kept = st.kept;
    sp.volume = st.volume;
    sp.sd = player_sd_mounted() ? "ok" : "missing";
    sp.folder_count = player_folders(folders, STATUS_MAX_FOLDERS);
    for (size_t i = 0; i < sp.folder_count; i++) {
        sp.folders[i] = folders[i];
    }
    sp.armed_state = "none"; /* with the alarm (Open items) */
    sp.armed_version = 0;
    sp.armed_alarm_utc = -1;
    sp.link_up = st.link_up;
    return status_json_speaker(c, &sp, out, out_size);
}

static bool on_cmd(const char *name, const char *payload, size_t len)
{
    if (strcmp(name, "music/sync") == 0) {
        music_sync_start(payload, len); /* the folder list follows when the card changed */
        return true;
    }
    return false; /* not yet: identify, ota, log/verbose */
}

static const char *const V1_KEYS[] = {"wifi_ssid", "wifi_pass", NULL};

static const server_app_t s_app = {
    .node = "bedroom_speaker",
    .v1_settings = true,
    .v1_keys = V1_KEYS,
    .group_count = 0,
    .build_status = build_status,
    .on_cmd = on_cmd,
};

void app_main(void)
{
    nvs_start();
    if (app_settings_load() != ESP_OK) {
        ESP_LOGE(TAG, "settings not loaded, using defaults");
    }
    ESP_ERROR_CHECK(events_init() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_start(); /* before ESP-NOW and the random secret */
    enroll_task_start("bedroom_speaker");
    server_link_start(&s_app);
    player_task_start();
    speaker_task_start();
    input_task_start(); /* button: short / 3 s */
}
