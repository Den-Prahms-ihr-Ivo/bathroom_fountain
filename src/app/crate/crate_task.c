/*
 * crate_task – the holy crate's control loop.
 *
 * Every 20 ms: read the lid, feed crate_controller, follow its outputs.
 *   light   light_ramp (gamma) -> LEDC duty on PIN_LED_PWM (IRL540N gate)
 *   volume  a second light_ramp with gamma 1 carries the controller's volume
 *           fade (0 ... 1000 per mille); fade_curve turns it into an even dB
 *           fade; times the encoder volume (lib/volume) -> TAS5825M code
 *   amp     power up while outputs.amp_on, down otherwise (2 s retry backoff)
 *   music   MUSIC_START / MUSIC_STOP -> player_play()
 *   blink   BLINK -> short off/on pattern on the strip
 *   sleep   outputs.sleep_ok -> amplifier PDN held low, ext0 wake on the lid,
 *           deep sleep. A wake-up is a fresh boot.
 * Every 2 s: battery voltage from the 1 MOhm / 220 kOhm divider.
 * Encoder: turn = volume (saved in NVS), press = next track; held 3 s with
 * the lid open while the server offers pairing = confirm (strip pulses).
 * Wi-Fi / MQTT run while awake: a music sync keeps the closed crate awake
 * (crate_controller_busy, at most busy_max_ms); right before deep sleep a
 * retained {"online":false,"asleep":true,...} status replaces the last will.
 */
#include "crate_task.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "soc/soc_caps.h"

#include "amp_io.h"
#include "amp_volume.h"
#include "board_pins.h"
#include "crate_controller.h"
#include "enroll_task.h"
#include "events.h"
#include "fade_curve.h"
#include "light_ramp.h"
#include "mqtt_link.h"
#include "music_sync.h"
#include "player_task.h"
#include "server_link.h"
#include "status_json.h"
#include "tas5825m.h"
#include "volume.h"

#define CRATE_PERIOD_MS   20u
#define CRATE_TASK_STACK  4096u
#define CRATE_TASK_PRIO   5u
#define CRATE_TASK_CORE   1

#define BATTERY_PERIOD_MS 2000u
#define BATTERY_SAMPLES   8
#define DIV_TOP_KOHM      1000u   /* 1 MOhm from the battery  */
#define DIV_BOTTOM_KOHM   220u    /* 220 kOhm to GND          */

#define AMP_RETRY_MS      2000u
#define FAULT_POLL_MS     2000u

#define LED_BITS          LEDC_TIMER_14_BIT
#define LED_MAX_DUTY      16383u
#define LED_FREQ_HZ       1000u
#define LED_MODE          LEDC_LOW_SPEED_MODE
#define LED_TIMER         LEDC_TIMER_0
#define LED_CHANNEL       LEDC_CHANNEL_0

#define BLINK_PHASE_MS    150u
#define BLINK_PHASES      5u      /* off, on, off, on, off */
#define BLINK_LEVEL       400u    /* per mille while "on"  */
#define PAIR_PULSE_MS     600u    /* strip pulse while a pairing window is open */
#define ASLEEP_WAIT_MS    1500u   /* broker ack for the asleep status */

#define STATE_NVS_NAMESPACE "state"
#define VOLUME_NVS_KEY      "volume"

static const char *TAG = "crate";

typedef struct {
    crate_controller_t *cc;
    light_ramp_config_t light_cfg;
    light_ramp_t *light;
    light_ramp_t *fade;           /* volume fade, level = per mille */
    volume_t *vol;
    tas5825m_t *amp;

    nvs_handle_t nvs;
    bool nvs_open;

    adc_oneshot_unit_handle_t adc;
    adc_channel_t adc_chan;
    adc_cali_handle_t cali;
    bool cali_ok;
    uint32_t next_battery;
    uint16_t last_mv;

    uint16_t light_target, light_ms;
    uint16_t vol_target, vol_ms;
    bool have_targets;

    uint32_t blink_start;
    bool blinking;

    uint16_t last_duty;
    int last_code;                /* -1: not set since power-up */
    uint32_t amp_retry_at;
    bool amp_backoff;
    uint32_t next_fault_poll;
} crate_t;

static crate_t s_crate;

/* ------------------------------------------------------------- hardware -- */

static void lid_init(void)
{
    /* After an ext0 wake-up GPIO1 still belongs to the RTC domain. */
    rtc_gpio_deinit((gpio_num_t)PIN_LID);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_LID,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

static bool lid_open_raw(void)
{
    return gpio_get_level((gpio_num_t)PIN_LID) != 0; /* high = open */
}

static void led_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LED_MODE,
        .duty_resolution = LED_BITS,
        .timer_num = LED_TIMER,
        .freq_hz = LED_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const ledc_channel_config_t ch = {
        .gpio_num = PIN_LED_PWM,
        .speed_mode = LED_MODE,
        .channel = LED_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LED_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    gpio_set_drive_capability((gpio_num_t)PIN_LED_PWM, GPIO_DRIVE_CAP_3);
    s_crate.last_duty = 0;
}

static void led_set(uint16_t duty)
{
    if (duty == s_crate.last_duty) {
        return;
    }
    s_crate.last_duty = duty;
    ledc_set_duty(LED_MODE, LED_CHANNEL, duty);
    ledc_update_duty(LED_MODE, LED_CHANNEL);
}

static void battery_init(void)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(PIN_VBAT_ADC, &unit, &s_crate.adc_chan));
    const adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_crate.adc));
    const adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_crate.adc, s_crate.adc_chan, &ccfg));
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    const adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit,
        .chan = s_crate.adc_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_crate.cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_crate.cali) == ESP_OK);
#endif
    if (!s_crate.cali_ok) {
        ESP_LOGW(TAG, "no ADC calibration: battery voltage is approximate");
    }
}

/* Battery voltage in mV, 0 if the ADC failed. */
static uint16_t battery_mv(void)
{
    int32_t sum = 0;
    int n = 0;
    for (int i = 0; i < BATTERY_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_crate.adc, s_crate.adc_chan, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s_crate.cali_ok) {
            if (adc_cali_raw_to_voltage(s_crate.cali, raw, &mv) != ESP_OK) {
                continue;
            }
        } else {
            mv = raw * 3100 / 4095; /* rough, 12 dB attenuation */
        }
        sum += mv;
        n++;
    }
    if (n == 0) {
        return 0;
    }
    const uint32_t pin_mv = (uint32_t)(sum / n);
    const uint32_t bat = pin_mv * (DIV_TOP_KOHM + DIV_BOTTOM_KOHM) / DIV_BOTTOM_KOHM;
    return (uint16_t)(bat > 65535u ? 65535u : bat);
}

/* ------------------------------------------------------------ amplifier -- */

static bool amp_may_act(uint32_t now)
{
    return !s_crate.amp_backoff || (int32_t)(now - s_crate.amp_retry_at) >= 0;
}

static void amp_result(tas5825m_err_t err, const char *what, uint32_t now)
{
    if (err == TAS5825M_OK) {
        s_crate.amp_backoff = false;
        return;
    }
    ESP_LOGW(TAG, "amplifier %s failed (%d), retry in %u ms", what, (int)err, (unsigned)AMP_RETRY_MS);
    s_crate.amp_backoff = true;
    s_crate.amp_retry_at = now + AMP_RETRY_MS;
}

static void amp_follow(const crate_outputs_t *o, uint32_t now)
{
    tas5825m_t *amp = s_crate.amp;
    if (!amp) {
        return;
    }
    const bool on = tas5825m_is_powered(amp);
    if (o->amp_on && !on && amp_may_act(now)) {
        const tas5825m_err_t err = tas5825m_power_up(amp);
        amp_result(err, "power-up", now);
        s_crate.last_code = -1;
        s_crate.next_fault_poll = now + FAULT_POLL_MS;
        if (err == TAS5825M_OK) {
            ESP_LOGI(TAG, "amplifier on");
        }
    } else if (!o->amp_on && on) {
        amp_result(tas5825m_power_down(amp), "power-down", now);
        ESP_LOGI(TAG, "amplifier off");
        return;
    }
    if (!tas5825m_is_powered(amp)) {
        return;
    }

    const float progress = (float)light_ramp_level(s_crate.fade, now) / 1000.0f;
    const float fade_gain = fade_curve_gain(progress, FADE_CURVE_FLOOR_DB_DEFAULT);
    const uint8_t code = amp_volume_code_for(volume_gain(s_crate.vol), fade_gain);
    if ((int)code != s_crate.last_code && amp_may_act(now)) {
        const tas5825m_err_t err = tas5825m_set_volume(amp, code);
        amp_result(err, "volume", now);
        if (err == TAS5825M_OK) {
            s_crate.last_code = code;
        }
    }

    if ((int32_t)(now - s_crate.next_fault_poll) >= 0) {
        s_crate.next_fault_poll = now + FAULT_POLL_MS;
        tas5825m_faults_t f;
        if (tas5825m_read_faults(amp, &f) == TAS5825M_OK && tas5825m_faults_any(&f)) {
            ESP_LOGW(TAG, "amplifier fault: chan 0x%02X global1 0x%02X global2 0x%02X, clearing",
                     f.chan, f.global1, f.global2);
            tas5825m_clear_faults(amp);
        }
    }
}

/* ---------------------------------------------------------------- sleep -- */

static void save_volume(uint32_t now, bool force)
{
    uint8_t step;
    /* force: pretend the rest time is over (just before deep sleep). */
    const uint32_t t = force ? now + 3600000u : now;
    if (!volume_take_save(s_crate.vol, t, &step) || !s_crate.nvs_open) {
        return;
    }
    esp_err_t err = nvs_set_u8(s_crate.nvs, VOLUME_NVS_KEY, step);
    if (err == ESP_OK) {
        err = nvs_commit(s_crate.nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "volume not saved: %s", esp_err_to_name(err));
    }
}

/* Retained status instead of the last will: the web app shows "asleep"
 * with the battery, not "offline". Best effort: no broker, no wait. */
static void publish_asleep(void)
{
    if (!mqtt_link_connected()) {
        return;
    }
    char json[128];
    const status_battery_t b = (status_battery_t)crate_controller_outputs(s_crate.cc)->battery; /* same order */
    if (status_json_crate_asleep(s_crate.last_mv, b, json, sizeof(json)) > 0) {
        if (!mqtt_link_publish_and_wait(MQTT_TOPIC_STATUS, json, ASLEEP_WAIT_MS)) {
            ESP_LOGW(TAG, "asleep status not confirmed");
        }
    }
    mqtt_link_stop(); /* clean DISCONNECT: the broker drops the will */
}

static void go_to_sleep(uint32_t now)
{
    save_volume(now, true);
    publish_asleep();
    player_play(false);
    if (s_crate.amp && tas5825m_is_powered(s_crate.amp)) {
        tas5825m_power_down(s_crate.amp);
    }
    led_set(0);
    ledc_stop(LED_MODE, LED_CHANNEL, 0);

    /* Amplifier PDN stays low while the chip sleeps. */
    gpio_set_level((gpio_num_t)PIN_AMP_PWDN, 0);
    gpio_hold_en((gpio_num_t)PIN_AMP_PWDN);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif

    /* Wake on the lid's other level: normally "opens"; with an empty battery
     * and the lid open, "closes" (crate_controller.h, Sleep). */
    const bool open = lid_open_raw();
    const int wake_level = open ? 0 : 1;
    rtc_gpio_init((gpio_num_t)PIN_LID);
    rtc_gpio_set_direction((gpio_num_t)PIN_LID, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en((gpio_num_t)PIN_LID);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_LID);
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_LID, wake_level));

    ESP_LOGI(TAG, "deep sleep, wake when the lid %s", open ? "closes" : "opens");
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the log drain */
    esp_deep_sleep_start();
}

/* ----------------------------------------------------------------- loop -- */

static void handle_inputs(uint32_t now)
{
    event_t ev;
    while (events_receive(&ev, 0)) {
        switch (ev.type) {
        case EVT_VOLUME:
            volume_turn(s_crate.vol, ev.detents, now);
            ESP_LOGI(TAG, "volume step %u", (unsigned)volume_step(s_crate.vol));
            break;
        case EVT_LONG_PRESS:
            /* lid open and the server offers pairing: confirm; else a press */
            if (crate_controller_outputs(s_crate.cc)->lid_open && enroll_task_button_held()) {
                break;
            }
            /* fall through */
        case EVT_BUTTON:
            if (crate_controller_outputs(s_crate.cc)->music_playing) {
                player_next();
            }
            break;
        default:
            break;
        }
    }
}

static void handle_events(uint32_t now)
{
    crate_event_t e;
    while (crate_controller_take_event(s_crate.cc, &e)) {
        switch (e) {
        case CRATE_EV_MUSIC_START:
            ESP_LOGI(TAG, "music start");
            player_play(true);
            break;
        case CRATE_EV_MUSIC_STOP:
            ESP_LOGI(TAG, "music stop");
            player_play(false);
            break;
        case CRATE_EV_BLINK:
            ESP_LOGW(TAG, "battery empty: blink");
            s_crate.blinking = true;
            s_crate.blink_start = now;
            break;
        }
    }
}

static void follow_targets(const crate_outputs_t *o, uint32_t now)
{
    if (!s_crate.have_targets || o->light_permille != s_crate.light_target || o->light_fade_ms != s_crate.light_ms) {
        light_ramp_set(s_crate.light, o->light_permille, o->light_fade_ms, now);
        s_crate.light_target = o->light_permille;
        s_crate.light_ms = o->light_fade_ms;
    }
    if (!s_crate.have_targets || o->volume_permille != s_crate.vol_target || o->volume_fade_ms != s_crate.vol_ms) {
        light_ramp_set(s_crate.fade, o->volume_permille, o->volume_fade_ms, now);
        s_crate.vol_target = o->volume_permille;
        s_crate.vol_ms = o->volume_fade_ms;
    }
    s_crate.have_targets = true;
}

static void update_led(uint32_t now)
{
    uint16_t duty = light_ramp_now(s_crate.light, now);
    if (s_crate.blinking) {
        const uint32_t phase = (now - s_crate.blink_start) / BLINK_PHASE_MS;
        if (phase >= BLINK_PHASES) {
            s_crate.blinking = false;
        } else {
            duty = (phase % 2u) ? light_ramp_duty(&s_crate.light_cfg, BLINK_LEVEL) : 0;
        }
    }
    if (!s_crate.blinking && enroll_task_blink()) {
        /* pairing window: pulse between dark and the current level */
        const uint32_t ph = now % (2u * PAIR_PULSE_MS);
        const uint32_t tri = ph < PAIR_PULSE_MS ? ph : 2u * PAIR_PULSE_MS - ph;
        duty = (uint16_t)((uint32_t)duty * tri / PAIR_PULSE_MS);
    }
    led_set(duty);
}

/* ------------------------------------------------------------- status --- */

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static status_crate_t s_status;

static void update_status(const crate_outputs_t *o)
{
    status_crate_t st = {
        .state = (status_crate_state_t)o->state, /* same order */
        .light = o->light_permille,
        .playing = o->music_playing,
        .volume = volume_step(s_crate.vol),
        .battery_mv = s_crate.last_mv,
        .battery = (status_battery_t)o->battery,  /* same order */
    };
    portENTER_CRITICAL(&s_status_lock);
    const bool changed = st.state != s_status.state || st.playing != s_status.playing ||
                         st.volume != s_status.volume || st.battery != s_status.battery;
    s_status = st;
    portEXIT_CRITICAL(&s_status_lock);
    if (changed) {
        server_link_status_changed();
    }
}

void crate_task_status(status_crate_t *out)
{
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

static void crate_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(CRATE_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last_wake, period);
        const uint32_t now = app_now_ms();

        handle_inputs(now);
        crate_controller_lid(s_crate.cc, lid_open_raw(), now);
        if ((int32_t)(now - s_crate.next_battery) >= 0) {
            s_crate.next_battery = now + BATTERY_PERIOD_MS;
            const uint16_t mv = battery_mv();
            if (mv > 0) {
                s_crate.last_mv = mv;
                crate_controller_battery(s_crate.cc, mv, now);
            }
        }
        crate_controller_busy(s_crate.cc, music_sync_running(), now); /* later also: an OTA running */
        crate_controller_tick(s_crate.cc, now);
        handle_events(now);

        const crate_outputs_t *o = crate_controller_outputs(s_crate.cc);
        follow_targets(o, now);
        update_led(now);
        amp_follow(o, now);
        save_volume(now, false);
        update_status(o);

        if (o->sleep_ok && !s_crate.blinking) {
            go_to_sleep(now);
        }
    }
}

void crate_task_start(void)
{
    /* Release the PDN hold left from the last deep sleep before amp_io
     * reconfigures the pin. */
    gpio_hold_dis((gpio_num_t)PIN_AMP_PWDN);
#if !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif

    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "boot (%s)", cause == ESP_SLEEP_WAKEUP_EXT0 ? "lid woke me" : "power-on or reset");

    lid_init();
    led_init();
    battery_init();
    s_crate.amp = tas5825m_create(amp_io_get());
    if (!s_crate.amp) {
        ESP_LOGE(TAG, "amplifier driver not created: no sound");
    }
    s_crate.last_code = -1;

    light_ramp_config_default(&s_crate.light_cfg);
    s_crate.light_cfg.max_duty = LED_MAX_DUTY;
    light_ramp_config_t fade_cfg;
    light_ramp_config_default(&fade_cfg);
    fade_cfg.gamma = 1.0f; /* we read the level, not the duty */

    const uint32_t now = app_now_ms();
    s_crate.light = light_ramp_create(&s_crate.light_cfg, now);
    s_crate.fade = light_ramp_create(&fade_cfg, now);

    volume_config_t vol_cfg;
    volume_config_default(&vol_cfg);
    s_crate.nvs_open = (nvs_open(STATE_NVS_NAMESPACE, NVS_READWRITE, &s_crate.nvs) == ESP_OK);
    uint8_t saved = 0;
    const bool has_saved = s_crate.nvs_open && nvs_get_u8(s_crate.nvs, VOLUME_NVS_KEY, &saved) == ESP_OK;
    s_crate.vol = volume_create(&vol_cfg, has_saved, saved);

    crate_config_t cfg;
    crate_config_default(&cfg);
    cfg.volume = 1000; /* the encoder sets the loudness; the controller only fades */
    vTaskDelay(pdMS_TO_TICKS(5)); /* pull-up settles */
    const bool open = lid_open_raw();
    s_crate.cc = crate_controller_create(&cfg, open, now);

    if (!s_crate.light || !s_crate.fade || !s_crate.vol || !s_crate.cc) {
        ESP_LOGE(TAG, "out of memory or bad config: crate not started");
        return;
    }
    s_crate.next_battery = now; /* first sample at once */
    ESP_LOGI(TAG, "lid %s, volume step %u", open ? "open" : "closed", (unsigned)volume_step(s_crate.vol));

    if (xTaskCreatePinnedToCore(crate_task, "crate", CRATE_TASK_STACK, NULL, CRATE_TASK_PRIO, NULL,
                                CRATE_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "crate task not created");
    }
}
