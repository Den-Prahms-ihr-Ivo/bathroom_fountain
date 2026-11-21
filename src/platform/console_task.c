/*
 * console_task – set / get / show over the serial console. See console_task.h.
 */
#include "console_task.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#define HAVE_USB_CONSOLE 1
#else
#define HAVE_USB_CONSOLE 0
#endif

#include "app_settings.h"
#include "app_status.h"
#include "events.h"
#include "hue_task.h"
#include "player_task.h"
#include "server_link.h"
#include "wifi.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "input_task.h"
#include "amp_io.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"

/* On the Louder, UART0's pins 43/44 carry the encoder, so typing works on the
 * USB-C port only. */
#if CONFIG_ESP_CONSOLE_UART && !defined(UART0_PINS_ARE_GPIO)
#define CONSOLE_UART       CONFIG_ESP_CONSOLE_UART_NUM
#define HAVE_UART_CONSOLE  1
#else
#define HAVE_UART_CONSOLE  0
#endif
#define CONSOLE_LINE_MAX   160u
#define CONSOLE_TASK_STACK 4096u
#define CONSOLE_TASK_PRIO  2u
#define CONSOLE_TASK_CORE  0

static const char *TAG = "console";

static SemaphoreHandle_t s_run_lock; /* one command at a time, whichever port it came from */

static void print_value(const char *key)
{
    char text[APP_SETTINGS_VALUE_MAX];
    if (app_settings_format(key, false, text, sizeof(text))) {
        printf("%s = %s\n", key, text);
    } else {
        printf("unknown key: %s\n", key);
    }
}

static void cmd_show(void)
{
    for (size_t i = 0; i < settings_key_count(); i++) {
        print_value(settings_key_name(i));
    }
}

static void cmd_set(const char *key, const char *value)
{
    esp_err_t saved = ESP_OK;
    switch (app_settings_set(key, value, &saved)) {
    case SETTINGS_OK:
        print_value(key);
        server_link_settings_changed_locally(); /* the web app sees it (reported) */
        if (saved != ESP_OK) {
            printf("  not saved to flash: %s\n", esp_err_to_name(saved));
        }
        if (strncmp(key, "wifi_", 5) == 0) {
            printf("  reconnecting Wi-Fi\n");
            wifi_reconnect();
        } else if (strcmp(key, "transition_ms") == 0 || strcmp(key, "volume_max") == 0) {
            printf("  takes effect after 'reboot'\n");
        }
        break;
    case SETTINGS_ERR_UNKNOWN_KEY:
        printf("unknown key: %s\n", key);
        break;
    case SETTINGS_ERR_INVALID:
        printf("invalid value for %s\n", key);
        break;
    case SETTINGS_ERR_RANGE:
        printf("out of range for %s\n", key);
        break;
    case SETTINGS_ERR_CONFLICT:
        printf("conflict: clap_min_gap_ms must stay below clap_max_gap_ms\n");
        break;
    }
}

static void cmd_help(void)
{
    printf("settings:  set <key> <value> | get <key> | show\n"
           "device:    status | next | rescan | reboot\n"
           "hue:       hue pair | hue scenes | hue normal | hue ambient\n"
           "cold:      cold start | cold next | cold abort | voice 0..3 | beep\n"
           "test:      io | io watch | io candles on|off | io fountain on|off | amp | amp play | amp tone on|off\n");
}

static void cmd_status(void)
{
    app_status_t st;
    app_status_get(&st);
    printf("mode:      %s%s\n", st.ambient ? "Ambient" : "Normal", st.transitioning ? " (fading)" : "");
    printf("volume:    step %u of %u\n", (unsigned)st.volume_step, (unsigned)st.volume_max);
    printf("amplifier: %s", st.amp_powered ? "on" : "off");
    if (st.amp_i2c_addr != 0) {
        printf(", I2C 0x%02X", st.amp_i2c_addr);
    }
    printf(", %lu faults since boot\n", (unsigned long)st.amp_faults);
    printf("mic:       %lu double claps since boot", (unsigned long)st.claps);
    if (st.claps > 0) {
        printf(", last at %lu s", (unsigned long)(st.last_clap_ms / 1000u));
    }
    printf("\n");
    printf("wifi:      %s %s\n", st.wifi_connected ? "connected" : "not connected", st.wifi_ip);
    printf("hue:       %s, last: %s\n", st.hue_state[0] ? st.hue_state : "-", st.hue_last[0] ? st.hue_last : "-");
    printf("music:     %s, %u tracks, %s %s\n", st.sd_mounted ? "card mounted" : "no card", (unsigned)st.tracks,
           st.playing ? "playing" : "stopped", st.track);
}

static void cmd_hue(const char *arg)
{
    if (strcmp(arg, "pair") == 0) {
        printf("pairing for 30 s: press the link button on the bridge now\n");
        hue_task_request_pairing();
    } else if (strcmp(arg, "scenes") == 0) {
        hue_task_request_scene_list();
    } else if (strcmp(arg, "normal") == 0 || strcmp(arg, "ambient") == 0) {
        settings_t s;
        app_settings_snapshot(&s, NULL);
        const hue_command_t cmd = {
            .scene = (arg[0] == 'a') ? HUE_SCENE_AMBIENT : HUE_SCENE_NORMAL,
            .duration_ms = s.transition_ms,
        };
        hue_task_submit(&cmd);
    } else {
        printf("usage: hue pair | hue scenes | hue normal | hue ambient\n");
    }
}

/* Splits off the next word: returns its start and NUL-terminates it; *rest
 * points just past the single separating space (or at the end). */
static char *next_word(char *s, char **rest)
{
    while (*s == ' ') {
        s++;
    }
    char *end = s;
    while (*end != '\0' && *end != ' ') {
        end++;
    }
    *rest = (*end == ' ') ? end + 1 : end;
    *end = '\0';
    return s;
}


/* ------------------------------------------------------------------- io --- */
/* Diagnostics: what the chip really sees on the input pins, and direct control
 * of the outputs (bypasses the mode controller until the next transition). */

static void io_pin(const char *name, int pin)
{
    const uint32_t mux = REG_GET_FIELD(GPIO_PIN_MUX_REG[pin], MCU_SEL);
    const uint32_t oe = (pin < 32) ? ((REG_READ(GPIO_ENABLE_REG) >> pin) & 1u)
                                   : ((REG_READ(GPIO_ENABLE1_REG) >> (pin - 32)) & 1u);
    printf("  %-9s GPIO %2d  level %d  mux %lu%s  %s\n", name, pin, gpio_get_level(pin), (unsigned long)mux,
           mux == PIN_FUNC_GPIO ? " (GPIO)" : " (NOT GPIO)", oe ? "output ENABLED" : "input");
}

static void io_snapshot(void)
{
    io_pin("button", PIN_BUTTON);
#ifdef PIN_ENC_A
    io_pin("enc A", PIN_ENC_A);
    io_pin("enc B", PIN_ENC_B);
#endif
    io_pin("candles", PIN_SW_CANDLES);
    io_pin("fountain", PIN_SW_FOUNTAIN);
    int count = 0;
    if (input_encoder_raw(&count)) {
        printf("  encoder count %d (4 per click)\n", count);
    } else {
        printf("  encoder: not running\n");
    }
}

static void io_watch(void)
{
    printf("io watch: 15 s, prints every change (press the button, turn the knob)\n");
    int last_b = -1, last_a = -1, last_bb = -1, last_c = 0x7fffffff;
    for (int i = 0; i < 15000 / 20; i++) {
        const int b = gpio_get_level(PIN_BUTTON);
#ifdef PIN_ENC_A
        const int a = gpio_get_level(PIN_ENC_A);
        const int bb = gpio_get_level(PIN_ENC_B);
#else
        const int a = 0, bb = 0;
#endif
        int c = 0;
        input_encoder_raw(&c);
        if (b != last_b || a != last_a || bb != last_bb || c != last_c) {
            printf("  %5d ms  button %d  A %d  B %d  count %d\n", i * 20, b, a, bb, c);
            fflush(stdout);
            last_b = b; last_a = a; last_bb = bb; last_c = c;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("io watch: done\n");
}

static void cmd_io(char *rest)
{
    char *what = next_word(rest, &rest);
    char *val = next_word(rest, &rest);
    if (*what == '\0') {
        io_snapshot();
    } else if (strcmp(what, "watch") == 0) {
        io_watch();
    } else if ((strcmp(what, "candles") == 0 || strcmp(what, "fountain") == 0) &&
               (strcmp(val, "on") == 0 || strcmp(val, "off") == 0)) {
        const int pin = (what[0] == 'c') ? PIN_SW_CANDLES : PIN_SW_FOUNTAIN;
        gpio_set_level(pin, strcmp(val, "on") == 0);
        printf("%s %s (GPIO %d; test only, the next mode change overrides it)\n", what, val, pin);
    } else {
        printf("usage: io | io watch | io candles on|off | io fountain on|off\n");
    }
}


/* ------------------------------------------------------------------ amp --- */
/* Diagnostics: the TAS5825M's state as the chip reports it (book 0, page 0). */

static void amp_reg(const char *name, uint8_t reg, const char *hint)
{
    uint8_t v = 0;
    if (amp_io_read(reg, &v) == 0) {
        printf("  0x%02X %-14s 0x%02X  %s\n", reg, name, v, hint);
    } else {
        printf("  0x%02X %-14s (read failed)\n", reg, name);
    }
}

static void cmd_amp(char *rest)
{
    char *what = next_word(rest, &rest);
    if (amp_io_address() == 0) {
        printf("amplifier not found yet: enter Ambient once (button), then try again\n");
        return;
    }
    if (strcmp(what, "play") == 0) {
        /* Hi-Z, then play: what the driver does at power-up */
        amp_io_write(0x00, 0);
        amp_io_write(0x7F, 0);
        amp_io_write(0x03, 0x02);
        vTaskDelay(pdMS_TO_TICKS(10));
        amp_io_write(0x03, 0x03);
        amp_io_write(0x78, 0x80);
        printf("amp: Hi-Z -> play, faults cleared\n");
    } else if (strcmp(what, "tone") == 0) {
        char *val = next_word(rest, &rest);
        const bool on = (strcmp(val, "off") != 0);
        player_test_tone(on);
        amp_io_write(0x00, 0);
        amp_io_write(0x7F, 0);
        amp_io_write(0x50, on ? 0x00 : 0x07); /* auto mute off while testing */
        printf("test tone %s (1 kHz on L and R while music plays; auto mute %s)\n", on ? "ON" : "off",
               on ? "off" : "back on");
        return;
    } else if (*what != '\0') {
        printf("usage: amp | amp play | amp tone on|off\n");
        return;
    }
    amp_io_write(0x00, 0);   /* page 0 */
    amp_io_write(0x7F, 0);   /* book 0 */
    printf("amplifier at I2C 0x%02X:\n", amp_io_address());
    amp_reg("DEVICE_CTRL1", 0x02, "bit2 = PBTL");
    amp_reg("DEVICE_CTRL2", 0x03, "bits1:0 3 = play, 2 = Hi-Z; bit3 = mute");
    amp_reg("SAP_CTRL1", 0x33, "audio format / word length");
    amp_reg("FS_MON", 0x37, "detected sample rate code (0 = none)");
    amp_reg("BCK_MON", 0x38, "detected BCLK/FS ratio");
    amp_reg("CLKDET_STATUS", 0x39, "clock problems, 0 = ok");
    amp_reg("DIG_VOL", 0x4C, "0x30 = 0 dB, 0xFF = mute");
    amp_reg("AGAIN", 0x54, "analog gain, 0 = full");
    amp_reg("AUTO_MUTE", 0x50, "auto mute settings");
    amp_reg("POWER_STATE", 0x68, "0 deep sleep, 1 sleep, 2 Hi-Z, 3 play");
    amp_reg("CHAN_FAULT", 0x70, "0 = ok");
    amp_reg("GLOBAL_FAULT1", 0x71, "bit2 = clock fault");
    amp_reg("GLOBAL_FAULT2", 0x72, "0 = ok");
    amp_reg("WARNING", 0x73, "0 = ok");
}

static void run_line(char *line)
{
    char *rest = NULL;
    char *cmd = next_word(line, &rest);
    if (*cmd == '\0') {
        return;
    }
    if (strcmp(cmd, "set") == 0) {
        char *value = NULL;
        char *key = next_word(rest, &value);
        if (*key == '\0') {
            printf("usage: set <key> <value>\n");
        } else {
            cmd_set(key, value); /* value kept verbatim */
        }
    } else if (strcmp(cmd, "get") == 0) {
        char *key = next_word(rest, &rest);
        if (*key == '\0') {
            printf("usage: get <key>\n");
        } else {
            print_value(key);
        }
    } else if (strcmp(cmd, "show") == 0) {
        cmd_show();
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status();
    } else if (strcmp(cmd, "hue") == 0) {
        char *arg = next_word(rest, &rest);
        cmd_hue(arg);
    } else if (strcmp(cmd, "next") == 0) {
        player_next();
    } else if (strcmp(cmd, "rescan") == 0) {
        player_rescan();
    } else if (strcmp(cmd, "reboot") == 0) {
        printf("rebooting\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    } else if (strcmp(cmd, "cold") == 0) {
        char *arg = next_word(rest, &rest);
        const int code = strcmp(arg, "start") == 0 ? REMOTE_COLD_START
                       : strcmp(arg, "next") == 0  ? REMOTE_COLD_NEXT
                       : strcmp(arg, "abort") == 0 ? REMOTE_COLD_ABORT
                                                   : 0;
        if (code != 0) {
            events_post(EVT_REMOTE, code);
        } else {
            printf("usage: cold start | cold next | cold abort\n");
        }
    } else if (strcmp(cmd, "voice") == 0) {
        char *arg = next_word(rest, &rest);
        player_voice_cue((size_t)(arg[0] >= '0' && arg[0] <= '3' ? arg[0] - '0' : 0), false);
    } else if (strcmp(cmd, "beep") == 0) {
        player_voice_cue(0, true);
    } else if (strcmp(cmd, "amp") == 0) {
        cmd_amp(rest);
    } else if (strcmp(cmd, "io") == 0) {
        cmd_io(rest);
    } else if (strcmp(cmd, "help") == 0) {
        cmd_help();
    } else {
        printf("unknown command: %s\n", cmd);
        cmd_help();
    }
}

/* One line editor per input port. */
typedef struct {
    char line[CONSOLE_LINE_MAX];
    size_t len;
    bool too_long;
    bool after_cr;
} editor_t;

static void editor_feed(editor_t *ed, uint8_t c)
{
    if (c == '\n' && ed->after_cr) {
        ed->after_cr = false; /* CR LF counts as one line end */
        return;
    }
    ed->after_cr = (c == '\r');

    if (c == '\r' || c == '\n') {
        printf("\n");
        if (ed->too_long) {
            printf("line too long (max %u characters)\n", (unsigned)(CONSOLE_LINE_MAX - 1));
        } else {
            ed->line[ed->len] = '\0';
            xSemaphoreTake(s_run_lock, portMAX_DELAY);
            run_line(ed->line);
            xSemaphoreGive(s_run_lock);
        }
        ed->len = 0;
        ed->too_long = false;
    } else if (c == 0x08 || c == 0x7F) {
        if (ed->len > 0) {
            ed->len--;
            printf("\b \b");
        }
    } else if (c >= 0x20) {
        if (ed->len < CONSOLE_LINE_MAX - 1) {
            ed->line[ed->len++] = (char)c;
            putchar(c);
        } else {
            ed->too_long = true;
        }
    }
    fflush(stdout);
}

#if HAVE_UART_CONSOLE
static void uart_reader(void *arg)
{
    (void)arg;
    static editor_t ed;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) == 1) {
            editor_feed(&ed, c);
        }
    }
}
#endif

#if HAVE_USB_CONSOLE
static void usb_reader(void *arg)
{
    (void)arg;
    static editor_t ed;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) == 1) {
            editor_feed(&ed, c);
        }
    }
}
#endif

void console_task_start(void)
{
    s_run_lock = xSemaphoreCreateMutex();
    if (s_run_lock == NULL) {
        return;
    }
    bool any = false;
#if HAVE_UART_CONSOLE
    /* RX through the driver; output keeps going through stdout like the log. */
    esp_err_t err = uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        xTaskCreatePinnedToCore(uart_reader, "console", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIO, NULL,
                                CONSOLE_TASK_CORE);
        any = true;
    } else {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
    }
#endif
#if HAVE_USB_CONSOLE
    /* The board's native USB port (the only one on some boards): typing works
     * there too. Output already goes to both ports. */
    usb_serial_jtag_driver_config_t usj = {.rx_buffer_size = 256, .tx_buffer_size = 1024};
    if (usb_serial_jtag_driver_install(&usj) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
        xTaskCreatePinnedToCore(usb_reader, "console_usb", CONSOLE_TASK_STACK, NULL, CONSOLE_TASK_PRIO, NULL,
                                CONSOLE_TASK_CORE);
        any = true;
    } else {
        ESP_LOGE(TAG, "USB console input not available");
    }
#endif
    if (any) {
        cmd_help();
    }
}
