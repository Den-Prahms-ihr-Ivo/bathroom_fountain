/*
 * bed_task – see bed_task.h.
 */
#include "bed_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_settings.h"
#include "bed_audio.h"
#include "board_pins.h"
#include "button.h"
#include "cfg_store.h"
#include "enroll_task.h"
#include "espnow_link.h"
#include "events.h"
#include "folder_list.h"
#include "hue_task.h"
#include "mode_volume.h"
#include "node_config.h"
#include "server_link.h"

#define BED_PERIOD_MS    20u
#define BED_TASK_STACK   6144u
#define BED_TASK_PRIO    5u
#define LINK_QUEUE_LEN   16
#define PRESS_QUEUE_LEN  4
#define STATE_NVS_NAMESPACE "state"
#define NIGHT_NVS_KEY       "night"
#define CLOCK_VALID_AFTER   1767225600 /* 2026-01-01: before that SNTP has not answered */

static const char *TAG = "bedside";

static const struct {
    int pin;
    bed_button_t id;
    const char *name;
} BUTTONS[] = {
    {PIN_BTN_MEDITATION, BED_BTN_MEDITATION, "meditation"},
    {PIN_BTN_MOOD, BED_BTN_MOOD, "mood"},
    {PIN_BTN_SLEEP, BED_BTN_SLEEP, "sleep"},
    {PIN_BTN_NEXT, BED_BTN_NEXT, "next"},
    {PIN_BTN_STOP, BED_BTN_STOP, "stop"},
};
#define BUTTON_COUNT (sizeof(BUTTONS) / sizeof(BUTTONS[0]))
#define STOP_INDEX   4

static cfg_bed_t s_bed;          /* the "bed" settings group (cfg_store) */
static bool s_bed_loaded;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static bed_status_t s_status;

static struct {
    bed_modes_t *bm;
    mode_volume_t *mv;
    folder_list_t folders;
    button_t *buttons[BUTTON_COUNT];
    QueueHandle_t link_q;       /* link_msg_t from the espnow task */
    QueueHandle_t press_q;      /* bed_button_t from cmd/button */
    nvs_handle_t nvs;
    bool nvs_open;
    uint32_t saved_night;
    bool last_candles;
    bed_mode_t last_mode;
    bool last_fading;
    const char *press_by;       /* "button" / "server" during this tick, else NULL */
    bool sleep_continued;       /* the last sleep PLAY was a resume */
    volatile bool config_dirty; /* the bed group changed: rebuild when idle */
} s;

/* ------------------------------------------------------------- clock --- */

/* SNTP runs in server_link; here only the zone (cfg_bed tz, POSIX form).
 * Not yet: an RTC (DS3231) as time source before SNTP answers. */
static void clock_zone(void)
{
    char tz[sizeof(s_bed.tz)];
    cfg_store_lock();
    memcpy(tz, s_bed.tz, sizeof(tz));
    cfg_store_unlock();
    setenv("TZ", tz, 1);
    tzset();
}

static uint32_t night_now(void)
{
    const time_t t = time(NULL);
    if (t < CLOCK_VALID_AFTER) {
        return BED_NIGHT_UNKNOWN;
    }
    struct tm lt;
    localtime_r(&t, &lt);
    /* local wall-clock time as seconds since 1970 (newlib has no tm_gmtoff):
     * days from the civil date (H. Hinnant's algorithm) + time of day */
    const int64_t y = (int64_t)lt.tm_year + 1900 - (lt.tm_mon < 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = (lt.tm_mon + 10) % 12; /* tm_mon is 0-based */
    const int64_t doy = (153 * mp + 2) / 5 + lt.tm_mday - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    const int64_t local_s = days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    return bed_night_id(local_s);
}

/* ------------------------------------------------------------ settings --- */

cfg_bed_t *bed_task_bed(void)
{
    if (!s_bed_loaded) {
        cfg_store_load(&cfg_bed_table, &s_bed);
        s_bed_loaded = true;
    }
    return &s_bed;
}

void bed_task_settings_changed(void)
{
    s.config_dirty = true;
}

static void save_folder(const char *key, const char *name)
{
    esp_err_t saved;
    if (cfg_store_set(&cfg_bed_table, &s_bed, key, name, &saved) == KV_OK) {
        ESP_LOGI(TAG, "%s = %s", key, name);
        server_link_settings_changed_locally(); /* the web app shows the new folder */
    }
}

static void mode_event(const char *mode, const char *by, int continued, const char *folder)
{
    char json[160];
    if (status_json_event_mode(mode, by, continued, folder, server_link_utc(), json, sizeof(json)) > 0) {
        server_link_event(json);
    }
    server_link_status_changed();
}

static const char *mode_name(bed_mode_t m)
{
    switch (m) {
    case BED_MODE_MEDITATION: return "meditation";
    case BED_MODE_MOOD: return "mood";
    case BED_MODE_SLEEP: return "sleep";
    default: return "none";
    }
}

static void save_night(void)
{
    const uint32_t n = bed_modes_sleep_night(s.bm);
    if (n == s.saved_night || !s.nvs_open) {
        return;
    }
    if (nvs_set_u32(s.nvs, NIGHT_NVS_KEY, n) == ESP_OK && nvs_commit(s.nvs) == ESP_OK) {
        s.saved_night = n;
    }
}

static void save_volumes(uint32_t now)
{
    int slot;
    uint8_t step;
    while (mode_volume_take_save(s.mv, now, &slot, &step)) {
        if (s.nvs_open && nvs_set_u8(s.nvs, bed_audio_volume_key(slot), step) == ESP_OK) {
            nvs_commit(s.nvs);
            ESP_LOGI(TAG, "volume %s step %u saved", bed_audio_volume_key(slot), (unsigned)step);
        }
    }
}

/* ------------------------------------------------------------- outputs --- */

static void send_audio(const link_audio_t *a)
{
    link_msg_t m = {.type = LINK_AUDIO};
    m.u.audio = *a;
    if (!espnow_link_send(&m, true)) {
        ESP_LOGW(TAG, "audio cmd %u: link queue full", (unsigned)a->cmd);
    }
}

static void scene_recall(const bed_cmd_t *cmd)
{
    settings_t st;
    app_settings_snapshot(&st, NULL);
    cfg_bed_t bed;
    cfg_store_lock();
    bed = s_bed;
    cfg_store_unlock();
    const char *id = bed_audio_scene(cmd->scene, &bed, st.scene_normal);
    if (id == NULL) {
        return; /* leave the lights */
    }
    ESP_LOGI(TAG, "scene %s over %lu ms", id, (unsigned long)cmd->fade_ms);
    hue_task_recall_id(id, cmd->fade_ms);
}

static void run_cmd(const bed_cmd_t *cmd)
{
    char picked[LINK_FOLDER_LEN + 1];
    cfg_bed_t bed; /* a copy: the server may write the group meanwhile */
    cfg_store_lock();
    bed = s_bed;
    cfg_store_unlock();
    switch (cmd->type) {
    case BED_CMD_SCENE:
        scene_recall(cmd);
        return;
    case BED_CMD_MOOD_FOLDER_NEXT:
        if (bed_audio_next_folder(&s.folders, bed.mood_folder, BED_PREFIX_MOOD, picked, sizeof(picked))) {
            save_folder("mood_folder", picked);
            mode_event("mood", s.press_by != NULL ? s.press_by : "button", -1, picked);
        }
        return;
    case BED_CMD_SLEEP_FOLDER_NEXT:
        if (bed_audio_next_folder(&s.folders, bed.sleep_folder, BED_PREFIX_SLEEP, picked, sizeof(picked))) {
            save_folder("sleep_folder", picked);
        }
        return;
    case BED_CMD_PLAY:
        /* the stored folder may be "" or gone from the card */
        if (cmd->folder == BED_MODE_MOOD &&
            bed_audio_pick_folder(&s.folders, bed.mood_folder, BED_PREFIX_MOOD, picked, sizeof(picked)) &&
            strcmp(picked, bed.mood_folder) != 0) {
            save_folder("mood_folder", picked);
            strlcpy(bed.mood_folder, picked, sizeof(bed.mood_folder));
        }
        if (cmd->folder == BED_MODE_SLEEP &&
            bed_audio_pick_folder(&s.folders, bed.sleep_folder, BED_PREFIX_SLEEP, picked, sizeof(picked)) &&
            strcmp(picked, bed.sleep_folder) != 0) {
            save_folder("sleep_folder", picked);
            strlcpy(bed.sleep_folder, picked, sizeof(bed.sleep_folder));
        }
        if (cmd->folder == BED_MODE_SLEEP) {
            s.sleep_continued = cmd->resume;
        }
        break;
    default:
        break;
    }
    bed_audio_ctx_t ctx = {
        .med_folder = bed.med_folder,
        .mood_folder = bed.mood_folder,
        .sleep_folder = bed.sleep_folder,
    };
    for (int i = 0; i < BED_VOL_COUNT; i++) {
        ctx.volume[i] = mode_volume_step(s.mv, i);
    }
    link_audio_t a;
    if (bed_audio_from_cmd(cmd, &ctx, &a)) {
        send_audio(&a);
    } else {
        ESP_LOGW(TAG, "cmd %d refused (no folder?)", (int)cmd->type);
    }
}

static void follow_outputs(void)
{
    const bed_outputs_t *o = bed_modes_outputs(s.bm);
    if (o->candles != s.last_candles) {
        gpio_set_level(PIN_CANDLES, o->candles ? 1 : 0);
        s.last_candles = o->candles;
        server_link_status_changed();
    }
    if (o->mode != s.last_mode) {
        /* a change without a press this tick is a timer running out */
        mode_event(mode_name(o->mode), s.press_by != NULL ? s.press_by : "timer",
                   o->mode == BED_MODE_SLEEP ? (s.sleep_continued ? 1 : 0) : -1, NULL);
        s.last_mode = o->mode;
    }
    if (o->fading != s.last_fading) {
        s.last_fading = o->fading;
        server_link_status_changed();
    }
}

/* -------------------------------------------------------------- inputs --- */

static void press(bed_button_t b, const char *by, uint32_t now)
{
    const bool used = bed_modes_button(s.bm, b, night_now(), now);
    ESP_LOGI(TAG, "button %d%s", (int)b, used ? "" : " ignored");
    if (used) {
        s.press_by = by;
    }
}

static void poll_buttons(uint32_t now)
{
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        const bool down = gpio_get_level(BUTTONS[i].pin) == 0; /* to GND, pull-up */
        switch (button_update(s.buttons[i], down, now)) {
        case BUTTON_SHORT:
            press(BUTTONS[i].id, "button", now);
            break;
        case BUTTON_LONG:
            /* only the stop button has a long press: server pairing */
            if (!enroll_task_button_held()) {
                press(BUTTONS[i].id, "button", now); /* no window open: a long stop is a stop */
            }
            break;
        case BUTTON_NONE:
            break;
        }
    }
    bed_button_t b;
    while (xQueueReceive(s.press_q, &b, 0) == pdTRUE) {
        press(b, "server", now);
    }
}

static void handle_events(uint32_t now)
{
    event_t ev;
    while (events_receive(&ev, 0)) {
        if (ev.type != EVT_VOLUME) {
            continue; /* encoder push: no job yet */
        }
        const int slot = bed_audio_volume_index(bed_modes_outputs(s.bm)->mode);
        if (!mode_volume_turn(s.mv, slot, ev.detents, now)) {
            continue; /* nothing plays: ignored */
        }
        link_audio_t a;
        if (bed_audio_volume_msg(mode_volume_step(s.mv, slot), &a)) {
            send_audio(&a);
        }
        server_link_status_changed();
    }
}

static void handle_link(void)
{
    link_msg_t m;
    while (xQueueReceive(s.link_q, &m, 0) == pdTRUE) {
        switch (m.type) {
        case LINK_FOLDER:
            if (folder_list_feed(&s.folders, &m.u.folder) && folder_list_ready(&s.folders)) {
                ESP_LOGI(TAG, "speaker has %u folders", (unsigned)folder_list_count(&s.folders));
            }
            break;
        case LINK_RING_STATE:
            /* not yet: alarm_ring / bed_modes_alarm() (Open items) */
            break;
        default:
            break;
        }
    }
}

/* Runs in the espnow task: copy and go. */
static void on_link_rx(const link_msg_t *msg)
{
    xQueueSend(s.link_q, msg, 0);
}

/* ------------------------------------------------------ config, status --- */

static void modes_config(bed_modes_config_t *mc)
{
    cfg_bed_t bed;
    cfg_store_lock();
    bed = s_bed;
    cfg_store_unlock();
    bed_modes_config_default(mc);
    mc->med_ms = (uint32_t)bed.med_min * 60000u;
    mc->med_fade_ms = (uint32_t)bed.med_fade_s * 1000u;
    mc->sleep_ms = (uint32_t)bed.sleep_timer_min * 60000u;
    mc->sleep_fade_ms = (uint32_t)bed.sleep_fade_s * 1000u;
    mc->sleep_candle_ms = (uint32_t)bed.sleep_candle_min * 60000u;
}

/* New timer settings: bed_modes has no reconfigure, so it is made again –
 * only while no mode runs (the sleep night is carried over). Folders and
 * scenes are read at every use and need nothing. */
static void rebuild_if_idle(uint32_t now)
{
    if (!s.config_dirty || bed_modes_outputs(s.bm)->mode != BED_MODE_NONE) {
        return;
    }
    s.config_dirty = false;
    clock_zone();
    bed_modes_config_t mc;
    modes_config(&mc);
    bed_modes_t *bm = bed_modes_create(&mc, bed_modes_sleep_night(s.bm), now);
    if (bm == NULL) {
        ESP_LOGW(TAG, "new timer settings refused (out of range?), old ones kept");
        return;
    }
    bed_modes_destroy(s.bm);
    s.bm = bm;
    ESP_LOGI(TAG, "timer settings applied");
}

static void update_status(void)
{
    const bed_outputs_t *o = bed_modes_outputs(s.bm);
    bed_status_t st = {
        .mode = o->mode,
        .fading = o->fading,
        .candles = o->candles,
        .speaker_up = espnow_link_peer_up(),
    };
    for (int i = 0; i < BED_VOL_COUNT; i++) {
        st.volume[i] = mode_volume_step(s.mv, i);
    }
    portENTER_CRITICAL(&s_status_lock);
    const bool link_changed = st.speaker_up != s_status.speaker_up;
    s_status = st;
    portEXIT_CRITICAL(&s_status_lock);
    if (link_changed) {
        server_link_status_changed();
    }
}

void bed_task_status(bed_status_t *out)
{
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

/* ---------------------------------------------------------------- task --- */

static void bed_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BED_PERIOD_MS));
        const uint32_t now = app_now_ms();
        poll_buttons(now);
        handle_events(now);
        handle_link();
        bed_modes_tick(s.bm, now);
        bed_cmd_t cmd;
        while (bed_modes_take_cmd(s.bm, &cmd)) {
            run_cmd(&cmd);
        }
        follow_outputs();
        save_night();
        save_volumes(now);
        rebuild_if_idle(now);
        update_status();
        s.press_by = NULL;
    }
}

static void gpio_init(void)
{
    uint64_t in = 0;
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        in |= 1ULL << BUTTONS[i].pin;
    }
    const gpio_config_t buttons = {
        .pin_bit_mask = in,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&buttons));
    const gpio_config_t candles = {
        .pin_bit_mask = 1ULL << PIN_CANDLES,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&candles));
    gpio_set_level(PIN_CANDLES, 0);
}

void bed_task_start(void)
{
    const uint32_t now = app_now_ms();
    bed_task_bed();
    folder_list_init(&s.folders);
    gpio_init();

    s.nvs_open = nvs_open(STATE_NVS_NAMESPACE, NVS_READWRITE, &s.nvs) == ESP_OK;
    s.saved_night = BED_NIGHT_UNKNOWN;
    if (s.nvs_open) {
        nvs_get_u32(s.nvs, NIGHT_NVS_KEY, &s.saved_night);
    }

    bed_modes_config_t mc;
    modes_config(&mc);
    s.bm = bed_modes_create(&mc, s.saved_night, now);
    if (s.bm == NULL) { /* stored values out of range for bed_modes: defaults */
        bed_modes_config_default(&mc);
        s.bm = bed_modes_create(&mc, s.saved_night, now);
    }

    volume_config_t vc;
    volume_config_default(&vc);
    bool has[BED_VOL_COUNT] = {false};
    uint8_t saved[BED_VOL_COUNT] = {0};
    for (int i = 0; i < BED_VOL_COUNT; i++) {
        has[i] = s.nvs_open && nvs_get_u8(s.nvs, bed_audio_volume_key(i), &saved[i]) == ESP_OK;
    }
    s.mv = mode_volume_create(&vc, BED_VOL_COUNT, has, saved);

    button_config_t bc;
    button_config_default(&bc);
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        button_config_t c = bc;
        if (i != STOP_INDEX) {
            c.long_press_ms = 0; /* only stop has a 3 s hold */
        }
        s.buttons[i] = button_create(&c, now);
    }

    s.link_q = xQueueCreate(LINK_QUEUE_LEN, sizeof(link_msg_t));
    s.press_q = xQueueCreate(PRESS_QUEUE_LEN, sizeof(bed_button_t));
    if (s.bm == NULL || s.mv == NULL || s.link_q == NULL || s.press_q == NULL) {
        ESP_LOGE(TAG, "init failed");
        return;
    }
    for (size_t i = 0; i < BUTTON_COUNT; i++) {
        if (s.buttons[i] == NULL) {
            ESP_LOGE(TAG, "button init failed");
            return;
        }
    }
    clock_zone();
    espnow_link_start(LINK_NODE_BEDSIDE, LINK_CAP_LIGHT, "bedside", on_link_rx);
    xTaskCreate(bed_task, "bed", BED_TASK_STACK, NULL, BED_TASK_PRIO, NULL);
}

void bed_task_press(bed_button_t button)
{
    if (s.press_q != NULL) {
        xQueueSend(s.press_q, &button, 0);
    }
}
