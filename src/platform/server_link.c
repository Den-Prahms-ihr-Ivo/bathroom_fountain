/*
 * server_link – see server_link.h.
 */
#include "server_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "app_settings.h"
#include "app_status.h"
#include "cfg_store.h"
#include "enroll_task.h"
#include "events.h"
#include "mqtt_link.h"
#include "node_config.h"
#include "record_outbox.h"
#include "settings.h"
#include "settings_sync.h"
#include "wifi.h"

#define SRV_PERIOD_MS      200u
#define SRV_TASK_STACK     6144u
#define SRV_TASK_PRIO      3u
#define STATUS_MIN_GAP_MS  2000u
#define STATUS_EVERY_MS    60000u
#define REPORT_MAX         6144u
#define DESIRED_MAX        8192u
#define NVS_NS             "srv"
#define CLOCK_VALID_AFTER  1767225600 /* 2026-01-01 */
#define MAX_STORE_GROUPS   (SERVER_MAX_GROUPS + 1) /* net + the app's */

static const char *TAG = "server";

static struct {
    const server_app_t *app;
    SemaphoreHandle_t mx;          /* outbox, pending desired, flags */
    settings_sync_t *sync;
    record_outbox_t *outbox;
    server_group_t groups[MAX_STORE_GROUPS]; /* [0] = net */
    size_t group_count;
    uint32_t boot;
    char *desired;                 /* waiting desired document (malloc), under mx */
    size_t desired_len;
    volatile bool status_dirty;
    volatile bool report_due;
    volatile bool connect_seen;    /* mqtt_link said connected */
    bool was_connected;
    bool outbox_dirty;
    uint32_t changed_groups;       /* bit 0: settings (v1), bit 1 + i: groups[i] */
    bool wifi_changed;
    bool mqtt_changed;
    uint32_t last_status_ms;
    char built[24];
    mqtt_link_hooks_t hooks;
} s;

/* ======================================================= settings store === */

/* The v1 keys in the store: all of lib/settings, or the app's list. */
static size_t v1_count(void)
{
    if (s.app == NULL || !s.app->v1_settings) {
        return 0;
    }
    if (s.app->v1_keys == NULL) {
        return settings_key_count();
    }
    size_t n = 0;
    while (s.app->v1_keys[n] != NULL) {
        n++;
    }
    return n;
}

static const char *v1_name(size_t i)
{
    return s.app->v1_keys == NULL ? settings_key_name(i) : s.app->v1_keys[i];
}

static bool is_v1_key(const char *key)
{
    for (size_t i = 0; i < v1_count(); i++) {
        if (strcmp(v1_name(i), key) == 0) {
            return true;
        }
    }
    return false;
}

/* Group index owning key, or -1. */
static int group_of(const char *key)
{
    for (size_t g = 0; g < s.group_count; g++) {
        if (kv_find(s.groups[g].table, key) >= 0) {
            return (int)g;
        }
    }
    return -1;
}

static size_t st_count(void *ctx)
{
    (void)ctx;
    size_t n = v1_count();
    for (size_t g = 0; g < s.group_count; g++) {
        n += s.groups[g].table->count;
    }
    return n;
}

static const char *st_name(void *ctx, size_t index)
{
    (void)ctx;
    const size_t v1 = v1_count();
    if (index < v1) {
        return v1_name(index);
    }
    index -= v1;
    for (size_t g = 0; g < s.group_count; g++) {
        if (index < s.groups[g].table->count) {
            return s.groups[g].table->keys[index].name;
        }
        index -= s.groups[g].table->count;
    }
    return NULL;
}

static bool st_secret(void *ctx, const char *key)
{
    (void)ctx;
    if (is_v1_key(key)) {
        return strcmp(key, "wifi_pass") == 0 || strcmp(key, "hue_key") == 0;
    }
    const int g = group_of(key);
    return g >= 0 && s.groups[g].table->keys[kv_find(s.groups[g].table, key)].type == KV_SECRET;
}

static sync_set_t st_set(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    if (is_v1_key(key)) {
        esp_err_t saved;
        switch (app_settings_set(key, value, &saved)) {
        case SETTINGS_OK:
            s.changed_groups |= 1u;
            if (strncmp(key, "wifi_", 5) == 0) {
                s.wifi_changed = true;
            }
            return SYNC_SET_OK;
        case SETTINGS_ERR_UNKNOWN_KEY: return SYNC_SET_UNKNOWN;
        case SETTINGS_ERR_INVALID: return SYNC_SET_INVALID;
        case SETTINGS_ERR_RANGE: return SYNC_SET_RANGE;
        case SETTINGS_ERR_CONFLICT: return SYNC_SET_CONFLICT;
        }
        return SYNC_SET_INVALID;
    }
    const int g = group_of(key);
    if (g < 0) {
        return SYNC_SET_UNKNOWN;
    }
    switch (cfg_store_set(s.groups[g].table, s.groups[g].data, key, value, NULL)) {
    case KV_OK:
        s.changed_groups |= 1u << (1 + g);
        if (g == 0 && (strncmp(key, "mqtt_", 5) == 0 || strcmp(key, "device_id") == 0)) {
            s.mqtt_changed = true;
        }
        return SYNC_SET_OK;
    case KV_ERR_UNKNOWN_KEY: return SYNC_SET_UNKNOWN;
    case KV_ERR_INVALID: return SYNC_SET_INVALID;
    case KV_ERR_RANGE: return SYNC_SET_RANGE;
    }
    return SYNC_SET_INVALID;
}

static bool st_format(void *ctx, const char *key, bool reveal, char *out, size_t out_size)
{
    (void)ctx;
    if (is_v1_key(key)) {
        return app_settings_format(key, reveal, out, out_size);
    }
    const int g = group_of(key);
    if (g < 0) {
        return false;
    }
    cfg_store_lock();
    const bool ok = kv_format(s.groups[g].table, s.groups[g].data, key, reveal, out, out_size);
    cfg_store_unlock();
    return ok;
}

static const sync_store_t s_store = {
    .ctx = NULL,
    .count = st_count,
    .name = st_name,
    .is_secret = st_secret,
    .set = st_set,
    .format = st_format,
};

/* ================================================================ status === */

static const char *reset_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT: return "external";
    default: return "other";
    }
}

/* "Nov 21 2026" + "16:42:11" -> "2026-11-21T16:42:11" (build machine's time) */
static void make_built(void)
{
    static const char *const MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const esp_app_desc_t *d = esp_app_get_description();
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strncmp(d->date, MON + 3 * i, 3) == 0) {
            month = i + 1;
        }
    }
    const int day = atoi(d->date + 4);
    const int year = atoi(d->date + 7);
    snprintf(s.built, sizeof(s.built), "%04d-%02d-%02dT%.8s", year, month, day, d->time);
}

static void boot_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint32_t n = 0;
    nvs_get_u32(h, "boot", &n);
    s.boot = n + 1;
    nvs_set_u32(h, "boot", s.boot);
    nvs_commit(h);
    nvs_close(h);
}

static void fill_common(status_common_t *c, char *ip)
{
    memset(c, 0, sizeof(*c));
    c->fw = esp_app_get_description()->version;
    c->built = s.built;
    c->boot = s.boot;
    c->reset = reset_name();
    c->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    wifi_ap_record_t ap;
    c->wifi_rssi = (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    app_status_t st;
    app_status_get(&st);
    strlcpy(ip, st.wifi_ip, 16);
    c->ip = ip;
    c->heap_min = (uint32_t)esp_get_minimum_free_heap_size();
    size_t n = 0;
    if (s.app->v1_settings) {
        c->groups[n++] = "settings";
    }
    for (size_t g = 0; g < s.group_count && n < STATUS_MAX_GROUPS; g++) {
        c->groups[n++] = s.groups[g].table->name;
    }
    c->group_count = n;
    c->settings_version = (int32_t)settings_sync_version(s.sync);
}

static void publish_status(uint32_t now)
{
    static char doc[STATUS_JSON_MAX + 1];
    status_common_t c;
    char ip[16];
    fill_common(&c, ip);
    const size_t n = s.app->build_status != NULL ? s.app->build_status(&c, doc, sizeof(doc)) : 0;
    if (n == 0) {
        ESP_LOGW(TAG, "status not built");
        return;
    }
    if (mqtt_link_publish(MQTT_TOPIC_STATUS, NULL, doc)) {
        s.last_status_ms = now;
        s.status_dirty = false;
    }
}

/* ============================================================== settings === */

static void save_version(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "sver", settings_sync_version(s.sync));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void publish_reported(void)
{
    static char doc[REPORT_MAX];
    const int n = settings_sync_report(s.sync, doc, sizeof(doc));
    if (n < 0) {
        ESP_LOGE(TAG, "reported settings do not fit %u bytes", (unsigned)sizeof(doc));
        return;
    }
    if (mqtt_link_publish(MQTT_TOPIC_REPORTED, NULL, doc)) {
        s.report_due = false;
    }
}

/* Runs in this task: apply a waiting desired document. */
static void apply_desired(void)
{
    char *doc = NULL;
    size_t len = 0;
    xSemaphoreTake(s.mx, portMAX_DELAY);
    doc = s.desired;
    len = s.desired_len;
    s.desired = NULL;
    xSemaphoreGive(s.mx);
    if (doc == NULL) {
        return;
    }
    const sync_result_t r = settings_sync_apply(s.sync, doc, len);
    free(doc);
    switch (r.status) {
    case SYNC_APPLIED:
        ESP_LOGI(TAG, "desired v%lu: %u changed, %u rejected", (unsigned long)r.version, r.changed, r.rejected);
        save_version();
        s.report_due = true;
        s.status_dirty = true;
        break;
    case SYNC_STALE:
        break;
    case SYNC_ERR_FORMAT:
        ESP_LOGW(TAG, "desired document refused (format)");
        break;
    }
}

/* What a changed value needs (from st_set, in this task or the console's). */
static void follow_changes(void)
{
    uint32_t changed;
    bool wifi, mqtt;
    xSemaphoreTake(s.mx, portMAX_DELAY);
    changed = s.changed_groups;
    wifi = s.wifi_changed;
    mqtt = s.mqtt_changed;
    s.changed_groups = 0;
    s.wifi_changed = s.mqtt_changed = false;
    xSemaphoreGive(s.mx);
    if (changed == 0) {
        return;
    }
    if (s.app->on_settings != NULL) {
        if (changed & 1u) {
            s.app->on_settings("settings");
        }
        for (size_t g = 0; g < s.group_count; g++) {
            if (changed & (1u << (1 + g))) {
                s.app->on_settings(s.groups[g].table->name);
            }
        }
    }
    if (wifi) {
        wifi_reconnect();
    }
    if (mqtt) {
        mqtt_link_restart();
    }
}

/* =============================================================== records === */

static void outbox_save(void)
{
    uint8_t *blob = malloc(record_outbox_saved_size(s.outbox));
    if (blob == NULL) {
        return;
    }
    const size_t n = record_outbox_save(s.outbox, blob, record_outbox_saved_size(s.outbox));
    nvs_handle_t h;
    if (n > 0 && nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "outbox", blob, n);
        nvs_commit(h);
        nvs_close(h);
    }
    free(blob);
}

static void outbox_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = 0;
    if (nvs_get_blob(h, "outbox", NULL, &len) == ESP_OK && len > 0) {
        uint8_t *blob = malloc(len);
        if (blob != NULL && nvs_get_blob(h, "outbox", blob, &len) == ESP_OK) {
            if (record_outbox_load(s.outbox, blob, len)) {
                ESP_LOGI(TAG, "%u records wait for the server", (unsigned)record_outbox_count(s.outbox));
            }
        }
        free(blob);
    }
    nvs_close(h);
}

static void send_records(uint32_t now)
{
    char rec[512];
    uint32_t id;
    for (;;) {
        xSemaphoreTake(s.mx, portMAX_DELAY);
        const size_t n = record_outbox_take_due(s.outbox, now, &id, rec, sizeof(rec) - 1);
        xSemaphoreGive(s.mx);
        if (n == 0) {
            break;
        }
        rec[n] = '\0';
        mqtt_link_publish(MQTT_TOPIC_EVENT, NULL, rec);
    }
    bool save;
    xSemaphoreTake(s.mx, portMAX_DELAY);
    save = s.outbox_dirty;
    s.outbox_dirty = false;
    xSemaphoreGive(s.mx);
    if (save) {
        xSemaphoreTake(s.mx, portMAX_DELAY);
        outbox_save();
        xSemaphoreGive(s.mx);
    }
}

static void handle_ack(const char *payload, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id) && id->valuedouble >= 1.0 && id->valuedouble <= 4294967295.0) {
        xSemaphoreTake(s.mx, portMAX_DELAY);
        if (record_outbox_ack(s.outbox, (uint32_t)id->valuedouble)) {
            s.outbox_dirty = true;
        }
        xSemaphoreGive(s.mx);
    }
    cJSON_Delete(root);
}

/* ================================================================= hooks === */

static void on_connect(void)
{
    s.connect_seen = true; /* the task does the rest (not in the MQTT task) */
    if (s.app->on_connect != NULL) {
        s.app->on_connect();
    }
}

static void on_cmd(const char *name, const char *payload, size_t len)
{
    if (strcmp(name, "ack") == 0) {
        handle_ack(payload, len);
        return;
    }
    if (s.app->on_cmd == NULL || !s.app->on_cmd(name, payload, len)) {
        server_link_cmd_result(name, false, "unknown");
    }
}

static void on_desired(const char *payload, size_t len)
{
    if (len == 0 || len > DESIRED_MAX) {
        return; /* an empty retained message clears the topic */
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, payload, len);
    copy[len] = '\0';
    xSemaphoreTake(s.mx, portMAX_DELAY);
    free(s.desired); /* a newer document replaces one not yet applied */
    s.desired = copy;
    s.desired_len = len;
    xSemaphoreGive(s.mx);
}

/* ================================================================== task === */

static void server_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SRV_PERIOD_MS));
        const uint32_t now = app_now_ms();
        const bool connected = mqtt_link_connected();
        if (connected != s.was_connected || s.connect_seen) {
            xSemaphoreTake(s.mx, portMAX_DELAY);
            record_outbox_set_connected(s.outbox, connected, now);
            xSemaphoreGive(s.mx);
            if (connected) {
                s.report_due = true;
                s.status_dirty = true;
                s.last_status_ms = now - STATUS_EVERY_MS; /* at once */
            }
            s.was_connected = connected;
            s.connect_seen = false;
        }
        apply_desired();
        follow_changes();
        if (!connected) {
            continue;
        }
        if (s.report_due) {
            publish_reported();
        }
        const uint32_t since = now - s.last_status_ms;
        if ((s.status_dirty && since >= STATUS_MIN_GAP_MS) || since >= STATUS_EVERY_MS) {
            publish_status(now);
        }
        send_records(now);
    }
}

static void sntp_start(void)
{
    if (esp_sntp_enabled()) {
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init(); /* UTC for events, records and the bedside's clock */
}

void server_link_start(const server_app_t *app)
{
    s.app = app;
    s.mx = xSemaphoreCreateMutex();
    s.groups[0].table = &cfg_net_table;
    s.groups[0].data = enroll_task_net_data();
    s.group_count = 1;
    for (size_t i = 0; i < app->group_count && s.group_count < MAX_STORE_GROUPS; i++) {
        s.groups[s.group_count++] = app->groups[i];
    }
    s.sync = settings_sync_create(&s_store);
    record_outbox_config_t ocfg;
    record_outbox_config_default(&ocfg);
    s.outbox = record_outbox_create(&ocfg);
    if (s.mx == NULL || s.sync == NULL || s.outbox == NULL) {
        ESP_LOGE(TAG, "init failed");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0;
        if (nvs_get_u32(h, "sver", &v) == ESP_OK) {
            settings_sync_set_version(s.sync, v);
        }
        nvs_close(h);
    }
    outbox_load();
    boot_count();
    make_built();
    sntp_start();

    s.hooks.node = app->node;
    s.hooks.group_count = 0;
    for (size_t g = 0; g < s.group_count && g < MQTT_LINK_MAX_GROUPS; g++) {
        s.hooks.groups[s.hooks.group_count++] = s.groups[g].table;
    }
    s.hooks.on_connect = on_connect;
    s.hooks.on_cmd = on_cmd;
    s.hooks.on_desired = on_desired;
    mqtt_link_start(&s.hooks);
    xTaskCreate(server_task, "server", SRV_TASK_STACK, NULL, SRV_TASK_PRIO, NULL);
}

/* ================================================================ public === */

bool server_link_connected(void)
{
    return mqtt_link_connected();
}

void server_link_status_changed(void)
{
    s.status_dirty = true;
}

bool server_link_event(const char *json)
{
    return json != NULL && mqtt_link_publish(MQTT_TOPIC_EVENT, NULL, json);
}

void server_link_cmd_result(const char *name, bool ok, const char *why)
{
    char json[160];
    if (status_json_event_cmd(name, ok, why, server_link_utc(), json, sizeof(json)) > 0) {
        server_link_event(json);
    }
}

bool server_link_record(uint32_t id, const char *json)
{
    if (s.outbox == NULL || json == NULL) {
        return false;
    }
    uint32_t dropped = 0;
    xSemaphoreTake(s.mx, portMAX_DELAY);
    const bool ok = record_outbox_push(s.outbox, id, json, strlen(json), &dropped);
    if (ok) {
        s.outbox_dirty = true;
    }
    xSemaphoreGive(s.mx);
    if (dropped != 0) {
        ESP_LOGW(TAG, "outbox full: record %lu dropped", (unsigned long)dropped);
    }
    return ok;
}

void server_link_settings_changed_locally(void)
{
    s.report_due = true;
    s.status_dirty = true;
}

void server_link_hue_scenes(const char *json)
{
    if (!mqtt_link_publish(MQTT_TOPIC_HUE_SCENES, NULL, json)) {
        ESP_LOGW(TAG, "scene list not published (not connected)");
    }
}

int64_t server_link_utc(void)
{
    const time_t t = time(NULL);
    return (t >= CLOCK_VALID_AFTER) ? (int64_t)t : -1;
}
