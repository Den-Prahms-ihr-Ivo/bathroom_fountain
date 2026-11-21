/*
 * mqtt_link – see mqtt_link.h.
 */
#include "mqtt_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

#include "enroll_task.h"
#include "settings_schema.h"

#define MQTT_BUFFER     4096
#define MQTT_KEEPALIVE  30
#define CMD_NAME_MAX    33

static const char *TAG = "mqtt";

static struct {
    const mqtt_link_hooks_t *hooks;
    esp_mqtt_client_handle_t client;
    SemaphoreHandle_t mutex;
    SemaphoreHandle_t published;
    volatile int wait_msg_id;
    volatile bool connected;
    char device_id[25];
    char will[64];
    char will_topic[MQTT_TOPIC_MAX];
    char uri[96];
    char user[33];
    char pass[65];
} s;

/* ------------------------------------------------------------- helpers --- */

static void publish_schemas(void)
{
    static char json[MQTT_BUFFER];
    for (size_t i = 0; i < s.hooks->group_count; i++) {
        const kv_table_t *t = s.hooks->groups[i];
        const size_t n = settings_schema_json(t, json, sizeof(json));
        if (n == 0) {
            ESP_LOGW(TAG, "schema %s does not fit", t->name);
            continue;
        }
        char topic[MQTT_TOPIC_MAX + 24];
        snprintf(topic, sizeof(topic), "shower/%s/settings/schema/%s", s.device_id, t->name);
        esp_mqtt_client_publish(s.client, topic, json, (int)n, 1, 1);
    }
}

static void subscribe_all(void)
{
    char topic[MQTT_TOPIC_MAX];
    if (mqtt_topic_build(topic, sizeof(topic), s.device_id, MQTT_TOPIC_CMD, "#")) {
        esp_mqtt_client_subscribe(s.client, topic, 1);
    }
    if (mqtt_topic_build(topic, sizeof(topic), s.device_id, MQTT_TOPIC_DESIRED, NULL)) {
        esp_mqtt_client_subscribe(s.client, topic, 1);
    }
}

static void on_data(const esp_mqtt_event_handle_t ev)
{
    if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len) {
        ESP_LOGW(TAG, "message over %d bytes dropped", MQTT_BUFFER);
        return;
    }
    mqtt_topic_t kind;
    const char *suffix;
    size_t suffix_len;
    if (!mqtt_topic_parse(ev->topic, (size_t)ev->topic_len, s.device_id, &kind, &suffix, &suffix_len)) {
        return;
    }
    if (kind == MQTT_TOPIC_DESIRED) {
        if (s.hooks->on_desired != NULL) {
            s.hooks->on_desired(ev->data, (size_t)ev->data_len);
        }
        return;
    }
    if (kind != MQTT_TOPIC_CMD || suffix_len >= CMD_NAME_MAX) {
        return;
    }
    char name[CMD_NAME_MAX];
    memcpy(name, suffix, suffix_len);
    name[suffix_len] = '\0';
    ESP_LOGI(TAG, "cmd/%s", name);
    if (strcmp(name, "forget") == 0) {
        enroll_task_forget(); /* -> ENROLL_ACT_CLEAR -> mqtt_link_stop() */
    } else if (strcmp(name, "restart") == 0) {
        esp_restart();
    } else if (s.hooks->on_cmd != NULL) {
        s.hooks->on_cmd(name, ev->data, (size_t)ev->data_len);
    }
}

static void handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s.connected = true;
        ESP_LOGI(TAG, "connected as %s", s.device_id);
        enroll_task_auth_result(true);
        subscribe_all();
        publish_schemas();
        if (s.hooks->on_connect != NULL) {
            s.hooks->on_connect();
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s.connected = false;
        break;
    case MQTT_EVENT_PUBLISHED:
        if (ev->msg_id == s.wait_msg_id) {
            xSemaphoreGive(s.published);
        }
        break;
    case MQTT_EVENT_DATA:
        on_data(ev);
        break;
    case MQTT_EVENT_ERROR:
        if (ev->error_handle != NULL && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            const esp_mqtt_connect_return_code_t rc = ev->error_handle->connect_return_code;
            ESP_LOGW(TAG, "connection refused (%d)", (int)rc);
            if (rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED || rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME) {
                enroll_task_auth_result(false); /* three in a row: credentials erased */
            }
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------ lifecycle --- */

static void stop_locked(void)
{
    if (s.client != NULL) {
        esp_mqtt_client_stop(s.client);
        esp_mqtt_client_destroy(s.client);
        s.client = NULL;
    }
    s.connected = false;
}

static void start_locked(void)
{
    cfg_net_t net;
    enroll_task_net(&net);
    if (net.device_id[0] == '\0' || net.mqtt_host[0] == '\0' || net.mqtt_user[0] == '\0' || net.mqtt_pass[0] == '\0') {
        ESP_LOGI(TAG, "no broker account yet (pairing)");
        return;
    }
    strlcpy(s.device_id, net.device_id, sizeof(s.device_id));
    strlcpy(s.user, net.mqtt_user, sizeof(s.user));
    strlcpy(s.pass, net.mqtt_pass, sizeof(s.pass));
    snprintf(s.uri, sizeof(s.uri), "mqtts://%s:%u", net.mqtt_host, (unsigned)net.mqtt_port);
    snprintf(s.will, sizeof(s.will), "{\"online\":false,\"node\":\"%s\"}", s.hooks->node);
    mqtt_topic_build(s.will_topic, sizeof(s.will_topic), s.device_id, MQTT_TOPIC_STATUS, NULL);
    memset(&net, 0, sizeof(net));

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s.uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = s.device_id,
        .credentials.username = s.user,
        .credentials.authentication.password = s.pass,
        .session.keepalive = MQTT_KEEPALIVE,
        .session.last_will.topic = s.will_topic,
        .session.last_will.msg = s.will,
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .buffer.size = MQTT_BUFFER,
    };
    s.client = esp_mqtt_client_init(&cfg);
    if (s.client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return;
    }
    esp_mqtt_client_register_event(s.client, MQTT_EVENT_ANY, handler, NULL);
    esp_mqtt_client_start(s.client);
}

void mqtt_link_start(const mqtt_link_hooks_t *hooks)
{
    s.hooks = hooks;
    s.mutex = xSemaphoreCreateMutex();
    s.published = xSemaphoreCreateBinary();
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    start_locked();
    xSemaphoreGive(s.mutex);
}

void mqtt_link_restart(void)
{
    if (s.mutex == NULL) {
        return;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    stop_locked();
    start_locked();
    xSemaphoreGive(s.mutex);
}

void mqtt_link_stop(void)
{
    if (s.mutex == NULL) {
        return;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    stop_locked();
    xSemaphoreGive(s.mutex);
}

bool mqtt_link_connected(void)
{
    return s.connected;
}

/* -------------------------------------------------------------- publish --- */

static int publish_raw(mqtt_topic_t kind, const char *suffix, const char *payload)
{
    if (!s.connected || s.client == NULL || payload == NULL) {
        return -1;
    }
    char topic[MQTT_TOPIC_MAX];
    if (!mqtt_topic_build(topic, sizeof(topic), s.device_id, kind, suffix)) {
        return -1;
    }
    const bool retain = (kind == MQTT_TOPIC_STATUS || kind == MQTT_TOPIC_REPORTED || kind == MQTT_TOPIC_HUE_SCENES ||
                         kind == MQTT_TOPIC_NODE);
    const int qos = (kind == MQTT_TOPIC_LOG) ? 0 : 1;
    /* enqueue: never blocks the caller's control loop on the network */
    return esp_mqtt_client_enqueue(s.client, topic, payload, 0, qos, retain ? 1 : 0, true);
}

bool mqtt_link_publish(mqtt_topic_t kind, const char *suffix, const char *payload)
{
    return publish_raw(kind, suffix, payload) >= 0;
}

bool mqtt_link_publish_and_wait(mqtt_topic_t kind, const char *payload, uint32_t timeout_ms)
{
    xSemaphoreTake(s.published, 0); /* drop a stale give */
    const int id = publish_raw(kind, NULL, payload);
    if (id <= 0) {
        return false;
    }
    s.wait_msg_id = id;
    const bool ok = xSemaphoreTake(s.published, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    s.wait_msg_id = -1;
    return ok;
}
