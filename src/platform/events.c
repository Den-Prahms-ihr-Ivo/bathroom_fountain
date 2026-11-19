/*
 * events – see events.h.
 */
#include "events.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define EVENT_QUEUE_LEN 16u

static const char *TAG = "events";

static QueueHandle_t s_queue;

bool events_init(void)
{
    s_queue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(event_t));
    return s_queue != NULL;
}

bool events_post(event_type_t type, int32_t detents)
{
    const event_t ev = {.type = type, .time_ms = app_now_ms(), .detents = detents};
    if (s_queue == NULL || xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, event %d dropped", (int)type);
        return false;
    }
    return true;
}

bool events_receive(event_t *ev, uint32_t timeout_ms)
{
    return xQueueReceive(s_queue, ev, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
