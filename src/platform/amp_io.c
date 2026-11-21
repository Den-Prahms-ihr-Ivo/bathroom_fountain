/*
 * amp_io – see amp_io.h.
 */
#include "amp_io.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"

static const char *TAG = "amp_io";

static void real_delay(void *ctx, uint32_t ms)
{
    (void)ctx;
    /* At least ms: round up to whole ticks and add one for the partial tick
     * already running. */
    vTaskDelay((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1);
}

static void output_pin(int pin)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(pin, 0);
}

#if defined(PIN_AMP_I2C_SDA)
/* ------------------------------------------------------------ real I2C --- */

#include "driver/i2c_master.h"

#define I2C_HZ         100000
#define I2C_TIMEOUT_MS 50

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static uint8_t s_addr;

static const uint8_t CANDIDATES[] = {0x4C, 0x4D, 0x4E, 0x4F, 0x2C, 0x2D, 0x2E, 0x2F};

static bool ensure_device(void)
{
    if (s_dev != NULL) {
        return true;
    }
    for (size_t i = 0; i < sizeof(CANDIDATES); i++) {
        if (i2c_master_probe(s_bus, CANDIDATES[i], I2C_TIMEOUT_MS) == ESP_OK) {
            const i2c_device_config_t dev = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = CANDIDATES[i],
                .scl_speed_hz = I2C_HZ,
            };
            if (i2c_master_bus_add_device(s_bus, &dev, &s_dev) != ESP_OK) {
                return false;
            }
            s_addr = CANDIDATES[i];
            ESP_LOGI(TAG, "amplifier found at I2C address 0x%02X", s_addr);
            return true;
        }
    }
    ESP_LOGE(TAG, "no amplifier answers on I2C (SDA %d, SCL %d): is the board powered with 12 V?",
             PIN_AMP_I2C_SDA, PIN_AMP_I2C_SCL);
    return false;
}

static int i2c_write(void *ctx, uint8_t reg, uint8_t value)
{
    (void)ctx;
    if (!ensure_device()) {
        return -1;
    }
    const uint8_t buf[2] = {reg, value};
    const esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write 0x%02X: %s", reg, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int i2c_read(void *ctx, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    if (!ensure_device()) {
        return -1;
    }
    const esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read 0x%02X: %s", reg, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static void set_pdn(void *ctx, bool high)
{
    (void)ctx;
    gpio_set_level(PIN_AMP_PWDN, high ? 1 : 0);
}

static const tas5825m_io_t s_io = {
    .ctx = NULL,
    .write_reg = i2c_write,
    .read_reg = i2c_read,
    .set_pdn = set_pdn,
    .delay_ms = real_delay,
};

static void bus_init(void)
{
    if (s_bus != NULL) {
        return;
    }
    const i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_AMP_I2C_SDA,
        .scl_io_num = PIN_AMP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &s_bus));
}

const tas5825m_io_t *amp_io_get(void)
{
    output_pin(PIN_AMP_PWDN); /* low: amplifier off until the driver says otherwise */
    bus_init();
    return &s_io;
}

void *amp_io_bus(void)
{
    bus_init(); /* the expander shares the amplifier's bus (expander_io.c) */
    return s_bus;
}

uint8_t amp_io_address(void)
{
    return s_addr;
}

#else
/* ------------------------------------------------------------ log only --- */

static int log_write(void *ctx, uint8_t reg, uint8_t value)
{
    (void)ctx;
    ESP_LOGD(TAG, "I2C write reg 0x%02X = 0x%02X", reg, value);
    return 0;
}

static int log_read(void *ctx, uint8_t reg, uint8_t *value)
{
    (void)ctx;
    *value = 0;
    ESP_LOGD(TAG, "I2C read  reg 0x%02X -> 0x00", reg);
    return 0;
}

static void log_pdn(void *ctx, bool high)
{
    (void)ctx;
    ESP_LOGI(TAG, "PDN %s (log only, no amplifier on this board)", high ? "high" : "low");
#ifdef PIN_LED_AMP
    gpio_set_level(PIN_LED_AMP, high ? 1 : 0);
#endif
}

static const tas5825m_io_t s_io = {
    .ctx = NULL,
    .write_reg = log_write,
    .read_reg = log_read,
    .set_pdn = log_pdn,
    .delay_ms = real_delay,
};

const tas5825m_io_t *amp_io_get(void)
{
#ifdef PIN_LED_AMP
    output_pin(PIN_LED_AMP);
#endif
    return &s_io;
}

uint8_t amp_io_address(void)
{
    return 0;
}

void *amp_io_bus(void)
{
    return NULL;
}
#endif

int amp_io_read(uint8_t reg, uint8_t *value)
{
    return s_io.read_reg(s_io.ctx, reg, value);
}

int amp_io_write(uint8_t reg, uint8_t value)
{
    return s_io.write_reg(s_io.ctx, reg, value);
}
