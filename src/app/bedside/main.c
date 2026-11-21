/*
 * bedside – bedside node (ESP32-S3): owns time, alarms, lights, buttons, display; game master.
 *
 * Skeleton (v2.2/2.3 glue): the five mode buttons, candles, per-mode encoder
 * volume, the ESP-NOW link to the speaker, pairing and MQTT. Alarm, light
 * ramp, display and game are TODO (docs/ARCHITECTURE.md).
 *
 * Tasks: bed_task (modes, buttons, link), input_task (encoder), espnow
 * (link_peer), enroll (pairing), esp-mqtt, Wi-Fi.
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "bed_task.h"
#include "board_pins.h"
#include "enroll_task.h"
#include "events.h"
#include "input_task.h"
#include "mqtt_link.h"
#include "node_config.h"
#include "wifi.h"

static const char *TAG = "bedside";

static void nvs_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* ------------------------------------------------------------ server --- */

static void on_connect(void)
{
    /* TODO: status_json_bedside -> MQTT_TOPIC_STATUS; reported settings. */
}

static void on_cmd(const char *name, const char *payload, size_t len)
{
    if (strcmp(name, "button") == 0) {
        char buf[64];
        const size_t n = (payload == NULL) ? 0 : (len < sizeof(buf) - 1 ? len : sizeof(buf) - 1);
        memcpy(buf, payload, n);
        buf[n] = '\0';
        /* {"name":"sleep"} – a strstr is enough for the skeleton; TODO: cJSON */
        static const struct {
            const char *key;
            bed_button_t b;
        } map[] = {
            {"\"meditation\"", BED_BTN_MEDITATION}, {"\"mood\"", BED_BTN_MOOD}, {"\"sleep\"", BED_BTN_SLEEP},
            {"\"next\"", BED_BTN_NEXT},             {"\"stop\"", BED_BTN_STOP},
        };
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (strstr(buf, map[i].key) != NULL) {
                bed_task_press(map[i].b);
                return;
            }
        }
        return;
    }
    /* TODO: alarm/stop, alarm/snooze, scene/test, hue/scenes, game/...,
     * link/pair, identify, ota, log/verbose (docs/PROTOCOL.md). */
    ESP_LOGI(TAG, "cmd/%s not handled yet", name);
}

static void on_desired(const char *payload, size_t len)
{
    (void)payload;
    (void)len;
    /* TODO: lib/settings_sync -> net / bed / game groups (cfg_store), then
     * reported. mood_folder / sleep_folder chosen in the web app land here. */
}

static const mqtt_link_hooks_t s_hooks = {
    .node = "bedside",
    .groups = {&cfg_net_table, &cfg_bed_table, &cfg_game_table},
    .group_count = 3,
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
    enroll_task_start("bedside");
    mqtt_link_start(&s_hooks);
    bed_task_start();
    input_task_start(); /* encoder -> EVT_VOLUME */
}
