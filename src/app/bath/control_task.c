/*
 * control_task – see control_task.h.
 */
#include "control_task.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "amp_io.h"
#include "amp_volume.h"
#include "app_settings.h"
#include "app_status.h"
#include "bath_volume.h"
#include "board_pins.h"
#include "enroll_task.h"
#include "events.h"
#include "hue_task.h"
#include "mode_controller.h"
#include "mode_volume.h"
#include "outputs.h"
#include "player_task.h"
#include "tas5825m.h"
#include "volume.h"

#define CONTROL_PERIOD_MS  20u
#define CONTROL_TASK_STACK 4096u
#define CONTROL_TASK_PRIO  5u
#define CONTROL_TASK_CORE  1

#define AMP_RETRY_MS        2000u  /* after a failed amplifier action */
#define FAULT_POLL_MS       2000u  /* while the amplifier is on */

#define STATE_NVS_NAMESPACE "state"
/* volume keys: bath_volume_key(slot) ("volume" = Ambient, as in v1; "vol_cold") */

static const char *TAG = "control";

typedef struct {
    mode_controller_t *mc;
    mode_volume_t *vol;      /* BATH_VOL_COUNT slots, lib/bath_volume */
    outputs_t *outs;
    tas5825m_t *amp;
    nvs_handle_t nvs;
    bool nvs_open;
    uint32_t amp_retry_at;   /* no amplifier action before this time      */
    bool amp_backoff;
    uint32_t next_fault_poll;
} control_t;

static control_t s_ctl;

/* ---------------------------------------------------------------- setup --- */

static void outputs_gpio_init(void)
{
    uint64_t mask = (1ULL << PIN_SW_CANDLES) | (1ULL << PIN_SW_FOUNTAIN);
#ifdef PIN_LED_MUSIC
    mask |= 1ULL << PIN_LED_MUSIC;
#endif
    const gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT_OUTPUT, /* readable for the console's "io" command */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(PIN_SW_CANDLES, 0);
    gpio_set_level(PIN_SW_FOUNTAIN, 0);
#ifdef PIN_LED_MUSIC
    gpio_set_level(PIN_LED_MUSIC, 0);
#endif
}

static bool control_init(void)
{
    const uint32_t now = app_now_ms();
    settings_t s;
    app_settings_snapshot(&s, NULL);

    mode_config_t mode_cfg;
    mode_config_default(&mode_cfg);
    mode_cfg.transition_ms = s.transition_ms;
    // mode_cfg.max_ambient_ms = 60000; /* live test of BEH-10: 1 min instead of 30 */

    volume_config_t vol_cfg;
    volume_config_default(&vol_cfg);
    vol_cfg.max_step = s.volume_max;
    if (vol_cfg.default_step > vol_cfg.max_step) {
        vol_cfg.default_step = vol_cfg.max_step;
    }

    s_ctl.nvs_open = (nvs_open(STATE_NVS_NAMESPACE, NVS_READWRITE, &s_ctl.nvs) == ESP_OK);
    uint8_t saved[BATH_VOL_COUNT] = {0};
    bool has_saved[BATH_VOL_COUNT] = {false};
    for (int i = 0; i < BATH_VOL_COUNT; i++) {
        has_saved[i] = s_ctl.nvs_open && nvs_get_u8(s_ctl.nvs, bath_volume_key(i), &saved[i]) == ESP_OK;
    }

    outputs_gpio_init();
    s_ctl.mc = mode_controller_create(&mode_cfg, now);
    s_ctl.vol = mode_volume_create(&vol_cfg, BATH_VOL_COUNT, has_saved, saved);
    s_ctl.outs = outputs_create();
    s_ctl.amp = tas5825m_create(amp_io_get());
    if (s_ctl.mc == NULL || s_ctl.vol == NULL || s_ctl.outs == NULL || s_ctl.amp == NULL) {
        ESP_LOGE(TAG, "init failed");
        return false;
    }
    app_status_t *st = app_status_lock();
    st->volume_max = vol_cfg.max_step;
    app_status_unlock();
    ESP_LOGI(TAG, "transition %lu ms, auto exit after %lu s, volume steps ambient %u%s / cold %u%s of %u",
             (unsigned long)mode_cfg.transition_ms, (unsigned long)(mode_cfg.max_ambient_ms / 1000u),
             (unsigned)mode_volume_step(s_ctl.vol, BATH_VOL_AMBIENT), has_saved[BATH_VOL_AMBIENT] ? " (saved)" : "",
             (unsigned)mode_volume_step(s_ctl.vol, BATH_VOL_COLD), has_saved[BATH_VOL_COLD] ? " (saved)" : "",
             (unsigned)vol_cfg.max_step);
    return true;
}

/* --------------------------------------------------------------- events --- */

/* TODO(cold): true while cold_session / cold_protocol runs (any phase). The
 * cold shower is not wired into control_task yet, so the cold slot is never
 * picked; the encoder sets the Ambient volume as before. */
static bool cold_running(void)
{
    return false;
}

/* The slot the encoder sets now (MODE_VOLUME_NONE: quiet Normal). */
static int volume_slot(void)
{
    return bath_volume_mode(mode_controller_outputs(s_ctl.mc), cold_running());
}

/* The slot whose gain goes to the amplifier: in quiet Normal the music is off
 * anyway; the Ambient gain keeps the fade-in of the next Ambient right. */
static int gain_slot(void)
{
    const int slot = volume_slot();
    return slot == MODE_VOLUME_NONE ? BATH_VOL_AMBIENT : slot;
}

static void handle_event(const event_t *ev, uint32_t now)
{
    switch (ev->type) {
    case EVT_CLAP:
    case EVT_BUTTON: {
        const bool clap = (ev->type == EVT_CLAP);
        const bool ok = mode_controller_trigger(s_ctl.mc, clap ? TRIGGER_CLAP : TRIGGER_BUTTON, now);
        const bool ambient = (mode_controller_outputs(s_ctl.mc)->mode == AMBIENCE_AMBIENT);
        ESP_LOGI(TAG, "%s: %s", clap ? "clap" : "button",
                 !ok ? "ignored" : (ambient ? "entering Ambient" : "leaving Ambient"));
        break;
    }
    case EVT_VOLUME: {
        const int slot = volume_slot();
        if (mode_volume_turn(s_ctl.vol, slot, ev->detents, now)) {
            ESP_LOGI(TAG, "volume %s step %u", bath_volume_name(slot), (unsigned)mode_volume_step(s_ctl.vol, slot));
        } else {
            ESP_LOGI(TAG, "volume: nothing plays, ignored");
        }
        break;
    }
    case EVT_LONG_PRESS:
        /* 3 s: server pairing while a confirm window is open, else Hue pairing */
        if (!enroll_task_button_held()) {
            ESP_LOGI(TAG, "long press: Hue pairing (press the bridge's link button)");
            hue_task_request_pairing();
        }
        break;
    }
}

/* -------------------------------------------------------------- outputs --- */

static bool carry_out(const output_action_t *a)
{
    switch (a->kind) {
    case OUTPUT_CANDLES:
        ESP_LOGI(TAG, "candles %s", a->value ? "on" : "off");
        return gpio_set_level(PIN_SW_CANDLES, a->value) == ESP_OK;
    case OUTPUT_FOUNTAIN:
        ESP_LOGI(TAG, "fountain %s", a->value ? "on" : "off");
        return gpio_set_level(PIN_SW_FOUNTAIN, a->value) == ESP_OK;
    case OUTPUT_MUSIC:
        ESP_LOGI(TAG, "music %s", a->value ? "start" : "stop");
        player_play(a->value != 0);
#ifdef PIN_LED_MUSIC
        gpio_set_level(PIN_LED_MUSIC, a->value);
#endif
        return true;
    case OUTPUT_AMP_POWER: {
        ESP_LOGI(TAG, "amplifier power %s", a->value ? "up" : "down");
        const tas5825m_err_t r = a->value ? tas5825m_power_up(s_ctl.amp) : tas5825m_power_down(s_ctl.amp);
        if (r != TAS5825M_OK) {
            ESP_LOGW(TAG, "amplifier power %s failed (%d)", a->value ? "up" : "down", (int)r);
        }
        return r == TAS5825M_OK;
    }
    case OUTPUT_VOLUME: {
        if (a->value == AMP_VOL_MUTE) {
            ESP_LOGI(TAG, "volume 0x%02X (mute)", a->value);
        } else {
            ESP_LOGI(TAG, "volume 0x%02X (%.1f dB)", a->value, (double)amp_volume_db(a->value));
        }
        const tas5825m_err_t r = tas5825m_set_volume(s_ctl.amp, a->value);
        if (r != TAS5825M_OK) {
            ESP_LOGW(TAG, "volume write failed (%d)", (int)r);
        }
        return r == TAS5825M_OK;
    }
    }
    return false;
}

static bool is_amp_action(const output_action_t *a)
{
    return a->kind == OUTPUT_AMP_POWER || a->kind == OUTPUT_VOLUME;
}

static void apply_outputs(uint32_t now)
{
    outputs_state_t want;
    outputs_want_from_mode(mode_controller_outputs(s_ctl.mc), mode_volume_gain(s_ctl.vol, gain_slot()), &want);

    output_action_t actions[OUTPUTS_MAX_ACTIONS];
    const size_t n = outputs_plan(s_ctl.outs, &want, actions);
    for (size_t i = 0; i < n; i++) {
        if (is_amp_action(&actions[i]) && s_ctl.amp_backoff && (int32_t)(now - s_ctl.amp_retry_at) < 0) {
            break; /* the amplifier failed recently: try again later, not every 20 ms */
        }
        if (!carry_out(&actions[i])) {
            if (is_amp_action(&actions[i])) {
                s_ctl.amp_backoff = true;
                s_ctl.amp_retry_at = now + AMP_RETRY_MS;
            }
            break; /* the rest waits; outputs plans it again next cycle */
        }
        if (is_amp_action(&actions[i])) {
            s_ctl.amp_backoff = false;
        }
        outputs_done(s_ctl.outs, &actions[i]);
    }
}

/* Faults latch in the amplifier and switch its outputs off; clearing lets it
 * try again (TI: DC, over-current and over-temperature faults need this). */
static void poll_amp_faults(uint32_t now)
{
    if (!tas5825m_is_powered(s_ctl.amp) || (int32_t)(now - s_ctl.next_fault_poll) < 0) {
        return;
    }
    s_ctl.next_fault_poll = now + FAULT_POLL_MS;
    tas5825m_faults_t f;
    if (tas5825m_read_faults(s_ctl.amp, &f) != TAS5825M_OK) {
        return;
    }
    if (f.warning != 0) {
        ESP_LOGW(TAG, "amplifier temperature warning 0x%02X", f.warning);
    }
    if (tas5825m_faults_any(&f)) {
        ESP_LOGW(TAG, "amplifier fault: chan 0x%02X global1 0x%02X global2 0x%02X, clearing",
                 f.chan, f.global1, f.global2);
        tas5825m_clear_faults(s_ctl.amp);
        app_status_t *st = app_status_lock();
        st->amp_faults++;
        app_status_unlock();
    }
}

static void publish_status(void)
{
    const mode_outputs_t *o = mode_controller_outputs(s_ctl.mc);
    const uint8_t step = mode_volume_step(s_ctl.vol, gain_slot()); /* TODO(status_json): both volumes */
    const bool powered = tas5825m_is_powered(s_ctl.amp);
    const uint8_t addr = amp_io_address();
    app_status_t *st = app_status_lock();
    st->ambient = (o->mode == AMBIENCE_AMBIENT);
    st->transitioning = o->transitioning;
    st->volume_step = step;
    st->amp_powered = powered;
    st->amp_i2c_addr = addr;
    app_status_unlock();
}

/* ------------------------------------------------------- hue and saving --- */

static void forward_hue_command(void)
{
    hue_command_t cmd;
    if (mode_controller_take_hue_command(s_ctl.mc, &cmd)) {
        ESP_LOGI(TAG, "Hue: scene %s over %lu ms",
                 cmd.scene == HUE_SCENE_AMBIENT ? "AMBIENT" : "NORMAL", (unsigned long)cmd.duration_ms);
        hue_task_submit(&cmd);
    }
}

static void save_volume(uint32_t now)
{
    int slot;
    uint8_t step;
    while (mode_volume_take_save(s_ctl.vol, now, &slot, &step)) {
        esp_err_t err = s_ctl.nvs_open ? nvs_set_u8(s_ctl.nvs, bath_volume_key(slot), step) : ESP_ERR_NVS_INVALID_HANDLE;
        if (err == ESP_OK) {
            err = nvs_commit(s_ctl.nvs);
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "volume %s step %u saved", bath_volume_name(slot), (unsigned)step);
        } else {
            ESP_LOGW(TAG, "volume %s step %u not saved: %s", bath_volume_name(slot), (unsigned)step,
                     esp_err_to_name(err));
        }
    }
}

/* ----------------------------------------------------------------- task --- */

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        event_t ev;
        const bool got = events_receive(&ev, CONTROL_PERIOD_MS);
        const uint32_t now = app_now_ms();
        if (got) {
            handle_event(&ev, now);
        }
        mode_controller_tick(s_ctl.mc, now);
        apply_outputs(now);
        forward_hue_command();
        save_volume(now);
        poll_amp_faults(now);
        publish_status();
    }
}

void control_task_start(void)
{
    if (!control_init()) {
        return;
    }
    xTaskCreatePinnedToCore(control_task, "control", CONTROL_TASK_STACK, NULL, CONTROL_TASK_PRIO, NULL,
                            CONTROL_TASK_CORE);
}
