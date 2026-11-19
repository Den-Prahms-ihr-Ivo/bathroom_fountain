/*
 * hue_task – see hue_task.h.
 */
#include "hue_task.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "app_settings.h"
#include "app_status.h"
#include "events.h"
#include "hue_api.h"
#include "hue_sender.h"
#include "wifi.h"

#define HUE_TASK_STACK   10240u /* TLS handshake runs on this stack */
#define HUE_TASK_PRIO    4u
#define HUE_TASK_CORE    0
#define HUE_LOOP_MS      50u
#define HUE_TIMEOUT_MS   5000
#define HUE_RESP_MAX     1024u
#define PAIRING_TOTAL_MS 30000u
#define PAIRING_EVERY_MS 1000u
#define PAIRING_INSTANCE "esp32s3"

static const char *TAG = "hue";

typedef enum { REQ_COMMAND, REQ_PAIR, REQ_LIST_SCENES } req_type_t;

typedef struct {
    req_type_t type;
    hue_command_t cmd;
} req_t;

static QueueHandle_t s_queue;
static hue_sender_t *s_sender;

/* ------------------------------------------------------------- status --- */

static const char *result_name(hue_result_t r)
{
    switch (r) {
    case HUE_OK: return "ok";
    case HUE_ERR_TRANSPORT: return "no connection";
    case HUE_ERR_AUTH: return "key rejected";
    case HUE_ERR_NOT_FOUND: return "scene not found";
    case HUE_ERR_RATE_LIMITED: return "rate limited";
    case HUE_ERR_BRIDGE_BUSY: return "bridge busy";
    case HUE_ERR_REJECTED: return "rejected";
    case HUE_ERR_BAD_RESPONSE: return "bad response";
    case HUE_ERR_LINK_BUTTON: return "link button";
    }
    return "?";
}

static const char *state_name(hue_sender_state_t s)
{
    switch (s) {
    case HUE_SENDER_IDLE: return "idle";
    case HUE_SENDER_PENDING: return "pending";
    case HUE_SENDER_IN_FLIGHT: return "sending";
    case HUE_SENDER_WAITING_RETRY: return "waiting to retry";
    case HUE_SENDER_NEEDS_PAIRING: return "needs pairing";
    }
    return "?";
}

static void publish(const char *last)
{
    app_status_t *s = app_status_lock();
    app_status_copy(s->hue_state, sizeof(s->hue_state), state_name(hue_sender_state(s_sender)));
    if (last != NULL) {
        app_status_copy(s->hue_last, sizeof(s->hue_last), last);
    }
    app_status_unlock();
}

/* --------------------------------------------------------------- HTTPS --- */

typedef void (*stream_fn_t)(const char *data, int len, void *ctx);

typedef struct {
    char *buf;          /* collected body (NULL when streaming)          */
    size_t size;
    size_t len;
    stream_fn_t stream; /* or: every chunk goes here                     */
    void *stream_ctx;
} body_t;

static esp_http_client_handle_t s_client;
static char s_client_host[64];

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b != NULL && e->data_len > 0) {
        if (b->stream != NULL) {
            b->stream(e->data, e->data_len, b->stream_ctx);
        } else if (b->buf != NULL) {
            const size_t room = b->size - b->len;
            const size_t n = ((size_t)e->data_len < room) ? (size_t)e->data_len : room;
            memcpy(b->buf + b->len, e->data, n);
            b->len += n;
        }
    }
    return ESP_OK;
}

static void client_drop(void)
{
    if (s_client != NULL) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    s_client_host[0] = '\0';
}

/* One request. Returns the HTTP status, or <= 0 on a transport failure. */
static int https_request(const char *host, const char *key, const char *method, const char *path,
                         const char *body, body_t *resp)
{
    char url[160];
    snprintf(url, sizeof(url), "https://%s%s", host, path);

    if (s_client == NULL || strcmp(s_client_host, host) != 0) {
        client_drop();
        const esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = HUE_TIMEOUT_MS,
            .event_handler = on_http_event,
            .keep_alive_enable = true,
            .skip_cert_common_name_check = true,
            .buffer_size = 1024,
            .buffer_size_tx = 1024,
        };
        s_client = esp_http_client_init(&cfg);
        if (s_client == NULL) {
            return -1;
        }
        app_status_copy(s_client_host, sizeof(s_client_host), host);
    }

    esp_http_client_set_url(s_client, url);
    esp_http_client_set_user_data(s_client, resp);
    const bool is_put = strcmp(method, "PUT") == 0;
    const bool is_post = strcmp(method, "POST") == 0;
    esp_http_client_set_method(s_client, is_put ? HTTP_METHOD_PUT : is_post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (key != NULL && key[0] != '\0') {
        esp_http_client_set_header(s_client, "hue-application-key", key);
    } else {
        esp_http_client_delete_header(s_client, "hue-application-key");
    }
    if (body != NULL && body[0] != '\0') {
        esp_http_client_set_header(s_client, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_client, body, (int)strlen(body));
    } else {
        esp_http_client_delete_header(s_client, "Content-Type");
        esp_http_client_set_post_field(s_client, NULL, 0);
    }

    const esp_err_t err = esp_http_client_perform(s_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", method, path, esp_err_to_name(err));
        client_drop(); /* a fresh connection next time */
        return -1;
    }
    return esp_http_client_get_status_code(s_client);
}

/* ------------------------------------------------------------ commands --- */

static hue_result_t send_recall(const hue_command_t *cmd)
{
    settings_t st;
    app_settings_snapshot(&st, NULL);
    const bool ambient = (cmd->scene == HUE_SCENE_AMBIENT);
    const char *scene = ambient ? st.scene_ambient : st.scene_normal;

    if (st.hue_host[0] == '\0') {
        ESP_LOGW(TAG, "no bridge set: 'set hue_host <ip>'");
        return HUE_ERR_TRANSPORT; /* retried, so it goes through once the host is set */
    }
    if (st.hue_key[0] == '\0') {
        ESP_LOGW(TAG, "not paired: press the bridge's link button, then hold the device button 3 s");
        return HUE_ERR_AUTH;
    }
    hue_request_t req;
    if (!hue_build_scene_recall(scene, cmd->duration_ms, &req)) {
        ESP_LOGW(TAG, "%s scene not set or not a valid id: 'set %s <id>' ('hue scenes' lists them)",
                 ambient ? "Ambient" : "Normal", ambient ? "scene_ambient" : "scene_normal");
        return HUE_ERR_REJECTED;
    }

    char buf[HUE_RESP_MAX];
    body_t resp = {.buf = buf, .size = sizeof(buf)};
    const int status = https_request(st.hue_host, st.hue_key, req.method, req.path, req.body, &resp);
    const hue_result_t r = hue_parse_response(status, buf, resp.len);
    ESP_LOGI(TAG, "recall %s (%lu ms): HTTP %d, %s", ambient ? "AMBIENT" : "NORMAL",
             (unsigned long)cmd->duration_ms, status, result_name(r));
    if (r == HUE_ERR_AUTH) {
        ESP_LOGW(TAG, "the bridge rejected the key: pair again (link button, then hold the device button 3 s)");
    }
    return r;
}

static void do_pairing(void)
{
    if (!wifi_wait_connected(5000)) {
        ESP_LOGW(TAG, "pairing: no Wi-Fi");
        return;
    }
    hue_request_t req;
    if (!hue_build_pairing(PAIRING_INSTANCE, &req)) {
        return;
    }
    publish("pairing");
    const uint32_t start = app_now_ms();
    while ((uint32_t)(app_now_ms() - start) < PAIRING_TOTAL_MS) {
        settings_t st;
        app_settings_snapshot(&st, NULL);
        if (st.hue_host[0] == '\0') {
            ESP_LOGW(TAG, "pairing: set the bridge first ('set hue_host <ip>')");
            publish("pairing: no host");
            return;
        }
        char buf[HUE_RESP_MAX];
        body_t resp = {.buf = buf, .size = sizeof(buf)};
        const int status = https_request(st.hue_host, NULL, req.method, req.path, req.body, &resp);
        char key[sizeof(st.hue_key)];
        const hue_result_t r = hue_parse_pairing(status, buf, resp.len, key, sizeof(key));
        if (r == HUE_OK) {
            esp_err_t saved = ESP_OK;
            const settings_result_t sr = app_settings_set("hue_key", key, &saved);
            if (sr != SETTINGS_OK || saved != ESP_OK) {
                ESP_LOGE(TAG, "paired, but the key could not be stored (%d, %s)", (int)sr, esp_err_to_name(saved));
            } else {
                ESP_LOGI(TAG, "paired, key saved");
            }
            hue_sender_paired(s_sender, app_now_ms());
            publish("paired");
            return;
        }
        if (r == HUE_ERR_LINK_BUTTON) {
            ESP_LOGI(TAG, "pairing: press the link button on the bridge");
        } else if (!hue_result_is_retryable(r)) {
            ESP_LOGW(TAG, "pairing failed: HTTP %d, %s", status, result_name(r));
            publish("pairing failed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(PAIRING_EVERY_MS));
    }
    ESP_LOGW(TAG, "pairing timed out: link button not pressed within 30 s");
    publish("pairing timed out");
}

/* ------------------------------------------------------ scene listing --- */

/*
 * A streaming scanner over the CLIP v2 JSON: picks out "id", "name" and the
 * room/zone a scene belongs to ("rid" followed by "rtype":"room"/"zone")
 * without holding the whole (often 30+ KB) answer in memory.
 */
#define NAME_MAX_LEN 40
#define MAX_GROUPS   24

typedef struct {
    char id[37];
    char name[NAME_MAX_LEN];
} named_t;

typedef struct {
    char tail[16];          /* last characters seen, for key matching   */
    int capture;            /* 0 none, 1 id, 2 name, 3 rid              */
    char cap[NAME_MAX_LEN];
    size_t cap_len;
    bool cap_escape;
    named_t cur;            /* object being collected                    */
    char last_rid[37];
    char group_rid[37];
    bool have_cur;
    /* mode: collecting groups, or printing scenes */
    bool scenes;
    named_t groups[MAX_GROUPS];
    size_t n_groups;
    unsigned printed;
} scan_t;

static bool tail_ends_with(const scan_t *s, const char *pat)
{
    const size_t n = strlen(s->tail);
    const size_t m = strlen(pat);
    return n >= m && memcmp(s->tail + n - m, pat, m) == 0;
}

static const char *group_name(const scan_t *s, const char *rid)
{
    for (size_t i = 0; i < s->n_groups; i++) {
        if (strcmp(s->groups[i].id, rid) == 0) {
            return s->groups[i].name;
        }
    }
    return "";
}

static void scan_emit(scan_t *s)
{
    if (!s->have_cur || s->cur.id[0] == '\0') {
        return;
    }
    if (s->scenes) {
        printf("  %s  %-28s %s\n", s->cur.id, s->cur.name, group_name(s, s->group_rid));
        s->printed++;
    } else if (s->n_groups < MAX_GROUPS) {
        s->groups[s->n_groups++] = s->cur;
    }
    memset(&s->cur, 0, sizeof(s->cur));
    s->group_rid[0] = '\0';
    s->have_cur = false;
}

static void scan_char(scan_t *s, char c)
{
    if (s->capture != 0) {
        if (s->cap_escape) {
            s->cap_escape = false;
        } else if (c == '\\') {
            s->cap_escape = true;
            return;
        } else if (c == '"') {
            s->cap[s->cap_len] = '\0';
            if (s->capture == 1) {
                scan_emit(s); /* a new object starts with its id */
                app_status_copy(s->cur.id, sizeof(s->cur.id), s->cap);
                s->have_cur = true;
            } else if (s->capture == 2 && s->have_cur && s->cur.name[0] == '\0') {
                app_status_copy(s->cur.name, sizeof(s->cur.name), s->cap);
            } else if (s->capture == 3) {
                app_status_copy(s->last_rid, sizeof(s->last_rid), s->cap);
            }
            s->capture = 0;
            s->tail[0] = '\0';
            return;
        }
        if (s->cap_len + 1 < sizeof(s->cap)) {
            s->cap[s->cap_len++] = c;
        }
        return;
    }

    size_t n = strlen(s->tail);
    if (n + 1 >= sizeof(s->tail)) {
        memmove(s->tail, s->tail + 1, n);
        n--;
    }
    s->tail[n] = c;
    s->tail[n + 1] = '\0';

    int start = 0;
    if (tail_ends_with(s, "\"id\":\"")) {
        start = 1;
    } else if (tail_ends_with(s, "\"name\":\"")) {
        start = 2;
    } else if (tail_ends_with(s, "\"rid\":\"")) {
        start = 3;
    } else if (tail_ends_with(s, "\"rtype\":\"room\"") || tail_ends_with(s, "\"rtype\":\"zone\"")) {
        app_status_copy(s->group_rid, sizeof(s->group_rid), s->last_rid);
    }
    if (start != 0) {
        s->capture = start;
        s->cap_len = 0;
        s->cap_escape = false;
    }
}

static void scan_stream(const char *data, int len, void *ctx)
{
    for (int i = 0; i < len; i++) {
        scan_char(ctx, data[i]);
    }
}

static bool scan_get(scan_t *s, const settings_t *st, const char *path)
{
    s->tail[0] = '\0';
    s->capture = 0;
    s->have_cur = false;
    memset(&s->cur, 0, sizeof(s->cur));
    body_t resp = {.stream = scan_stream, .stream_ctx = s};
    const int status = https_request(st->hue_host, st->hue_key, "GET", path, NULL, &resp);
    scan_emit(s);
    if (status != 200) {
        printf("GET %s: HTTP %d (%s)\n", path, status, status <= 0 ? "no connection" : "error");
        return false;
    }
    return true;
}

static void list_scenes(void)
{
    settings_t st;
    app_settings_snapshot(&st, NULL);
    if (!wifi_wait_connected(5000)) {
        printf("no Wi-Fi\n");
        return;
    }
    if (st.hue_host[0] == '\0' || st.hue_key[0] == '\0') {
        printf("set hue_host and pair first\n");
        return;
    }
    scan_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return;
    }
    if (scan_get(s, &st, "/clip/v2/resource/room") && scan_get(s, &st, "/clip/v2/resource/zone")) {
        s->scenes = true;
        printf("scenes (id, name, room):\n");
        if (scan_get(s, &st, "/clip/v2/resource/scene")) {
            printf("%u scenes. Use: set scene_normal <id> / set scene_ambient <id>\n", s->printed);
        }
    }
    free(s);
}

/* ---------------------------------------------------------------- task --- */

static void hue_task(void *arg)
{
    (void)arg;
    publish(NULL);
    for (;;) {
        req_t req;
        if (xQueueReceive(s_queue, &req, pdMS_TO_TICKS(HUE_LOOP_MS)) == pdTRUE) {
            switch (req.type) {
            case REQ_COMMAND:
                hue_sender_submit(s_sender, &req.cmd, app_now_ms());
                break;
            case REQ_PAIR:
                do_pairing();
                break;
            case REQ_LIST_SCENES:
                list_scenes();
                break;
            }
            publish(NULL);
        }

        if (!wifi_is_connected()) {
            continue; /* the command waits (PENDING) until the network is back */
        }
        hue_command_t cmd;
        if (hue_sender_poll(s_sender, app_now_ms(), &cmd)) {
            publish(NULL);
            const hue_result_t r = send_recall(&cmd);
            hue_sender_report(s_sender, r, app_now_ms());
            if (r != HUE_OK && hue_sender_state(s_sender) == HUE_SENDER_WAITING_RETRY) {
                ESP_LOGI(TAG, "retry %lu scheduled", (unsigned long)hue_sender_attempts(s_sender));
            }
            publish(result_name(r));
        }
    }
}

void hue_task_start(void)
{
    s_queue = xQueueCreate(8, sizeof(req_t));
    s_sender = hue_sender_create();
    if (s_queue == NULL || s_sender == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    xTaskCreatePinnedToCore(hue_task, "hue", HUE_TASK_STACK, NULL, HUE_TASK_PRIO, NULL, HUE_TASK_CORE);
}

static void post(const req_t *req)
{
    if (s_queue == NULL || xQueueSend(s_queue, req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "request %d dropped", (int)req->type);
    }
}

void hue_task_submit(const hue_command_t *cmd)
{
    const req_t req = {.type = REQ_COMMAND, .cmd = *cmd};
    post(&req);
}

void hue_task_request_pairing(void)
{
    const req_t req = {.type = REQ_PAIR};
    post(&req);
}

void hue_task_request_scene_list(void)
{
    const req_t req = {.type = REQ_LIST_SCENES};
    post(&req);
}
