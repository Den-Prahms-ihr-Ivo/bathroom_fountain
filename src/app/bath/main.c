/*
 * Firmware entry point.
 *
 * The task layout is described in docs/ARCHITECTURE.md. Hardware-independent
 * logic lives in lib/ and is developed test-first against the native
 * environment (pio test -e native).
 *
 * Tasks: mic_task (double clap → EVT_CLAP), input_task (button, encoder),
 * expander_io (cold button, hype/zen switch), control_task (mode, volume,
 * outputs, amplifier, cold shower), player_task (microSD → MP3 → I2S, voice
 * clips), hue_task (bridge over HTTPS), console_task (settings, status),
 * plus Wi-Fi in the background, enroll_task (pairing with the server),
 * server_link / mqtt_link (status, settings, commands, records) and
 * music_sync while a sync runs.
 */
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "cold_protocol.h"
#include "console_task.h"
#include "control_task.h"
#include "enroll_task.h"
#include "events.h"
#include "expander_io.h"
#include "hue_task.h"
#include "input_task.h"
#include "mic_task.h"
#include "music_sync.h"
#include "node_config.h"
#include "player_task.h"
#include "server_link.h"
#include "status_json.h"
#include "wifi.h"

static const char *TAG = "main";

/* Voice clips in the order of cold_cue_id_t (GO, WARNING, TO_COLD, DONE). */
static const char *const VOICE_CLIPS[COLD_CUE_COUNT] = {
    "V/go.wav",
    "V/20s_left.wav",
    "V/turn_to_cold.wav",
    "V/done.wav",
};
static const char *const CUE_NAMES[COLD_CUE_COUNT] = {"go", "warning", "to_cold", "done"};

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

static size_t build_status(const status_common_t *c, char *out, size_t out_size)
{
    control_status_t cs;
    control_task_status(&cs);
    status_bath_t b;
    memset(&b, 0, sizeof(b));
    b.mode = cs.mode;
    b.volume_ambient = cs.volume_ambient;
    b.volume_cold = cs.volume_cold;
    b.hue = hue_task_health();
    b.sd = player_sd_mounted() ? "ok" : "missing";
    static const char *const FOLDERS[] = {"A", "B", "C", "V"};
    for (size_t i = 0; i < sizeof(FOLDERS) / sizeof(FOLDERS[0]); i++) {
        strlcpy(b.tracks[i].name, FOLDERS[i], sizeof(b.tracks[i].name));
        b.tracks[i].count = player_folder_tracks(FOLDERS[i]);
    }
    b.track_count = 4;
    b.cold_running = cs.cold_running;
    b.cold_phase = cs.cold_phase;
    b.cold_remaining_s = cs.cold_remaining_s;
    b.cold_profile = cs.cold_profile;
    b.cold_switch = cs.cold_switch;
    return status_json_bath(c, &b, out, out_size);
}

/* A string member of a small JSON payload, copied into out ("" if missing). */
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

static bool remote(remote_cmd_t code)
{
    return events_post(EVT_REMOTE, (int32_t)code);
}

/* Runs in the MQTT task: decide, post, return. */
static bool on_cmd(const char *name, const char *payload, size_t len)
{
    char arg[24];
    if (strcmp(name, "music/sync") == 0) {
        music_sync_start(payload, len);
    } else if (strcmp(name, "mode") == 0) {
        json_string(payload, len, "mode", arg, sizeof(arg));
        if (strcmp(arg, "ambient") == 0) {
            remote(REMOTE_MODE_AMBIENT);
        } else if (strcmp(arg, "normal") == 0) {
            remote(REMOTE_MODE_NORMAL);
        } else {
            server_link_cmd_result(name, false, "invalid");
        }
    } else if (strcmp(name, "cold/start") == 0) {
        remote(REMOTE_COLD_START);
    } else if (strcmp(name, "cold/next") == 0) {
        remote(REMOTE_COLD_NEXT);
    } else if (strcmp(name, "cold/abort") == 0) {
        remote(REMOTE_COLD_ABORT);
    } else if (strcmp(name, "voice/test") == 0) {
        json_string(payload, len, "cue", arg, sizeof(arg));
        for (size_t i = 0; i < COLD_CUE_COUNT; i++) {
            if (strcmp(arg, CUE_NAMES[i]) == 0) {
                player_voice_cue(i, false); /* a missing clip plays the beep */
                return true;
            }
        }
        server_link_cmd_result(name, false, "invalid");
    } else if (strcmp(name, "hue/scenes") == 0) {
        hue_task_publish_scenes();
    } else if (strcmp(name, "hue/pair") == 0) {
        hue_task_request_pairing();
    } else if (strcmp(name, "scene/test") == 0) {
        char id[40]; /* {"scene":"<uuid>"} */
        json_string(payload, len, "scene", id, sizeof(id));
        hue_task_recall_id(id, 1000);
    } else {
        /* not yet: ota, log/verbose, identify (docs/ARCHITECTURE.md, Open items) */
        return false;
    }
    return true;
}

static void on_connect(void)
{
    hue_task_publish_scenes(); /* the web app's scene pickers */
}

static const server_app_t s_app = {
    .node = "bath",
    .v1_settings = true,
    .group_count = 1,
    /* .groups set in app_main (the bath group lives in control_task) */
    .build_status = build_status,
    .on_cmd = on_cmd,
    .on_connect = on_connect,
    .on_settings = NULL, /* control_task reads the bath group at every cold start */
};
static server_app_t s_app_rw;

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
    s_app_rw = s_app;
    s_app_rw.groups[0].table = &cfg_bath_table;
    s_app_rw.groups[0].data = control_task_bath();
    server_link_start(&s_app_rw); /* MQTT idle until claimed */
    hue_task_set_scene_sink(server_link_hue_scenes);
    hue_task_start();      /* queues exist before control_task sends to them */
    player_set_default_folder("A");
    player_voice_set_clips(VOICE_CLIPS, COLD_CUE_COUNT);
    player_task_start();
    control_task_start();
    input_task_start();
    expander_io_start();
    mic_task_start();
    console_task_start();
}
