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
#include "hue_task.h"
#include "player_task.h"
#include "wifi.h"

#if CONFIG_ESP_CONSOLE_UART
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
           "hue:       hue pair | hue scenes | hue normal | hue ambient\n");
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
