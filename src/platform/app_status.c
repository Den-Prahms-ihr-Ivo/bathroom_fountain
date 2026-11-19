/*
 * app_status – see app_status.h.
 */
#include "app_status.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static app_status_t s_status;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

app_status_t *app_status_lock(void)
{
    taskENTER_CRITICAL(&s_mux);
    return &s_status;
}

void app_status_unlock(void)
{
    taskEXIT_CRITICAL(&s_mux);
}

void app_status_get(app_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

void app_status_copy(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    size_t n = 0;
    if (src != NULL) {
        while (n + 1 < dst_size && src[n] != '\0') {
            dst[n] = src[n];
            n++;
        }
    }
    dst[n] = '\0';
}
