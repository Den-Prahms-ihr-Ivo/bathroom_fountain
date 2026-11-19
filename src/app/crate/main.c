/*
 * crate – the holy crate (Louder-ESP32-S3 Plus on a 12.8 V LiFePO4 battery):
 * lid open = light and choir, lid closed = fade out and deep sleep.
 *
 * Tasks: crate_task (lid, battery, light, amplifier, sleep), player_task
 * (microSD -> MP3 -> I2S), input_task (volume encoder, press = next track).
 * No Wi-Fi. The plan is in docs/CRATE.md; the decisions are in
 * lib/crate_controller.
 */
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "board_pins.h"
#include "crate_task.h"
#include "events.h"
#include "input_task.h"
#include "player_task.h"

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
    ESP_ERROR_CHECK(events_init() ? ESP_OK : ESP_ERR_NO_MEM);
    player_task_start();
    crate_task_start();   /* amplifier, light and lid before the encoder posts */
    input_task_start();
}
