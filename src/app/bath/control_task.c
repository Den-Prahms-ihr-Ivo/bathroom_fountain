/*
 * control_task – see control_task.h.
 */
#include "control_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "amp_io.h"
#include "amp_volume.h"
#include "app_settings.h"
#include "app_status.h"
#include "bath_volume.h"
#include "board_pins.h"
#include "cfg_store.h"
#include "cold_protocol.h"
#include "cold_session.h"
#include "enroll_task.h"
#include "events.h"
#include "expander_io.h"
#include "hue_task.h"
#include "mode_controller.h"
#include "mode_volume.h"
#include "outputs.h"
#include "player_task.h"
#include "server_link.h"
#include "tas5825m.h"
#include "volume.h"

#define CONTROL_PERIOD_MS  20u
#define CONTROL_TASK_STACK 6144u
#define CONTROL_TASK_PRIO  5u
#define CONTROL_TASK_CORE  1

#define AMP_RETRY_MS        2000u  /* after a failed amplifier action */
#define FAULT_POLL_MS       2000u  /* while the amplifier is on */
#define COLD_SCENE_FADE_MS  2000u  /* Hue fade when a cold phase begins */

#define STATE_NVS_NAMESPACE "state"
/* volume keys: bath_volume_key(slot) ("volume" = Ambient, as in v1; "vol_cold") */

#define FOLDER_AMBIENT "A"
#define FOLDER_HYPE    "B"
#define FOLDER_CALM    "C"

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

    /* cold shower */
    cold_protocol_t *cp;
    cold_session_t *cs;
    cold_phase_t phase;      /* last phase seen (for cold_session / events) */
    uint8_t run_profile;     /* cfg_profile_t of the run */
    bool switch_hype;        /* last EVT_COLD_SWITCH */
    bool pending_exit;       /* run ended: back to Normal when a trigger is accepted */

    /* music */
    bool music_on;           /* outputs says music should play */
    bool player_on;          /* what was last asked of the player */
    char player_folder[PLAYER_FOLDER_MAX + 1];

    ambience_mode_t last_mode;
    bool by_us;              /* the last mode change came from a trigger here */
    const char *trigger_by;  /* "clap", "button", "server" */
} control_t;

static control_t s_ctl;
static cfg_bath_t s_bath;
static bool s_bath_loaded;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static control_status_t s_status;

/* Clip order = cold_cue_id_t (GO, WARNING, TO_COLD, DONE); set in main.c. */

/* ---------------------------------------------------------------- setup --- */

cfg_bath_t *control_task_bath(void)
{
    if (!s_bath_loaded) {
        cfg_store_load(&cfg_bath_table, &s_bath);
        s_bath_loaded = true;
    }
    return &s_bath;
}

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
    control_task_bath();

    mode_config_t mode_cfg;
    mode_config_default(&mode_cfg);
    mode_cfg.transition_ms = s.transition_ms;
    cfg_store_lock();
    mode_cfg.clap_exits = s_bath.clap_exits; /* takes effect after a reboot */
    cfg_store_unlock();
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

    cold_config_t cold_cfg;
    cold_config_default(&cold_cfg);

    outputs_gpio_init();
    s_ctl.mc = mode_controller_create(&mode_cfg, now);
    s_ctl.vol = mode_volume_create(&vol_cfg, BATH_VOL_COUNT, has_saved, saved);
    s_ctl.outs = outputs_create();
    s_ctl.amp = tas5825m_create(amp_io_get());
    s_ctl.cp = cold_protocol_create(&cold_cfg, now);
    s_ctl.cs = cold_session_create();
    if (s_ctl.mc == NULL || s_ctl.vol == NULL || s_ctl.outs == NULL || s_ctl.amp == NULL || s_ctl.cp == NULL ||
        s_ctl.cs == NULL) {
        ESP_LOGE(TAG, "init failed");
        return false;
    }
    s_ctl.last_mode = AMBIENCE_NORMAL;
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

/* --------------------------------------------------------- cold profile --- */

static bool cold_running(void)
{
    return cold_protocol_outputs(s_ctl.cp)->phase != COLD_IDLE;
}

/* The profile a run uses: the switch when cold_switch is on and the expander
 * answers, else the cold_profile setting. */
static uint8_t chosen_profile(void)
{
    cfg_store_lock();
    const bool use_switch = s_bath.cold_switch;
    const uint8_t setting = s_bath.cold_profile;
    cfg_store_unlock();
    if (use_switch && expander_io_present()) {
        return s_ctl.switch_hype ? CFG_PROFILE_HYPE : CFG_PROFILE_ZEN;
    }
    return setting < CFG_PROFILE_COUNT ? setting : CFG_PROFILE_HYPE;
}

static void profile_of(uint8_t which, cold_profile_t *out, cfg_cold_profile_t *full)
{
    cfg_store_lock();
    *full = s_bath.profile[which];
    cfg_store_unlock();
    out->warm_s = full->warm_s;
    out->cold_s = full->cold_s;
    out->after_s = full->after_s;
    out->voice = full->voice;
    out->music = full->music;
    out->calm = full->calm;
}

static const char *profile_name(uint8_t p)
{
    return p == CFG_PROFILE_ZEN ? "zen" : "hype";
}

static const char *phase_name(cold_phase_t p)
{
    switch (p) {
    case COLD_COUNTDOWN: return "countdown";
    case COLD_WARM: return "warm";
    case COLD_COLD: return "cold";
    case COLD_AFTER: return "after";
    case COLD_IDLE: break;
    }
    return "idle";
}

/* ---------------------------------------------------------------- events --- */

static void mode_event(const char *mode, const char *by)
{
    char json[128];
    if (status_json_event_mode(mode, by, -1, NULL, server_link_utc(), json, sizeof(json)) > 0) {
        server_link_event(json);
    }
    server_link_status_changed();
}

static void cold_event_phase(cold_phase_t p)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"type\":\"cold\",\"phase\":\"%s\",\"profile\":\"%s\"}", phase_name(p),
             profile_name(s_ctl.run_profile));
    server_link_event(json);
    server_link_status_changed();
}

static void cold_event_end(cold_end_t end)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"type\":\"cold\",\"end\":\"%s\"}", end == COLD_END_ABORTED ? "aborted" : "finished");
    server_link_event(json);
    server_link_status_changed();
}

/* --------------------------------------------------------- cold actions --- */

static bool trigger(trigger_source_t src, const char *by, uint32_t now)
{
    const bool ok = mode_controller_trigger(s_ctl.mc, src, now);
    if (ok) {
        s_ctl.by_us = true;
        s_ctl.trigger_by = by;
    }
    return ok;
}

static void cold_start(uint32_t now)
{
    if (cold_running()) {
        return;
    }
    const mode_outputs_t *o = mode_controller_outputs(s_ctl.mc);
    if (o->mode == AMBIENCE_NORMAL && !trigger(TRIGGER_BUTTON, "button", now)) {
        ESP_LOGI(TAG, "cold: ignored (mode change in progress)");
        return;
    }
    const uint8_t which = chosen_profile();
    cold_profile_t p;
    cfg_cold_profile_t full;
    profile_of(which, &p, &full);
    uint32_t clips[PLAYER_VOICE_MAX];
    player_voice_lengths(clips);
    if (!cold_protocol_start(s_ctl.cp, &p, clips, now)) {
        ESP_LOGW(TAG, "cold: profile %s refused (values out of range?)", profile_name(which));
        return;
    }
    s_ctl.run_profile = which;
    s_ctl.phase = COLD_IDLE;
    s_ctl.pending_exit = false;
    uint32_t id = esp_random();
    if (id == 0) {
        id = 1;
    }
    const int64_t utc = server_link_utc();
    cold_session_begin(s_ctl.cs, id, which == CFG_PROFILE_ZEN ? COLD_SESSION_ZEN : COLD_SESSION_HYPE, &p,
                       utc > 0 ? (uint32_t)utc : 0, now);
    expander_io_cold_led(true);
    ESP_LOGI(TAG, "cold: %s run started (warm %u s, cold %u s, after %u s)", profile_name(which), p.warm_s, p.cold_s,
             p.calm ? p.after_s : 0);
    mode_event("cold", s_ctl.trigger_by != NULL ? s_ctl.trigger_by : "button");
}

static void cold_next(uint32_t now)
{
    if (cold_protocol_next(s_ctl.cp, now)) {
        cold_session_next(s_ctl.cs);
        ESP_LOGI(TAG, "cold: next");
    }
}

static void cold_abort(uint32_t now)
{
    if (cold_protocol_abort(s_ctl.cp, now)) {
        ESP_LOGI(TAG, "cold: aborted");
    }
}

/* One tick of the protocol: scenes, cues, phases, the end. */
static void cold_follow(uint32_t now)
{
    /* Ambient ended by itself (auto exit) during a run: the run ends too */
    if (cold_running() && mode_controller_outputs(s_ctl.mc)->mode == AMBIENCE_NORMAL) {
        cold_abort(now);
    }
    cold_protocol_tick(s_ctl.cp, now);
    const cold_outputs_t *co = cold_protocol_outputs(s_ctl.cp);
    if (co->phase != s_ctl.phase) {
        cold_session_phase(s_ctl.cs, co->phase, now);
        s_ctl.phase = co->phase;
        if (co->phase != COLD_IDLE) {
            ESP_LOGI(TAG, "cold: %s", phase_name(co->phase));
            cold_event_phase(co->phase);
        }
    }
    cold_scene_t scene;
    if (cold_protocol_take_scene(s_ctl.cp, &scene)) {
        cold_profile_t p;
        cfg_cold_profile_t full;
        profile_of(s_ctl.run_profile, &p, &full);
        const char *id = scene == COLD_SCENE_WARM ? full.scene_warm
                       : scene == COLD_SCENE_COLD ? full.scene_cold
                                                  : full.scene_after;
        char fallback[40];
        if (id[0] == '\0') {
            settings_t st;
            app_settings_snapshot(&st, NULL);
            strlcpy(fallback, st.scene_ambient, sizeof(fallback));
            id = fallback;
        }
        hue_task_recall_id(id, COLD_SCENE_FADE_MS);
    }
    cold_cue_t cue;
    if (cold_protocol_take_cue(s_ctl.cp, &cue)) {
        player_voice_cue((size_t)cue.id, cue.beep);
    }
    cold_end_t end;
    if (cold_protocol_take_end(s_ctl.cp, &end)) {
        cold_record_t rec;
        const int64_t utc = server_link_utc();
        if (cold_session_end(s_ctl.cs, end, utc > 0 ? (uint32_t)utc : 0, now, &rec)) {
            char json[COLD_SESSION_JSON_MAX];
            if (cold_session_to_json(&rec, json, sizeof(json)) > 0) {
                server_link_record(rec.id, json); /* kept until the server has stored it */
            }
            ESP_LOGI(TAG, "cold: %lu s cold exposure recorded", (unsigned long)rec.cold_s);
        }
        if (end == COLD_END_ABORTED) {
            player_voice_stop();
        }
        s_ctl.phase = COLD_IDLE;
        s_ctl.pending_exit = true;
        expander_io_cold_led(false);
        cold_event_end(end);
    }
    /* back to Normal after the run, as soon as a trigger is accepted */
    if (s_ctl.pending_exit) {
        if (mode_controller_outputs(s_ctl.mc)->mode == AMBIENCE_NORMAL) {
            s_ctl.pending_exit = false;
        } else if (trigger(TRIGGER_BUTTON, "button", now)) {
            s_ctl.pending_exit = false;
        }
    }
}

/* --------------------------------------------------------------- events --- */

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

static void handle_remote(int32_t code, uint32_t now)
{
    const bool ambient = mode_controller_outputs(s_ctl.mc)->mode == AMBIENCE_AMBIENT;
    switch ((remote_cmd_t)code) {
    case REMOTE_MODE_AMBIENT:
        if (!ambient && !trigger(TRIGGER_BUTTON, "server", now)) {
            ESP_LOGI(TAG, "server: Ambient refused (mode change in progress)");
        }
        break;
    case REMOTE_MODE_NORMAL:
        if (cold_running()) {
            cold_abort(now); /* the end brings Normal */
        } else if (ambient && !trigger(TRIGGER_BUTTON, "server", now)) {
            ESP_LOGI(TAG, "server: Normal refused (mode change in progress)");
        }
        break;
    case REMOTE_COLD_START:
        s_ctl.trigger_by = "server";
        cold_start(now);
        break;
    case REMOTE_COLD_NEXT:
        cold_next(now);
        break;
    case REMOTE_COLD_ABORT:
        cold_abort(now);
        break;
    default:
        break;
    }
}

static void handle_event(const event_t *ev, uint32_t now)
{
    switch (ev->type) {
    case EVT_CLAP:
    case EVT_BUTTON: {
        const bool clap = (ev->type == EVT_CLAP);
        if (cold_running()) {
            if (!clap) {
                cold_abort(now); /* main button during a run */
            }
            break;               /* a double clap during a run is ignored */
        }
        const bool ok = trigger(clap ? TRIGGER_CLAP : TRIGGER_BUTTON, clap ? "clap" : "button", now);
        const bool ambient = (mode_controller_outputs(s_ctl.mc)->mode == AMBIENCE_AMBIENT);
        ESP_LOGI(TAG, "%s: %s", clap ? "clap" : "button",
                 !ok ? "ignored" : (ambient ? "entering Ambient" : "leaving Ambient"));
        break;
    }
    case EVT_VOLUME: {
        const int slot = volume_slot();
        if (mode_volume_turn(s_ctl.vol, slot, ev->detents, now)) {
            ESP_LOGI(TAG, "volume %s step %u", bath_volume_name(slot), (unsigned)mode_volume_step(s_ctl.vol, slot));
            server_link_status_changed();
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
    case EVT_COLD_PRESS:
        if (cold_running()) {
            cold_next(now);
        } else {
            s_ctl.trigger_by = "button";
            cold_start(now);
        }
        break;
    case EVT_COLD_HOLD:
        cold_abort(now);
        break;
    case EVT_COLD_SWITCH:
        s_ctl.switch_hype = (ev->detents != 0);
        if (cold_running()) {
            const uint8_t which = chosen_profile();
            if (which != s_ctl.run_profile) {
                cold_profile_t p;
                cfg_cold_profile_t full;
                profile_of(which, &p, &full);
                if (cold_protocol_set_profile(s_ctl.cp, &p)) { /* from the next phase on (COLD-13) */
                    s_ctl.run_profile = which;
                    ESP_LOGI(TAG, "cold: switched to %s from the next phase", profile_name(which));
                }
            }
        }
        server_link_status_changed();
        break;
    case EVT_REMOTE:
        handle_remote(ev->detents, now);
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
        s_ctl.music_on = (a->value != 0); /* follow_music() picks the folder */
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

/* The default folder: "A", or the root on a card without it (v1 cards). */
static const char *ambient_folder(void)
{
    return player_folder_tracks(FOLDER_AMBIENT) > 0 ? FOLDER_AMBIENT : "";
}

/* What the player should do now, asked only when it changes. */
static void follow_music(void)
{
    bool on = s_ctl.music_on;
    const char *folder = ambient_folder();
    if (on && cold_running()) {
        switch (cold_protocol_outputs(s_ctl.cp)->music) {
        case COLD_MUSIC_NONE:
            on = false; /* countdown, warm, cold without music: silence (the voice still plays) */
            break;
        case COLD_MUSIC_HYPE:
            folder = FOLDER_HYPE;
            break;
        case COLD_MUSIC_CALM:
            folder = player_folder_tracks(FOLDER_CALM) > 0 ? FOLDER_CALM : FOLDER_AMBIENT;
            break;
        }
    }
    if (on == s_ctl.player_on && (!on || strcmp(folder, s_ctl.player_folder) == 0)) {
        return;
    }
    if (on) {
        player_play_folder(folder, false, NULL); /* the "A" playlist continues its round */
    } else {
        player_play(false);
    }
    s_ctl.player_on = on;
    strlcpy(s_ctl.player_folder, folder, sizeof(s_ctl.player_folder));
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
    const uint8_t step = mode_volume_step(s_ctl.vol, gain_slot());
    const bool powered = tas5825m_is_powered(s_ctl.amp);
    const uint8_t addr = amp_io_address();
    app_status_t *st = app_status_lock();
    st->ambient = (o->mode == AMBIENCE_AMBIENT);
    st->transitioning = o->transitioning;
    st->volume_step = step;
    st->amp_powered = powered;
    st->amp_i2c_addr = addr;
    app_status_unlock();

    const cold_outputs_t *co = cold_protocol_outputs(s_ctl.cp);
    control_status_t cs = {
        .mode = co->phase != COLD_IDLE ? STATUS_BATH_COLD
              : o->mode == AMBIENCE_AMBIENT ? STATUS_BATH_AMBIENT
                                            : STATUS_BATH_NORMAL,
        .volume_ambient = mode_volume_step(s_ctl.vol, BATH_VOL_AMBIENT),
        .volume_cold = mode_volume_step(s_ctl.vol, BATH_VOL_COLD),
        .cold_running = co->phase != COLD_IDLE,
        .cold_phase = phase_name(co->phase),
        .cold_remaining_s = (co->remaining_ms + 999u) / 1000u,
        .cold_profile = profile_name(co->phase != COLD_IDLE ? s_ctl.run_profile : chosen_profile()),
        .cold_switch = expander_io_present() ? (s_ctl.switch_hype ? "hype" : "zen") : "none",
    };
    portENTER_CRITICAL(&s_status_lock);
    s_status = cs;
    portEXIT_CRITICAL(&s_status_lock);

    /* mode events: changes that were not ours are the auto exit (timer) */
    if (o->mode != s_ctl.last_mode) {
        mode_event(o->mode == AMBIENCE_AMBIENT ? "ambient" : "normal",
                   s_ctl.by_us ? (s_ctl.trigger_by != NULL ? s_ctl.trigger_by : "button") : "timer");
        s_ctl.last_mode = o->mode;
    }
    s_ctl.by_us = false;
}

void control_task_status(control_status_t *out)
{
    portENTER_CRITICAL(&s_status_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

/* ------------------------------------------------------- hue and saving --- */

static void forward_hue_command(void)
{
    hue_command_t cmd;
    if (mode_controller_take_hue_command(s_ctl.mc, &cmd)) {
        if (cold_running()) {
            return; /* the run's scenes rule the lights */
        }
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
        forward_hue_command(); /* before cold_follow: a run started this cycle drops it */
        cold_follow(now);
        apply_outputs(now);
        follow_music();
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
