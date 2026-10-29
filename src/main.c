/*
 * Firmware entry point.
 *
 * Skeleton only: the task layout is described in docs/ARCHITECTURE.md.
 * Hardware-independent logic lives in lib/ and is developed test-first
 * against the native environment (pio test -e native).
 */
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "bathroom-ambience skeleton: no tasks started yet");
}
