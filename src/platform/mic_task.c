/*
 * mic_task – reads the microphone, runs mic_frontend and clap_detector, and
 * logs and posts EVT_CLAP for every confirmed double clap. See mic_task.h.
 *
 * The frontend and detector are built from the shared settings. When a
 * clap_* or mic_* key changes (app_settings_audio_gen()), both are rebuilt
 * between two blocks, so the console never touches them.
 */
#include "mic_task.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_settings.h"
#include "app_status.h"
#include "board_pins.h"
#include "clap_detector.h"
#include "events.h"
#include "mic_frontend.h"

#define SAMPLE_RATE_HZ 16000u
#define FRAME_SAMPLES  256u                    /* 16 ms per block */
/* BEH-02 warm-up, in two phases:
 *   SETTLE  the mic's startup transient: the detector does not see it at all
 *   ADAPT   the detector runs, so its background estimate (which rises only
 *           ~10 dB/s) catches up with steady noise like a running shower, but
 *           its detections are ignored */
#define SETTLE_SAMPLES (1u * SAMPLE_RATE_HZ)
#define ADAPT_SAMPLES  (5u * SAMPLE_RATE_HZ)
#define WARMUP_SAMPLES (SETTLE_SAMPLES + ADAPT_SAMPLES)
#define MAX_EVENTS     4u

#define MIC_TASK_STACK 6144u
#define MIC_TASK_PRIO  10u
#define MIC_TASK_CORE  1

static const char *TAG = "mic";

/* ---- from tools/recorder/src/main.c, unchanged ---------------------------- */

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
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
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

/* --------------------------------------------------------------------------- */

typedef struct {
    mic_frontend_t *frontend;
    clap_detector_t *detector;
    uint32_t gen; /* app_settings_audio_gen() the chain was built from */
} chain_t;

/*
 * (Re)build frontend and detector from the current settings. On failure the
 * old chain is kept (it can only fail on out-of-memory: settings ranges keep
 * every config valid). chain->gen is updated either way, so a failed build is
 * not retried every block.
 */
static bool chain_build(chain_t *chain)
{
    settings_t s;
    app_settings_snapshot(&s, &chain->gen);

    const mic_frontend_config_t mf_cfg = {
        .sample_rate_hz = SAMPLE_RATE_HZ,
        .shift = s.mic_shift,
        .hp_cutoff_hz = s.mic_hp_hz,
    };
    const clap_config_t clap_cfg = {
        .sample_rate_hz = SAMPLE_RATE_HZ,
        .min_gap_ms = s.clap_min_gap_ms,
        .max_gap_ms = s.clap_max_gap_ms,
        .max_level_diff_db = s.clap_level_diff_db,
        .quiet_before_ms = s.clap_quiet_before_ms,
        .quiet_after_ms = s.clap_quiet_after_ms,
        .quiet_gap_factor = s.clap_gap_factor,
        .min_drop_db = s.clap_drop_db,
        .onset_threshold_db = s.clap_onset_db,
        .min_peak_dbfs = s.clap_min_peak_dbfs,
    };

    mic_frontend_t *mf = mic_frontend_create(&mf_cfg);
    clap_detector_t *det = clap_detector_create(&clap_cfg);
    if (mf == NULL || det == NULL) {
        ESP_LOGE(TAG, "cannot build frontend/detector from the settings");
        mic_frontend_destroy(mf);
        clap_detector_destroy(det);
        return false;
    }
    mic_frontend_destroy(chain->frontend);
    clap_detector_destroy(chain->detector);
    chain->frontend = mf;
    chain->detector = det;
    ESP_LOGI(TAG, "detector: gap %u-%u ms, onset %.1f dB, min peak %.1f dBFS, hp %.1f Hz, shift %u",
             (unsigned)clap_cfg.min_gap_ms, (unsigned)clap_cfg.max_gap_ms,
             (double)clap_cfg.onset_threshold_db, (double)clap_cfg.min_peak_dbfs,
             (double)mf_cfg.hp_cutoff_hz, (unsigned)mf_cfg.shift);
    return true;
}

static void log_double_clap(const clap_detector_t *det, const clap_event_t *ev)
{
    /* The event is confirmed only after the quiet-after window, so the claps
     * themselves were a little earlier than now. */
    const uint32_t now_ms = esp_log_timestamp();
    const uint64_t since_b = clap_detector_samples_processed(det) - ev->second_onset_sample;
    const uint32_t clap_ms = now_ms - (uint32_t)(since_b * 1000u / SAMPLE_RATE_HZ);
    const uint32_t gap_ms =
        (uint32_t)((ev->second_onset_sample - ev->first_onset_sample) * 1000u / SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "DOUBLE CLAP at %lu ms (second clap at %lu ms, gap %lu ms)",
             (unsigned long)now_ms, (unsigned long)clap_ms, (unsigned long)gap_ms);
}

static void mic_task(void *arg)
{
    (void)arg;
    chain_t chain = {0};
    if (!chain_build(&chain)) {
        ESP_LOGE(TAG, "no clap detection");
        vTaskDelete(NULL);
        return;
    }

    i2s_chan_handle_t rx = mic_start();

    static int32_t words[FRAME_SAMPLES];
    static int16_t pcm[FRAME_SAMPLES];
    clap_event_t events[MAX_EVENTS];
    uint32_t warmup = WARMUP_SAMPLES;
    uint32_t seen_overflows = 0;

    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, words, sizeof(words), &got, 1000) != ESP_OK || got != sizeof(words)) {
            continue;
        }
        const uint32_t ovf = s_overflows;
        if (ovf != seen_overflows) {
            ESP_LOGW(TAG, "I2S overflow, %lu blocks lost so far", (unsigned long)ovf);
            seen_overflows = ovf;
        }

        if (app_settings_audio_gen() != chain.gen && chain_build(&chain)) {
            warmup = WARMUP_SAMPLES; /* new filter and background estimate settle like at boot */
            app_status_lock()->mic_listening = false;
            app_status_unlock();
        }

        /* The frontend always runs, so its high-pass has settled when the
         * detector starts. */
        mic_frontend_process(chain.frontend, words, FRAME_SAMPLES, pcm);
        if (warmup > ADAPT_SAMPLES) {
            warmup = (warmup - ADAPT_SAMPLES > FRAME_SAMPLES) ? warmup - FRAME_SAMPLES : ADAPT_SAMPLES;
            if (warmup == ADAPT_SAMPLES) {
                clap_detector_reset(chain.detector); /* starts from the settled signal */
            }
            continue;
        }

        size_t n = clap_detector_process(chain.detector, pcm, FRAME_SAMPLES, events, MAX_EVENTS);
        if (warmup > 0) {
            warmup = (warmup > FRAME_SAMPLES) ? warmup - FRAME_SAMPLES : 0;
            if (n > 0) {
                ESP_LOGI(TAG, "warm-up: %u detection(s) ignored", (unsigned)n);
            }
            n = 0;
            if (warmup == 0) {
                ESP_LOGI(TAG, "warm-up done, listening");
                app_status_lock()->mic_listening = true;
                app_status_unlock();
            }
        }
        for (size_t i = 0; i < n; i++) {
            log_double_clap(chain.detector, &events[i]);
            events_post(EVT_CLAP, 0);
            app_status_t *st = app_status_lock();
            st->claps++;
            st->last_clap_ms = app_now_ms();
            app_status_unlock();
        }
    }
}

void mic_task_start(void)
{
    xTaskCreatePinnedToCore(mic_task, "mic", MIC_TASK_STACK, NULL, MIC_TASK_PRIO, NULL, MIC_TASK_CORE);
}
