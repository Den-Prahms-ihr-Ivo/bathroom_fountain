/*
 * bedside – bedside node (ESP32-S3): owns time, alarms, lights, buttons, display; game master.
 *
 * Working now: the five mode buttons, candles, per-mode encoder volume, the
 * ESP-NOW link to the speaker, Hue scenes by id, pairing, status, settings
 * and events over server_link. Not yet: alarm, light ramp, display, game
 * (docs/ARCHITECTURE.md, Open items).
 *
 * Tasks: bed_task (modes, buttons, link), input_task (encoder), espnow
 * (link_peer), hue_task, enroll (pairing), server_link + esp-mqtt, Wi-Fi.
 */
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "bed_task.h"
#include "cfg_store.h"
#include "board_pins.h"
#include "enroll_task.h"
#include "events.h"
#include "hue_task.h"
#include "input_task.h"
#include "node_config.h"
#include "server_link.h"
#include "status_json.h"
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

static size_t build_status(const status_common_t *c, char *out, size_t out_size)
{
    bed_status_t st;
    bed_task_status(&st);
    status_bedside_t bs;
    memset(&bs, 0, sizeof(bs));
    char local[24];
    const int64_t utc = server_link_utc();
    if (utc > 0) {
        const time_t t = (time_t)utc;
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(local, sizeof(local), "%Y-%m-%dT%H:%M:%S", &lt);
        bs.time = local;
        bs.time_source = STATUS_TIME_SNTP;
    } else {
        bs.time = NULL;
        bs.time_source = STATUS_TIME_NONE;
    }
    bs.mode = st.mode == BED_MODE_MEDITATION ? STATUS_BED_MEDITATION
            : st.mode == BED_MODE_MOOD       ? STATUS_BED_MOOD
            : st.mode == BED_MODE_SLEEP      ? STATUS_BED_SLEEP
                                             : STATUS_BED_NONE;
    bs.fading = st.fading;
    bs.candles = st.candles;
    bs.volume_meditation = st.volume[BED_VOL_MEDITATION];
    bs.volume_mood = st.volume[BED_VOL_MOOD];
    bs.volume_sleep = st.volume[BED_VOL_SLEEP];
    bs.remaining_s = -1;  /* not yet: bed_modes has no remaining-time query */
    bs.speaker_up = st.speaker_up;
    /* not yet (Open items): alarm, light, game */
    bs.armed_switch = false;
    bs.has_next_alarm = false;
    bs.alarm = "idle";
    bs.light = 0;
    bs.game_state = "off";
    bs.game_hits = 0;
    bs.game_needed = 0;
    return status_json_bedside(c, &bs, out, out_size);
}

static void json_string(const char *payload, size_t len, const char *member, char *out, size_t out_size)
{
    out[0] = '\0';
    cJSON *root = cJSON_ParseWithLength(payload, len);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, member);
    if (cJSON_IsString(v)) {
        strlcpy(out, v->valuestring, out_size);
    }
    cJSON_Delete(root);
}

static bool on_cmd(const char *name, const char *payload, size_t len)
{
    char arg[40];
    if (strcmp(name, "button") == 0) {
        static const struct {
            const char *name;
            bed_button_t b;
        } map[] = {
            {"meditation", BED_BTN_MEDITATION}, {"mood", BED_BTN_MOOD}, {"sleep", BED_BTN_SLEEP},
            {"next", BED_BTN_NEXT},             {"stop", BED_BTN_STOP},
        };
        json_string(payload, len, "name", arg, sizeof(arg));
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (strcmp(arg, map[i].name) == 0) {
                bed_task_press(map[i].b);
                return true;
            }
        }
        server_link_cmd_result(name, false, "invalid");
        return true;
    }
    if (strcmp(name, "hue/scenes") == 0) {
        hue_task_publish_scenes();
        return true;
    }
    if (strcmp(name, "hue/pair") == 0) {
        hue_task_request_pairing(); /* press the bridge's link button first */
        return true;
    }
    if (strcmp(name, "scene/test") == 0) {
        json_string(payload, len, "scene", arg, sizeof(arg));
        hue_task_recall_id(arg, 1000);
        return true;
    }
    /* not yet (Open items): alarm/stop, alarm/snooze, game/..., link/pair,
     * identify, ota, log/verbose */
    return false;
}

static void on_connect(void)
{
    hue_task_publish_scenes();
}

static void on_settings(const char *group)
{
    if (strcmp(group, "bed") == 0) {
        bed_task_settings_changed();
    }
}

static const char *const V1_KEYS[] = {"wifi_ssid", "wifi_pass", "hue_host", "hue_key", "scene_normal", NULL};

static server_app_t s_app = {
    .node = "bedside",
    .v1_settings = true,
    .v1_keys = V1_KEYS,
    .group_count = 2,
    /* .groups set in app_main */
    .build_status = build_status,
    .on_cmd = on_cmd,
    .on_connect = on_connect,
    .on_settings = on_settings,
};

static cfg_game_t s_game; /* the game group: stored and synced, used once the game exists */

void app_main(void)
{
    nvs_start();
    if (app_settings_load() != ESP_OK) {
        ESP_LOGE(TAG, "settings not loaded, using defaults");
    }
    ESP_ERROR_CHECK(events_init() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_start(); /* before ESP-NOW and the random secret */
    enroll_task_start("bedside");
    cfg_store_load(&cfg_game_table, &s_game);
    s_app.groups[0].table = &cfg_bed_table;
    s_app.groups[0].data = bed_task_bed();
    s_app.groups[1].table = &cfg_game_table;
    s_app.groups[1].data = &s_game;
    server_link_start(&s_app); /* also starts SNTP */
    hue_task_set_scene_sink(server_link_hue_scenes);
    hue_task_start();
    bed_task_start();
    input_task_start(); /* encoder -> EVT_VOLUME */
}
