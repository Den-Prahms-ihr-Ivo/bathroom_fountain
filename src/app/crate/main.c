/*
 * crate – the holy crate (Louder-ESP32-S3 Plus on a 12.8 V LiFePO4 battery):
 * lid open = light and choir, lid closed = fade out and deep sleep.
 *
 * Skeleton: builds and boots, does nothing yet. The plan for this app is in
 * docs/CRATE.md; the decisions are in lib/crate_controller.
 */
#include "esp_log.h"

#include "board_pins.h"

static const char *TAG = "crate";

void app_main(void)
{
    ESP_LOGI(TAG, "crate firmware skeleton: nothing implemented yet");
}
