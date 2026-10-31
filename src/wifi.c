/*
 * wifi – see wifi.h.
 */
#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "app_settings.h"
#include "app_status.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#define BIT_CONNECTED    BIT0
#define RETRY_FIRST_MS   1000u
#define RETRY_MAX_MS     30000u

static const char *TAG = "wifi";

static EventGroupHandle_t s_bits;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_ms = RETRY_FIRST_MS;
static bool s_started;     /* esp_wifi_start() done */
static bool s_configured;  /* an SSID is set */

static void set_status(bool connected, const char *ip)
{
    app_status_t *s = app_status_lock();
    s->wifi_connected = connected;
    app_status_copy(s->wifi_ip, sizeof(s->wifi_ip), ip);
    app_status_unlock();
}

static void retry_cb(void *arg)
{
    (void)arg;
    if (s_configured) {
        esp_wifi_connect();
    }
}

static void schedule_retry(void)
{
    ESP_LOGI(TAG, "reconnecting in %lu s", (unsigned long)(s_retry_ms / 1000u));
    esp_timer_stop(s_retry_timer); /* fine if not running */
    esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_ms * 1000u);
    s_retry_ms = (s_retry_ms >= RETRY_MAX_MS / 2) ? RETRY_MAX_MS : s_retry_ms * 2;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_configured) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_bits, BIT_CONNECTED);
        set_status(false, "");
        ESP_LOGW(TAG, "disconnected (reason %d)", d ? (int)d->reason : -1);
        if (s_configured) {
            schedule_retry();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "connected, IP %s", ip);
        s_retry_ms = RETRY_FIRST_MS;
        set_status(true, ip);
        xEventGroupSetBits(s_bits, BIT_CONNECTED);
    }
}

/* Copy the credentials from the settings into the driver. */
static void apply_credentials(void)
{
    settings_t st;
    app_settings_snapshot(&st, NULL);
    /* The console setting wins; otherwise the compiled-in network (secrets.h). */
    const bool from_console = (st.wifi_ssid[0] != '\0');
    const char *ssid = from_console ? st.wifi_ssid : WIFI_SSID;
    const char *pass = from_console ? st.wifi_pass : WIFI_PASSWORD;

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    s_configured = (ssid[0] != '\0');
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    if (!s_configured) {
        ESP_LOGW(TAG, "no network: fill in src/secrets.h, or 'set wifi_ssid <name>' and 'set wifi_pass <password>'");
    } else {
        ESP_LOGI(TAG, "network \"%s\" (%s)", ssid, from_console ? "console setting" : "secrets.h");
    }
}

void wifi_start(void)
{
    s_bits = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM)); /* credentials live in our settings */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    const esp_timer_create_args_t targs = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_credentials();
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
}

void wifi_reconnect(void)
{
    if (!s_started) {
        return;
    }
    esp_timer_stop(s_retry_timer);
    s_retry_ms = RETRY_FIRST_MS;
    s_configured = false;      /* the disconnect below must not schedule a retry */
    esp_wifi_disconnect();
    apply_credentials();
    if (s_configured) {
        esp_wifi_connect();
    }
}

bool wifi_is_connected(void)
{
    return s_bits != NULL && (xEventGroupGetBits(s_bits) & BIT_CONNECTED) != 0;
}

bool wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_bits == NULL) {
        return false;
    }
    const EventBits_t b = xEventGroupWaitBits(s_bits, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0;
}
