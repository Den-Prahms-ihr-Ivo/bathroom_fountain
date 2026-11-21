/*
 * hue_task – see hue_task.h.
 */
#include "hue_task.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

#define ID_MAX_ATTEMPTS  6       /* recall by id: 0.5 + 1 + 2 + 4 + 8 s of retries */
#define SCENES_JSON_MAX  8192u   /* scene list document (MQTT out buffer is 8 KB) */
#define CLOCK_VALID_AFTER 1767225600 /* 2026-01-01 */

typedef enum { REQ_COMMAND, REQ_PAIR, REQ_LIST_SCENES, REQ_RECALL_ID, REQ_PUBLISH_SCENES } req_type_t;

typedef struct {
    req_type_t type;
    hue_command_t cmd;        /* REQ_COMMAND   */
    char id[HUE_SCENE_ID_LEN + 1]; /* REQ_RECALL_ID */
    uint32_t duration_ms;     /* REQ_RECALL_ID */
} req_t;

static QueueHandle_t s_queue;
static hue_sender_t *s_sender;
static hue_scene_sink_t s_sink;

/*
 * Recall by id (bedside scenes, cold-shower scenes) runs beside hue_sender,
 * which only knows AMBIENT / NORMAL. Order between the two: every request
 * gets a sequence number; whichever was asked for last wins.
 *   - a REQ_COMMAND drops an id recall that is still waiting;
 *   - a hue_sender command polled after a newer id recall was asked for is
 *     reported as done without sending it (it would undo the newer scene).
 * hue_sender holds only the latest command, so one number for it is enough.
 */
static struct {
    bool pending;
    char id[HUE_SCENE_ID_LEN + 1];
    uint32_t duration_ms;
    uint32_t attempts;
    uint32_t due_ms;
    uint32_t seq;
} s_id;
static uint32_t s_seq;
static uint32_t s_cmd_seq;    /* sequence of the command hue_sender holds */
static uint32_t s_id_seq;     /* sequence of the latest id recall asked for */

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

/* One recall of a scene given by id. */
static hue_result_t send_recall_id(const char *id, uint32_t duration_ms)
{
    settings_t st;
    app_settings_snapshot(&st, NULL);
    if (st.hue_host[0] == '\0') {
        ESP_LOGW(TAG, "no bridge set: 'set hue_host <ip>'");
        return HUE_ERR_TRANSPORT;
    }
    if (st.hue_key[0] == '\0') {
        ESP_LOGW(TAG, "not paired (hue/pair, or 'hue pair' on the console)");
        return HUE_ERR_AUTH;
    }
    hue_request_t req;
    if (!hue_build_scene_recall(id, duration_ms, &req)) {
        ESP_LOGW(TAG, "scene id '%s' is not valid", id);
        return HUE_ERR_REJECTED;
    }
    char buf[HUE_RESP_MAX];
    body_t resp = {.buf = buf, .size = sizeof(buf)};
    const int status = https_request(st.hue_host, st.hue_key, req.method, req.path, req.body, &resp);
    const hue_result_t r = hue_parse_response(status, buf, resp.len);
    ESP_LOGI(TAG, "recall %s (%lu ms): HTTP %d, %s", id, (unsigned long)duration_ms, status, result_name(r));
    return r;
}

static void run_id_recall(uint32_t now)
{
    if (!s_id.pending || (int32_t)(now - s_id.due_ms) < 0) {
        return;
    }
    s_id.attempts++;
    const hue_result_t r = send_recall_id(s_id.id, s_id.duration_ms);
    publish(result_name(r));
    if (r == HUE_OK || !hue_result_is_retryable(r) || s_id.attempts >= ID_MAX_ATTEMPTS) {
        s_id.pending = false;
        return;
    }
    s_id.due_ms = app_now_ms() + hue_retry_delay_ms(s_id.attempts);
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
    /* mode: collecting groups, or printing / collecting scenes */
    bool scenes;
    bool print;             /* console listing                          */
    char *json;             /* scene list document, or NULL             */
    size_t json_len;
    unsigned dropped;       /* scenes that did not fit the document     */
    named_t groups[MAX_GROUPS];
    size_t n_groups;
    unsigned printed;
} scan_t;

/* Append text to the document, JSON-escaped when esc. false if it is full. */
static bool json_put(scan_t *s, const char *text, bool esc)
{
    for (const char *p = text; *p != '\0'; p++) {
        char tmp[8];
        const unsigned char c = (unsigned char)*p;
        size_t n;
        if (esc && (c == '"' || c == '\\')) {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            n = 2;
        } else if (esc && c < 0x20) {
            n = (size_t)snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        } else {
            tmp[0] = (char)c;
            n = 1;
        }
        if (s->json_len + n + 3 > SCENES_JSON_MAX) { /* room for "]}" and NUL */
            return false;
        }
        memcpy(s->json + s->json_len, tmp, n);
        s->json_len += n;
    }
    s->json[s->json_len] = '\0';
    return true;
}

static void json_scene(scan_t *s, const named_t *sc, const char *room)
{
    const size_t mark = s->json_len;
    const bool ok = json_put(s, s->printed > 0 ? ",{\"id\":\"" : "{\"id\":\"", false) && json_put(s, sc->id, true) &&
                    json_put(s, "\",\"name\":\"", false) && json_put(s, sc->name, true) &&
                    json_put(s, "\",\"room\":\"", false) && json_put(s, room, true) && json_put(s, "\"}", false);
    if (!ok) {
        s->json_len = mark; /* drop the half-written entry */
        s->json[mark] = '\0';
        s->dropped++;
    }
}

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
        if (s->print) {
            printf("  %s  %-28s %s\n", s->cur.id, s->cur.name, group_name(s, s->group_rid));
        }
        if (s->json != NULL) {
            json_scene(s, &s->cur, group_name(s, s->group_rid));
        }
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
        if (s->print) {
            printf("GET %s: HTTP %d (%s)\n", path, status, status <= 0 ? "no connection" : "error");
        }
        ESP_LOGW(TAG, "GET %s: HTTP %d", path, status);
        return false;
    }
    return true;
}

/* print: on the console; publish: as the scene list document to the sink
 * (docs/PROTOCOL.md, Hue scenes). */
static void list_scenes(bool print, bool publish_doc)
{
    settings_t st;
    app_settings_snapshot(&st, NULL);
    if (!wifi_wait_connected(5000)) {
        if (print) {
            printf("no Wi-Fi\n");
        }
        return;
    }
    if (st.hue_host[0] == '\0' || st.hue_key[0] == '\0') {
        if (print) {
            printf("set hue_host and pair first\n");
        }
        return;
    }
    if (publish_doc && s_sink == NULL) {
        publish_doc = false;
    }
    scan_t *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return;
    }
    s->print = print;
    if (publish_doc) {
        s->json = malloc(SCENES_JSON_MAX);
        if (s->json == NULL) {
            free(s);
            return;
        }
        const time_t t = time(NULL);
        if (t >= CLOCK_VALID_AFTER) {
            s->json_len = (size_t)snprintf(s->json, SCENES_JSON_MAX, "{\"t\":%lld,\"scenes\":[", (long long)t);
        } else {
            s->json_len = (size_t)snprintf(s->json, SCENES_JSON_MAX, "{\"scenes\":[");
        }
    }
    if (scan_get(s, &st, "/clip/v2/resource/room") && scan_get(s, &st, "/clip/v2/resource/zone")) {
        s->scenes = true;
        if (print) {
            printf("scenes (id, name, room):\n");
        }
        if (scan_get(s, &st, "/clip/v2/resource/scene")) {
            if (print) {
                printf("%u scenes. Use: set scene_normal <id> / set scene_ambient <id>\n", s->printed);
            }
            if (s->json != NULL) {
                memcpy(s->json + s->json_len, "]}", 3);
                if (s->dropped > 0) {
                    ESP_LOGW(TAG, "scene list: %u scenes did not fit and were left out", s->dropped);
                }
                s_sink(s->json);
            }
        }
    }
    free(s->json);
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
                s_cmd_seq = ++s_seq;
                s_id.pending = false; /* newer than any waiting id recall */
                hue_sender_submit(s_sender, &req.cmd, app_now_ms());
                break;
            case REQ_RECALL_ID:
                s_id_seq = ++s_seq;
                s_id.pending = true;
                memcpy(s_id.id, req.id, sizeof(s_id.id));
                s_id.duration_ms = req.duration_ms;
                s_id.attempts = 0;
                s_id.due_ms = app_now_ms();
                break;
            case REQ_PAIR:
                do_pairing();
                list_scenes(false, true); /* a new key: the web app gets the scenes */
                break;
            case REQ_LIST_SCENES:
                list_scenes(true, false);
                break;
            case REQ_PUBLISH_SCENES:
                list_scenes(false, true);
                break;
            }
            publish(NULL);
        }

        if (!wifi_is_connected()) {
            continue; /* the command waits (PENDING) until the network is back */
        }
        hue_command_t cmd;
        if (hue_sender_poll(s_sender, app_now_ms(), &cmd)) {
            if (s_cmd_seq < s_id_seq) {
                /* an id recall was asked for after this command: it wins */
                hue_sender_report(s_sender, HUE_OK, app_now_ms());
                ESP_LOGI(TAG, "%s dropped: a newer scene was asked for",
                         cmd.scene == HUE_SCENE_AMBIENT ? "AMBIENT" : "NORMAL");
                continue;
            }
            publish(NULL);
            const hue_result_t r = send_recall(&cmd);
            hue_sender_report(s_sender, r, app_now_ms());
            if (r != HUE_OK && hue_sender_state(s_sender) == HUE_SENDER_WAITING_RETRY) {
                ESP_LOGI(TAG, "retry %lu scheduled", (unsigned long)hue_sender_attempts(s_sender));
            }
            publish(result_name(r));
        }
        run_id_recall(app_now_ms());
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

void hue_task_recall_id(const char *scene_id, uint32_t duration_ms)
{
    if (scene_id == NULL || scene_id[0] == '\0') {
        return;
    }
    req_t req = {.type = REQ_RECALL_ID, .duration_ms = duration_ms};
    strlcpy(req.id, scene_id, sizeof(req.id));
    post(&req);
}

void hue_task_publish_scenes(void)
{
    const req_t req = {.type = REQ_PUBLISH_SCENES};
    post(&req);
}

void hue_task_set_scene_sink(hue_scene_sink_t sink)
{
    s_sink = sink;
}

const char *hue_task_health(void)
{
    if (s_sender == NULL) {
        return "off";
    }
    settings_t st;
    app_settings_snapshot(&st, NULL);
    if (st.hue_host[0] == '\0') {
        return "no_bridge";
    }
    if (st.hue_key[0] == '\0' || hue_sender_state(s_sender) == HUE_SENDER_NEEDS_PAIRING) {
        return "unpaired";
    }
    switch (hue_sender_last_result(s_sender)) {
    case HUE_OK: return "ok";
    case HUE_ERR_TRANSPORT: return "unreachable";
    case HUE_ERR_AUTH: return "unpaired";
    case HUE_ERR_NOT_FOUND: return "scene_missing";
    default: return "error";
    }
}
