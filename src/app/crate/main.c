/*
 * crate – the holy crate (Louder-ESP32-S3 Plus on a 12.8 V LiFePO4 battery):
 * lid open = light and choir, lid closed = fade out and deep sleep.
 *
 * Tasks: crate_task (lid, battery, light, amplifier, sleep), player_task
 * (microSD -> MP3 -> I2S), input_task (volume encoder, press = next track).
 * No Wi-Fi. The plan is in docs/CRATE.md; the decisions are in
 * lib/crate_controller.
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "board_pins.h"
#include "crate_task.h"
#include "enroll_task.h"
#include "events.h"
#include "input_task.h"
#include "mqtt_link.h"
#include "music_sync.h"
#include "node_config.h"
#include "player_task.h"
#include "wifi.h"

static const char *TAG = "main";

static void nvs_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition unusable (%s), erasing it", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* ------------------------------------------------------------ server --- */

static void on_connect(void)
{
    /* TODO: status_json_crate -> MQTT_TOPIC_STATUS (on connect, on change,
     * every 60 s): state, light, playing, volume, battery, sd, tracks. */
}

static void on_cmd(const char *name, const char *payload, size_t len)
{
    if (strcmp(name, "music/sync") == 0) {
        music_sync_start(payload, len); /* busy keeps the closed crate awake */
        return;
    }
    /* TODO: identify (pulse the strip), ota, log/verbose */
    ESP_LOGI(TAG, "cmd/%s not handled yet", name);
}

static void on_desired(const char *payload, size_t len)
{
    (void)payload;
    (void)len;
    /* TODO: lib/settings_sync -> net group */
}

static const mqtt_link_hooks_t s_hooks = {
    .node = "crate",
    .groups = {&cfg_net_table},
    .group_count = 1,
    .on_connect = on_connect,
    .on_cmd = on_cmd,
    .on_desired = on_desired,
};

void app_main(void)
{
    nvs_start();
    if (app_settings_load() != ESP_OK) { /* Wi-Fi credentials (console / secrets.h) */
        ESP_LOGE(TAG, "settings not loaded, using defaults");
    }
    ESP_ERROR_CHECK(events_init() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_start();               /* online whenever awake */
    enroll_task_start("crate");
    mqtt_link_start(&s_hooks);
    player_task_start();
    crate_task_start();   /* amplifier, light and lid before the encoder posts */
    input_task_start();
}
