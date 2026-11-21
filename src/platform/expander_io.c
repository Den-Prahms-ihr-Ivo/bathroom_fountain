/*
 * expander_io – see expander_io.h.
 */
#include "expander_io.h"

#include "board_pins.h"

#if defined(EXPANDER_I2C_ADDR) && defined(EXPANDER_ON_AMP_BUS) && defined(PIN_AMP_I2C_SDA)

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "amp_io.h"
#include "button.h"
#include "events.h"
#include "tca9555.h"

#define XP_PERIOD_MS     10u
#define XP_RETRY_MS      5000u
#define XP_TIMEOUT_MS    20
#define XP_I2C_HZ        100000
#define XP_TASK_STACK    3072u
#define XP_TASK_PRIO     6u
#define COLD_HOLD_MS     2000u
#define SWITCH_STABLE_MS 50u

static const char *TAG = "expander";

static struct {
    i2c_master_dev_handle_t dev;
    tca9555_t *chip;
    button_t *cold;
    volatile bool present;
    volatile bool hype;
    volatile bool led_cold;
    bool led_cold_applied;
    bool led_hype_applied;
    bool have_switch;
    bool switch_raw;
    uint32_t switch_since;
} s;

/* --------------------------------------------------------------- bus --- */

static int xp_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    (void)ctx;
    uint8_t buf[3];
    if (len > 2) {
        return -1;
    }
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_transmit(s.dev, buf, len + 1, XP_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int xp_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    (void)ctx;
    return i2c_master_transmit_receive(s.dev, &reg, 1, data, len, XP_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static const tca9555_io_t s_io = {.ctx = NULL, .write = xp_write, .read = xp_read};

static bool chip_init(void)
{
    const tca9555_config_t cfg = {
        .output_pins = XP_OUTPUT_MASK,
        .initial_levels = 0, /* LEDs off */
        .inverted_inputs = XP_INVERTED_MASK,
    };
    const tca9555_err_t r = tca9555_init(s.chip, &cfg);
    if (r != TCA9555_OK) {
        return false;
    }
    s.led_cold_applied = !s.led_cold; /* force both LEDs on the next tick */
    s.led_hype_applied = !s.hype;
    s.have_switch = false;
    return true;
}

/* -------------------------------------------------------------- task --- */

static void apply_leds(void)
{
    if (s.led_cold != s.led_cold_applied) {
        if (tca9555_write_pin(s.chip, XP_COLD_LED, s.led_cold) == TCA9555_OK) {
            s.led_cold_applied = s.led_cold;
        }
    }
    if (s.hype != s.led_hype_applied) {
        if (tca9555_write_pin(s.chip, XP_HYPE_LED, s.hype) == TCA9555_OK) {
            s.led_hype_applied = s.hype;
        }
    }
}

static void poll(uint32_t now)
{
    uint16_t levels;
    if (tca9555_read(s.chip, &levels) != TCA9555_OK) {
        ESP_LOGW(TAG, "read failed: chip gone?");
        s.present = false;
        return;
    }
    /* cold button: lib/button debounces, short = next / start, long = abort */
    const bool down = (levels >> XP_COLD_BUTTON) & 1u;
    switch (button_update(s.cold, down, now)) {
    case BUTTON_SHORT:
        events_post(EVT_COLD_PRESS, 0);
        break;
    case BUTTON_LONG:
        events_post(EVT_COLD_HOLD, 0);
        break;
    case BUTTON_NONE:
        break;
    }
    /* switch: stable for SWITCH_STABLE_MS before it counts */
    const bool raw = (levels >> XP_HYPE_SWITCH) & 1u;
    if (!s.have_switch || raw != s.switch_raw) {
        s.switch_raw = raw;
        s.switch_since = now;
        if (!s.have_switch) {
            s.have_switch = true;
            s.hype = !raw; /* forces one report below */
        }
    }
    if (raw != s.hype && (uint32_t)(now - s.switch_since) >= SWITCH_STABLE_MS) {
        s.hype = raw;
        ESP_LOGI(TAG, "switch: %s", raw ? "hype" : "zen");
        events_post(EVT_COLD_SWITCH, raw ? 1 : 0);
    }
    apply_leds();
}

static void expander_task(void *arg)
{
    (void)arg;
    uint32_t retry_at = 0;
    bool warned = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(XP_PERIOD_MS));
        const uint32_t now = app_now_ms();
        if (!s.present) {
            if ((int32_t)(now - retry_at) < 0) {
                continue;
            }
            retry_at = now + XP_RETRY_MS;
            if (chip_init()) {
                s.present = true;
                warned = false;
                ESP_LOGI(TAG, "TCA9555 at 0x%02X ready", EXPANDER_I2C_ADDR);
            } else if (!warned) {
                ESP_LOGW(TAG, "no TCA9555 at 0x%02X: cold button and switch unavailable (retrying)",
                         EXPANDER_I2C_ADDR);
                warned = true;
            }
            continue;
        }
        poll(now);
    }
}

void expander_io_start(void)
{
    i2c_master_bus_handle_t bus = amp_io_bus();
    if (bus == NULL) {
        return;
    }
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = EXPANDER_I2C_ADDR,
        .scl_speed_hz = XP_I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev, &s.dev) != ESP_OK) {
        ESP_LOGE(TAG, "cannot add the expander to the bus");
        return;
    }
    button_config_t bc;
    button_config_default(&bc);
    bc.long_press_ms = COLD_HOLD_MS;
    s.cold = button_create(&bc, app_now_ms());
    s.chip = tca9555_create(&s_io);
    if (s.cold == NULL || s.chip == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    xTaskCreate(expander_task, "expander", XP_TASK_STACK, NULL, XP_TASK_PRIO, NULL);
}

void expander_io_cold_led(bool on)
{
    s.led_cold = on;
}

bool expander_io_hype(void)
{
    return s.present && s.hype;
}

bool expander_io_present(void)
{
    return s.present;
}

#else /* no expander on this board */

void expander_io_start(void) {}
void expander_io_cold_led(bool on) { (void)on; }
bool expander_io_hype(void) { return false; }
bool expander_io_present(void) { return false; }

#endif
