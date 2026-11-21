/*
 * Firmware entry point.
 *
 * The task layout is described in docs/ARCHITECTURE.md. Hardware-independent
 * logic lives in lib/ and is developed test-first against the native
 * environment (pio test -e native).
 *
 * Tasks: mic_task (double clap → EVT_CLAP), input_task (button, encoder),
 * control_task (mode, volume, outputs, amplifier), player_task (microSD →
 * MP3 → I2S), hue_task (bridge over HTTPS), console_task (settings, status),
 * plus Wi-Fi in the background, enroll_task (pairing with the server) and
 * mqtt_link (status, settings, commands).
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "console_task.h"
#include "control_task.h"
#include "enroll_task.h"
#include "events.h"
#include "hue_task.h"
#include "input_task.h"
#include "mic_task.h"
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
    /* TODO: status_json_bath() from app_status + settings -> MQTT_TOPIC_STATUS,
     * then every 60 s and on change (at most every 2 s) from a small timer;
     * reported settings (lib/settings_sync) -> MQTT_TOPIC_REPORTED;
     * hue scene list -> MQTT_TOPIC_HUE_SCENES. */
}

static void on_cmd(const char *name, const char *payload, size_t len)
{
    if (strcmp(name, "music/sync") == 0) {
        music_sync_start(payload, len);
    } else {
        /* TODO: mode, cold/start|next|abort, scene/test, hue/scenes,
         * voice/test, ota, log/verbose, identify, ack (docs/PROTOCOL.md);
         * post to control_task instead of acting in the MQTT task. */
        ESP_LOGI(TAG, "cmd/%s not handled yet", name);
    }
}

static void on_desired(const char *payload, size_t len)
{
    (void)payload;
    (void)len;
    /* TODO: lib/settings_sync: apply to net / bath groups (cfg_store) and the
     * v1 keys (app_settings_set), then publish reported. */
}

static const mqtt_link_hooks_t s_hooks = {
    .node = "bath",
    .groups = {&cfg_net_table, &cfg_bath_table},
    .group_count = 2,
    .on_connect = on_connect,
    .on_cmd = on_cmd,
    .on_desired = on_desired,
};

void app_main(void)
{
    nvs_start();
    const esp_err_t err = app_settings_load();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings not loaded from NVS (%s), using defaults", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(events_init() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_start();          /* first: the hardware RNG is truly random once the radio runs */
    enroll_task_start("bath"); /* loads the net group; pairs if unclaimed */
    mqtt_link_start(&s_hooks); /* idle until claimed */
    hue_task_start();      /* queues exist before control_task sends to them */
    player_task_start();
    control_task_start();
    input_task_start();
    mic_task_start();
    console_task_start();
}
