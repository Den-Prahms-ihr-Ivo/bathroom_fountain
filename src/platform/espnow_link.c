/*
 * espnow_link – see espnow_link.h.
 */
#include "espnow_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "events.h"
#include "link_peer.h"

#define LINK_PERIOD_MS   10u
#define LINK_TASK_STACK  4096u
#define LINK_TASK_PRIO   6u
#define RX_QUEUE_LEN     16

static const char *TAG = "espnow";

typedef struct {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[LINK_FRAME_MAX];
} rx_item_t;

static const uint8_t BROADCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static struct {
    QueueHandle_t rx_q;
    SemaphoreHandle_t mutex;
    link_peer_t *lp;
    espnow_link_rx_t rx;
    link_hello_t hello;
    uint8_t peer[6];
    bool have_peer;
    uint8_t hb_state;
    volatile bool up;
} s;

/* ------------------------------------------------------------- radio --- */

static void add_peer(const uint8_t mac[6])
{
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0; /* the station's channel */
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false; /* TODO: key from pairing (PROTOCOL: ESP-NOW encryption) */
    esp_now_add_peer(&p);
}

static void radio_send(const uint8_t *frame, size_t len)
{
    esp_now_send(s.have_peer ? s.peer : BROADCAST, frame, len);
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len <= 0 || len > LINK_FRAME_MAX) {
        return;
    }
    rx_item_t it;
    memcpy(it.mac, info->src_addr, 6);
    it.len = (uint8_t)len;
    memcpy(it.data, data, (size_t)len);
    xQueueSend(s.rx_q, &it, 0); /* Wi-Fi task: never block */
}

static void load_peer(void)
{
    nvs_handle_t h;
    if (nvs_open("espnow", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s.peer);
    s.have_peer = (nvs_get_blob(h, "peer", s.peer, &len) == ESP_OK && len == sizeof(s.peer));
    nvs_close(h);
    if (s.have_peer) {
        add_peer(s.peer);
    }
}

static void learn_peer(const uint8_t mac[6])
{
    if (s.have_peer && memcmp(mac, s.peer, 6) == 0) {
        return;
    }
    /* TODO: only inside a pairing window (cmd/link/pair); now the first HELLO wins */
    memcpy(s.peer, mac, 6);
    s.have_peer = true;
    add_peer(mac);
    nvs_handle_t h;
    if (nvs_open("espnow", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "peer", mac, 6);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "peer %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ------------------------------------------------------------- frames --- */

static void send_plain(link_msg_t *m)
{
    uint8_t f[LINK_FRAME_MAX];
    m->seq = 0;
    m->flags = 0;
    const size_t n = link_msg_encode(m, f, sizeof(f));
    if (n > 0) {
        radio_send(f, n);
    }
}

static void send_ack(uint16_t seq)
{
    link_msg_t a = {.type = LINK_ACK};
    a.u.ack.acked_seq = seq;
    send_plain(&a);
}

static void send_hello(void)
{
    link_msg_t h = {.type = LINK_HELLO};
    h.u.hello = s.hello;
    send_plain(&h);
}

static void handle_rx(const rx_item_t *it, uint32_t now)
{
    link_msg_t m;
    if (!link_msg_decode(it->data, it->len, &m)) {
        return;
    }
    if (m.type == LINK_HELLO) {
        learn_peer(it->mac);
        link_peer_reset_rx(s.lp);
    } else if (!s.have_peer || memcmp(it->mac, s.peer, 6) != 0) {
        return; /* frames from other nodes are dropped (one peer for now) */
    }
    const bool act = link_peer_on_rx(s.lp, m.seq, now);
    if (m.flags & LINK_FLAG_ACK_REQ) {
        send_ack(m.seq); /* also for a repeat: the peer missed our ack */
    }
    if (m.type == LINK_ACK) {
        link_peer_on_ack(s.lp, m.u.ack.acked_seq, now);
        return;
    }
    if (act && s.rx != NULL) {
        s.rx(&m);
    }
}

static void link_task(void *arg)
{
    (void)arg;
    send_hello();
    for (;;) {
        rx_item_t it;
        const uint32_t now = app_now_ms();
        xSemaphoreTake(s.mutex, portMAX_DELAY);
        while (xQueueReceive(s.rx_q, &it, 0) == pdTRUE) {
            handle_rx(&it, now);
        }
        link_peer_tick(s.lp, now);
        uint8_t f[LINK_PEER_FRAME_MAX];
        const size_t n = link_peer_take_tx(s.lp, now, f, sizeof(f));
        if (n > 0) {
            radio_send(f, n);
        }
        if (link_peer_heartbeat_due(s.lp, now)) {
            link_msg_t hb = {.type = LINK_HEARTBEAT};
            hb.u.heartbeat.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
            hb.u.heartbeat.state = s.hb_state;
            send_plain(&hb);
        }
        link_peer_event_t ev;
        while (link_peer_take_event(s.lp, &ev)) {
            if (ev.id == LINK_PEER_UP) {
                s.up = true;
                send_hello(); /* the peer may have restarted: it wants our folders etc. */
            } else if (ev.id == LINK_PEER_DOWN) {
                s.up = false;
            } else if (ev.id == LINK_PEER_FAILED) {
                ESP_LOGW(TAG, "frame %u not acknowledged", (unsigned)ev.seq);
            }
        }
        xSemaphoreGive(s.mutex);
        vTaskDelay(pdMS_TO_TICKS(LINK_PERIOD_MS));
    }
}

/* ------------------------------------------------------------- public --- */

bool espnow_link_start(link_node_t self, uint16_t caps, const char *name, espnow_link_rx_t rx)
{
    s.rx = rx;
    s.hello.node = (uint8_t)self;
    s.hello.caps = caps;
    strncpy(s.hello.name, name, LINK_NAME_LEN);
    /* TODO: fw_major/minor/patch from esp_app_get_description()->version */
    s.rx_q = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_item_t));
    s.mutex = xSemaphoreCreateMutex();
    link_peer_config_t cfg;
    link_peer_config_default(&cfg);
    s.lp = link_peer_create(&cfg, app_now_ms());
    if (s.rx_q == NULL || s.mutex == NULL || s.lp == NULL) {
        ESP_LOGE(TAG, "init failed");
        return false;
    }
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed (Wi-Fi started?)");
        return false;
    }
    esp_now_register_recv_cb(recv_cb);
    add_peer(BROADCAST);
    load_peer();
    xTaskCreate(link_task, "espnow", LINK_TASK_STACK, NULL, LINK_TASK_PRIO, NULL);
    return true;
}

bool espnow_link_send(link_msg_t *msg, bool ack)
{
    if (s.lp == NULL || msg == NULL) {
        return false;
    }
    xSemaphoreTake(s.mutex, portMAX_DELAY);
    bool ok;
    if (ack) {
        msg->seq = link_peer_next_seq(s.lp);
        msg->flags = LINK_FLAG_ACK_REQ;
        uint8_t f[LINK_FRAME_MAX];
        const size_t n = link_msg_encode(msg, f, sizeof(f));
        ok = n > 0 && link_peer_send(s.lp, f, n, app_now_ms());
    } else {
        send_plain(msg);
        ok = true;
    }
    xSemaphoreGive(s.mutex);
    return ok;
}

bool espnow_link_peer_up(void)
{
    return s.up;
}

void espnow_link_set_heartbeat_state(uint8_t state)
{
    s.hb_state = state;
}
