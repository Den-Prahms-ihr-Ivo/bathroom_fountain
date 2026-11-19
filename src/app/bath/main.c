/*
 * Firmware entry point.
 *
 * The task layout is described in docs/ARCHITECTURE.md. Hardware-independent
 * logic lives in lib/ and is developed test-first against the native
 * environment (pio test -e native).
 *
 * Tasks: mic_task (double clap → EVT_CLAP), input_task (button, encoder),
 * control_task (mode, volume, outputs, amplifier), player_task (microSD →
 * MP3 → I2S), hue_task (bridge over HTTPS), console_task (settings, status),
 * plus Wi-Fi in the background.
 */
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "console_task.h"
#include "control_task.h"
#include "events.h"
#include "hue_task.h"
#include "input_task.h"
#include "mic_task.h"
#include "player_task.h"
#include "wifi.h"

static const char *TAG = "main";

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

void app_main(void)
{
    nvs_start();
    const esp_err_t err = app_settings_load();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settings not loaded from NVS (%s), using defaults", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(events_init() ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_start();          /* first: the hardware RNG is truly random once the radio runs */
    hue_task_start();      /* queues exist before control_task sends to them */
    player_task_start();
    control_task_start();
    input_task_start();
    mic_task_start();
    console_task_start();
}
