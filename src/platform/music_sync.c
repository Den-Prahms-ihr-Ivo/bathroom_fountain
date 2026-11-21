/*
 * music_sync – see music_sync.h. SKELETON: the task runs, does nothing yet.
 */
#include "music_sync.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SYNC_TASK_STACK 8192u
#define SYNC_TASK_PRIO  2u
#define SYNC_PAYLOAD_MAX 4096u

static const char *TAG = "music_sync";
static volatile bool s_running;

static void sync_task(void *arg)
{
    char *payload = arg;
    ESP_LOGI(TAG, "sync requested (%u bytes)", (unsigned)strlen(payload));
    /* TODO: steps 1-5 of music_sync.h */
    free(payload);
    s_running = false;
    vTaskDelete(NULL);
}

bool music_sync_start(const char *payload, size_t len)
{
    if (s_running || payload == NULL || len == 0 || len > SYNC_PAYLOAD_MAX) {
        return false;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, payload, len);
    copy[len] = '\0';
    s_running = true;
    if (xTaskCreate(sync_task, "music_sync", SYNC_TASK_STACK, copy, SYNC_TASK_PRIO, NULL) != pdPASS) {
        free(copy);
        s_running = false;
        return false;
    }
    return true;
}

bool music_sync_running(void)
{
    return s_running;
}
