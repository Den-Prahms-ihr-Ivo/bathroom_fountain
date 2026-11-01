/*
 * input_task – see input_task.h.
 */
#include "input_task.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "button.h"
#include "events.h"
#include "hue_task.h"

#define INPUT_POLL_MS      10   /* one tick at the default 100 Hz; plenty for a 30 ms debounce */
#define INPUT_TASK_STACK   3072u
#define INPUT_TASK_PRIO    6u
#define INPUT_TASK_CORE    1

#ifndef ENC_COUNTS_PER_DETENT
#define ENC_COUNTS_PER_DETENT 4
#endif
#define ENC_PCNT_LIMIT     1000   /* hardware limit; accum_count extends it */
#define ENC_GLITCH_NS      10000  /* PCNT filter; the RC on the pins does the rest */

static const char *TAG = "input";

/* ---------------------------------------------------------------- button --- */

static void button_gpio_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

/* --------------------------------------------------------------- encoder --- */

#if defined(PIN_ENC_A) && defined(PIN_ENC_B)
#define HAVE_ENCODER 1

static pcnt_unit_handle_t s_pcnt;

static bool encoder_init(void)
{
    /* Plain GPIO inputs first: on the Louder, 44 starts out as UART0 RX. */
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_ENC_A) | (1ULL << PIN_ENC_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    const pcnt_unit_config_t unit_cfg = {
        .high_limit = ENC_PCNT_LIMIT,
        .low_limit = -ENC_PCNT_LIMIT,
        .flags.accum_count = 1,
    };
    if (pcnt_new_unit(&unit_cfg, &s_pcnt) != ESP_OK) {
        return false;
    }
    const pcnt_glitch_filter_config_t filter = {.max_glitch_ns = ENC_GLITCH_NS};
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &filter));

    const pcnt_chan_config_t a_cfg = {.edge_gpio_num = PIN_ENC_A, .level_gpio_num = PIN_ENC_B};
    const pcnt_chan_config_t b_cfg = {.edge_gpio_num = PIN_ENC_B, .level_gpio_num = PIN_ENC_A};
    pcnt_channel_handle_t a = NULL;
    pcnt_channel_handle_t b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &a_cfg, &a));
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &b_cfg, &b));
    /* Full quadrature decoding: 4 counts per cycle. */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    /* accum_count needs the limits as watch points to carry the overflow. */
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt, ENC_PCNT_LIMIT));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(s_pcnt, -ENC_PCNT_LIMIT));

    /* The pins idle high: pull-ups (internal here, 10 kΩ external recommended). */
    gpio_pullup_en(PIN_ENC_A);
    gpio_pullup_en(PIN_ENC_B);

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));
    return true;
}

/* Whole detents since the last call; the remainder stays for the next one. */
static int32_t encoder_take_detents(int *consumed)
{
    int count = 0;
    if (pcnt_unit_get_count(s_pcnt, &count) != ESP_OK) {
        return 0;
    }
    const int delta = count - *consumed;
    const int32_t detents = delta / ENC_COUNTS_PER_DETENT; /* truncates toward zero */
    *consumed += (int)detents * ENC_COUNTS_PER_DETENT;
#ifdef ENC_REVERSE
    return -detents;
#else
    return detents;
#endif
}
#else
#define HAVE_ENCODER 0
#endif

/* ------------------------------------------------------------------ task --- */

static void input_task(void *arg)
{
    (void)arg;
    button_config_t cfg;
    button_config_default(&cfg);
    button_t *button = button_create(&cfg, app_now_ms());
    if (button == NULL) {
        ESP_LOGE(TAG, "button_create failed");
        vTaskDelete(NULL);
        return;
    }

#if HAVE_ENCODER
    const bool encoder = encoder_init();
    if (!encoder) {
        ESP_LOGE(TAG, "encoder: no PCNT unit, volume knob disabled");
    }
    int consumed = 0;
#endif

    TickType_t period = pdMS_TO_TICKS(INPUT_POLL_MS);
    if (period == 0) {
        period = 1;
    }
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, period);

        const bool pressed = (gpio_get_level(PIN_BUTTON) == 0); /* switch to GND, pull-up */
        switch (button_update(button, pressed, app_now_ms())) {
        case BUTTON_SHORT:
            events_post(EVT_BUTTON, 0);
            break;
        case BUTTON_LONG:
            ESP_LOGI(TAG, "long press: Hue pairing (press the bridge's link button)");
            hue_task_request_pairing();
            break;
        case BUTTON_NONE:
            break;
        }

#if HAVE_ENCODER
        if (encoder) {
            const int32_t detents = encoder_take_detents(&consumed);
            if (detents != 0) {
                events_post(EVT_VOLUME, detents);
            }
        }
#endif
    }
}

void input_task_start(void)
{
    button_gpio_init();
    xTaskCreatePinnedToCore(input_task, "input", INPUT_TASK_STACK, NULL, INPUT_TASK_PRIO, NULL, INPUT_TASK_CORE);
}
