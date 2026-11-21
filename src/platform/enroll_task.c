/*
 * enroll_task – see enroll_task.h.
 */
#include "enroll_task.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "cfg_store.h"
#include "enroll_msg.h"
#include "events.h"
#include "mqtt_link.h"
#include "wifi.h"

#if __has_include("secrets.h")
#include "secrets.h" /* may define SERVER_HOST_DEFAULT; values are never logged */
#endif

#define ENROLL_PERIOD_MS   100u
#define ENROLL_TASK_STACK  6144u
#define ENROLL_TASK_PRIO   3u
#define HTTP_TIMEOUT_MS    8000
#define HTTP_BODY_MAX      512
#define REPLY_MAX          768

static const char *TAG = "enroll";

static struct {
    SemaphoreHandle_t mutex;
    enroll_t *e;
    cfg_net_t net;
    const char *node;
    char chip[ENROLL_CHIP_LEN + 1];
    char secret[ENROLL_SECRET_LEN + 1];
} s;

/* ----------------------------------------------------------- identity --- */

static void load_chip_id(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    enroll_msg_hex(mac, sizeof(mac), s.chip, sizeof(s.chip));
}

static void load_secret(void)
{
    nvs_handle_t h;
    if (nvs_open("enroll", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "NVS: no secret, pairing impossible");
        return;
    }
    size_t len = sizeof(s.secret);
    if (nvs_get_str(h, "secret", s.secret, &len) != ESP_OK || strlen(s.secret) != ENROLL_SECRET_LEN) {
        uint8_t raw[ENROLL_SECRET_LEN / 2];
        esp_fill_random(raw, sizeof(raw)); /* Wi-Fi runs: true random */
        enroll_msg_hex(raw, sizeof(raw), s.secret, sizeof(s.secret));
        if (nvs_set_str(h, "secret", s.secret) == ESP_OK) {
            nvs_commit(h);
        }
        ESP_LOGI(TAG, "new device secret made");
    }
    nvs_close(h);
}

static bool claimed(const cfg_net_t *n)
{
    return n->device_id[0] != '\0' && n->mqtt_host[0] != '\0' && n->mqtt_user[0] != '\0' && n->mqtt_pass[0] != '\0';
}

/* --------------------------------------------------------------- HTTPS --- */

/* POST body to /api/enroll; reply parsed into *out. false on any network or
 * parse error (nothing goes into enroll_reply then: errors never count). */
static bool post(const char *body, enroll_reply_t *out)
{
    cfg_net_t net;
    enroll_task_net(&net);
    if (net.server_host[0] == '\0') {
        ESP_LOGW(TAG, "server_host not set: set it on the console");
        return false;
    }
    if (!wifi_wait_connected(5000)) {
        return false;
    }
    char url[96];
    snprintf(url, sizeof(url), "https://%s/api/enroll", net.server_host);
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return false;
    }
    bool ok = false;
    static char reply[REPLY_MAX];
    esp_http_client_set_header(c, "Content-Type", "application/json");
    const int len = (int)strlen(body);
    if (esp_http_client_open(c, len) == ESP_OK && esp_http_client_write(c, body, len) == len) {
        esp_http_client_fetch_headers(c);
        const int status = esp_http_client_get_status_code(c);
        const int got = esp_http_client_read_response(c, reply, sizeof(reply) - 1);
        if (status == 200 && got > 0) {
            reply[got] = '\0';
            ok = enroll_msg_parse(reply, (size_t)got, out);
            if (!ok) {
                ESP_LOGW(TAG, "reply not understood");
            }
        } else {
            ESP_LOGW(TAG, "HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "%s unreachable", net.server_host);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

/* -------------------------------------------------------------- actions --- */

static void save_creds(const enroll_creds_t *c)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)c->mqtt_port);
    cfg_store_lock();
    cfg_store_set(&cfg_net_table, &s.net, "device_id", c->device_id, NULL);
    cfg_store_set(&cfg_net_table, &s.net, "mqtt_host", c->mqtt_host, NULL);
    cfg_store_set(&cfg_net_table, &s.net, "mqtt_port", port, NULL);
    cfg_store_set(&cfg_net_table, &s.net, "mqtt_user", c->mqtt_user, NULL);
    cfg_store_set(&cfg_net_table, &s.net, "mqtt_pass", c->mqtt_pass, NULL);
    cfg_store_unlock();
    ESP_LOGI(TAG, "claimed as %s", c->device_id);
}

static void clear_creds(void)
{
    static const char *const keys[] = {"device_id", "mqtt_host", "mqtt_port", "mqtt_user", "mqtt_pass"};
    cfg_store_lock();
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cfg_store_reset_key(&cfg_net_table, &s.net, keys[i]);
    }
    cfg_store_unlock();
    ESP_LOGW(TAG, "broker account erased, unclaimed (chip %s)", s.chip);
}

static void run_action(enroll_act_t act)
{
    static char body[HTTP_BODY_MAX];
    enroll_reply_t reply;
    switch (act) {
    case ENROLL_ACT_HELLO:
    case ENROLL_ACT_CONFIRM: {
        size_t n;
        if (act == ENROLL_ACT_HELLO) {
            n = enroll_msg_hello(s.chip, s.node, esp_app_get_description()->version, s.secret, body, sizeof(body));
        } else {
            n = enroll_msg_confirm(s.chip, s.secret, body, sizeof(body));
        }
        /* The request runs without the mutex: a button hold must not wait 8 s. */
        if (n > 0 && post(body, &reply)) {
            xSemaphoreTake(s.mutex, portMAX_DELAY);
            enroll_reply(s.e, &reply, app_now_ms());
            xSemaphoreGive(s.mutex);
        }
        break;
    }
    case ENROLL_ACT_SAVE: {
        enroll_creds_t c;
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        c = *enroll_creds(s.e);
        xSemaphoreGive(s.mutex);
        save_creds(&c);
        memset(&c, 0, sizeof(c));
        mqtt_link_restart();
        break;
    }
    case ENROLL_ACT_CLEAR:
        mqtt_link_stop();
        clear_creds();
        break;
    }
}

/* ----------------------------------------------------------------- task --- */

static void enroll_task(void *arg)
{
    (void)arg;
    enroll_state_t last = (enroll_state_t)-1;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ENROLL_PERIOD_MS));
        enroll_act_t act;
        bool have;
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        enroll_tick(s.e, app_now_ms());
        const enroll_state_t st = enroll_outputs(s.e)->state;
        have = enroll_take(s.e, &act);
        xSemaphoreGive(s.mutex);
        if (st != last) {
            ESP_LOGI(TAG, "state %d (chip %s)", (int)st, s.chip);
            last = st;
        }
        while (have) {
            run_action(act);
            xSemaphoreTake(s.mutex, portMAX_DELAY);
            have = enroll_take(s.e, &act);
            xSemaphoreGive(s.mutex);
        }
    }
}

void enroll_task_start(const char *node)
{
    s.node = node;
    s.mutex = xSemaphoreCreateMutex();
    load_chip_id();
    load_secret();
    cfg_store_load(&cfg_net_table, &s.net);
    if (s.net.server_host[0] == '\0' && SERVER_HOST_DEFAULT[0] != '\0') {
        cfg_store_set(&cfg_net_table, &s.net, "server_host", SERVER_HOST_DEFAULT, NULL);
    }
    ESP_LOGI(TAG, "chip id %s, %s", s.chip, claimed(&s.net) ? "claimed" : "unclaimed");

    enroll_config_t cfg;
    enroll_config_default(&cfg);
    s.e = enroll_create(&cfg, claimed(&s.net), app_now_ms());
    if (s.e == NULL || s.mutex == NULL) {
        ESP_LOGE(TAG, "init failed");
        return;
    }
    xTaskCreate(enroll_task, "enroll", ENROLL_TASK_STACK, NULL, ENROLL_TASK_PRIO, NULL);
}

/* --------------------------------------------------------------- access --- */

bool enroll_task_button_held(void)
{
    if (s.e == NULL) {
        return false;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    const bool used = enroll_button_held(s.e, app_now_ms());
    xSemaphoreGive(s.mutex);
    if (used) {
        ESP_LOGI(TAG, "button held: confirming");
    }
    return used;
}

void enroll_task_auth_result(bool ok)
{
    if (s.e == NULL) {
        return;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    enroll_auth_result(s.e, ok, app_now_ms());
    xSemaphoreGive(s.mutex);
}

void enroll_task_forget(void)
{
    if (s.e == NULL) {
        return;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    enroll_forget(s.e, app_now_ms());
    xSemaphoreGive(s.mutex);
}

bool enroll_task_blink(void)
{
    if (s.e == NULL) {
        return false;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    const bool b = enroll_outputs(s.e)->blink;
    xSemaphoreGive(s.mutex);
    return b;
}

enroll_state_t enroll_task_state(void)
{
    if (s.e == NULL) {
        return ENROLL_UNCLAIMED;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    const enroll_state_t st = enroll_outputs(s.e)->state;
    xSemaphoreGive(s.mutex);
    return st;
}

void enroll_task_net(cfg_net_t *out)
{
    cfg_store_lock();
    *out = s.net;
    cfg_store_unlock();
}

kv_result_t enroll_task_net_set(const char *key, const char *value)
{
    return cfg_store_set(&cfg_net_table, &s.net, key, value, NULL);
}

const char *enroll_task_chip_id(void)
{
    return s.chip;
}
