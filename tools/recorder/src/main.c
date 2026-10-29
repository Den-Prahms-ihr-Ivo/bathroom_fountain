/*
 * Recorder firmware for the ESP32-S3-DevKitC-1 (also runs on the Louder).
 *
 * Reads the INMP441 over I2S at 16 kHz and streams every sample to the PC as
 * framed 24-bit PCM over UART0 at 921600 baud (the DevKitC-1 port marked
 * UART). tools/capture.py turns the stream into WAV files.
 *
 * Privacy (NFR-01): this firmware is for the bench recordings only. Nothing
 * is stored on the board; audio leaves it only over the USB cable to your PC.
 *
 * Build flags (platformio.ini, build_flags):
 *   -DMIC_SLOT_RIGHT   read the right I2S slot (if the left one is silent)
 */
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "frame.h"

#define PIN_MIC_SCK    GPIO_NUM_41
#define PIN_MIC_WS     GPIO_NUM_40
#define PIN_MIC_SD     GPIO_NUM_39

#define SAMPLE_RATE_HZ 16000u
#define FRAME_SAMPLES  256u           /* 16 ms per frame */
#define UART_PORT      UART_NUM_0
#define UART_BAUD      921600u
#define UART_TX_BUF    (16u * 1024u)  /* about 20 frames */

static volatile uint32_t s_overflows;

static bool IRAM_ATTR on_rx_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle;
    (void)event;
    (void)user_ctx;
    s_overflows++;
    return false;
}

static i2s_chan_handle_t mic_start(void)
{
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = FRAME_SAMPLES;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_MIC_SCK,
            .ws = PIN_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_MIC_SD,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
#ifdef MIC_SLOT_RIGHT
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
#else
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT; /* INMP441 with L/R tied to GND */
#endif
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_cfg));

    const i2s_event_callbacks_t cbs = {.on_recv_q_ovf = on_rx_overflow};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx, &cbs, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    return rx;
}

void app_main(void)
{
    /* From here on UART0 carries binary frames only. */
    esp_log_level_set("*", ESP_LOG_NONE);
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, UART_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_baudrate(UART_PORT, UART_BAUD));

    i2s_chan_handle_t rx = mic_start();

    static int32_t words[FRAME_SAMPLES];
    static uint8_t out[FRAME_BYTES(FRAME_SAMPLES)];
    uint32_t seq = 0;
    uint32_t seen_overflows = 0;

    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, words, sizeof(words), &got, 1000) != ESP_OK || got != sizeof(words)) {
            continue;
        }
        for (size_t i = 0; i < FRAME_SAMPLES; i++) {
            words[i] >>= 8; /* 24-bit sample, left-aligned in the 32-bit I2S word */
        }
        uint16_t flags = 0;
        const uint32_t ovf = s_overflows;
        if (ovf != seen_overflows) {
            flags |= FRAME_FLAG_I2S_OVERFLOW;
            seen_overflows = ovf;
        }
        const size_t len = frame_encode(out, sizeof(out), seq++, flags, words, FRAME_SAMPLES);
        uart_write_bytes(UART_PORT, out, len); /* blocks while the TX buffer is full */
    }
}
