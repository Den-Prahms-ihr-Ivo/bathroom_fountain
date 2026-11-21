/*
 * bedroom_speaker – bedroom speaker node (Louder-ESP32-S3 Plus): plays sleep sounds, music and the alarm; follower of the bedside node.
 *
 * Skeleton (v2.2/2.3 glue): LINK_AUDIO from the bedside -> speaker_session
 * -> player and amplifier; folder list to the bedside; pairing and MQTT.
 * Alarm sound and the folder / resume API of the player are TODO
 * (speaker_task.h).
 *
 * Tasks: speaker_task, player_task (microSD -> MP3 -> I2S), input_task
 * (button), espnow (link_peer), enroll (pairing), esp-mqtt, Wi-Fi.
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
#include "mqtt_link.h"
#include "music_sync.h"
#include "node_config.h"
#include "player_task.h"
#include "speaker_task.h"
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

static void on_connect(void)
{
    /* TODO: status_json_speaker -> MQTT_TOPIC_STATUS (folders, playing, armed). */
}

static void on_cmd(const char *name, const char *payload, size_t len)
{
    if (strcmp(name, "music/sync") == 0) {
        music_sync_start(payload, len); /* TODO: then a new folder list to the bedside */
        return;
    }
    /* TODO: identify, ota, log/verbose */
    ESP_LOGI(TAG, "cmd/%s not handled yet", name);
}

static void on_desired(const char *payload, size_t len)
{
    (void)payload;
    (void)len;
    /* TODO: lib/settings_sync -> net group */
}

static const mqtt_link_hooks_t s_hooks = {
    .node = "bedroom_speaker",
    .groups = {&cfg_net_table},
    .group_count = 1,
    .on_connect = on_connect,
    .on_cmd = on_cmd,
    .on_desired = on_desired,
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
    mqtt_link_start(&s_hooks);
    player_task_start();
    speaker_task_start();
    input_task_start(); /* button: short / 3 s */
}
