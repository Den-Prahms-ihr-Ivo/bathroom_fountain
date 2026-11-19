/*
 * player_task – see player_task.h.
 */
#include "player_task.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "app_status.h"
#include "board_pins.h"
#include "mp3_decoder.h"
#include "pcm_mix.h"
#include "playlist.h"

#if defined(PIN_AMP_I2S_BCLK)
#include "driver/i2s_std.h"
#define HAVE_I2S 1
#else
#define HAVE_I2S 0
#endif

#define MOUNT_POINT        "/sdcard"
#define SD_FREQ_KHZ        10000  /* plenty for MP3; raise only if a card needs it */
#define IN_BUF_BYTES       16384u
#define DEFAULT_RATE_HZ    44100u
#define DMA_DESC_NUM       8
#define DMA_FRAME_NUM      512    /* 8 x 512 frames: ~93 ms of audio buffered */
#define PLAYER_TASK_STACK  (24u * 1024u) /* minimp3 keeps ~16 KB of scratch on the stack */
#define PLAYER_TASK_PRIO   7u
#define PLAYER_TASK_CORE   0
#define PATH_MAX_LEN       (sizeof(MOUNT_POINT) + 1 + 128)

static const char *TAG = "player";

typedef enum { CMD_PLAY, CMD_STOP, CMD_NEXT, CMD_RESCAN } cmd_t;

static QueueHandle_t s_cmds;

/* --------------------------------------------------------------- state --- */

static playlist_t *s_pl;
static sdmmc_card_t *s_card;
static bool s_bus_ready;
static bool s_mounted;

static bool s_playing;            /* wanted by control_task                 */
static FILE *s_file;
static char s_name[128];          /* current track                          */
static bool s_eof;
static bool s_need_more;          /* the decoder asked for more data        */
static uint32_t s_frames_ok;      /* decoded MP3 frames of this track       */

static uint8_t s_in[IN_BUF_BYTES];
static size_t s_in_len;           /* valid bytes in s_in                    */
static int16_t s_pcm[MP3_DECODER_MAX_SAMPLES];
static int16_t s_out[MP3_DECODER_MAX_SAMPLES];
static mp3_decoder_t s_dec;

static uint32_t s_rate = DEFAULT_RATE_HZ;
static int64_t s_dry_start_us;    /* dry run pacing                         */
static uint64_t s_dry_played_us;

#if HAVE_I2S
static i2s_chan_handle_t s_tx;
#endif

static void publish(void)
{
    app_status_t *s = app_status_lock();
    s->sd_mounted = s_mounted;
    s->tracks = (uint16_t)(s_pl != NULL ? playlist_count(s_pl) : 0);
    s->playing = s_playing && s_file != NULL;
    app_status_copy(s->track, sizeof(s->track), s_file != NULL ? s_name : "");
    app_status_unlock();
}

/* ----------------------------------------------------------------- I2S --- */

#if HAVE_I2S
static void i2s_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear_after_cb = true; /* zeros when nothing is written: clocks keep running */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE_HZ),
        /* 16-bit samples in 32-bit slots: BCLK = 64 x fs, a ratio every TAS58xx accepts */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_AMP_I2S_BCLK,
            .ws = PIN_AMP_I2S_WS,
            .dout = PIN_AMP_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    /* The default ws_width is the sample width (16). With 32-bit slots the
     * word-select line must switch every 32 BCLK, otherwise the amplifier gets
     * a lopsided frame: it still locks to 48 kHz but reads silence. */
    std_cfg.slot_cfg.ws_width = 32;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
}
#endif

static void set_rate(uint32_t hz)
{
    if (hz == s_rate || hz == 0) {
        return;
    }
    ESP_LOGI(TAG, "sample rate %lu Hz", (unsigned long)hz);
#if HAVE_I2S
    /* Disabling halts the clocks for longer than the 100 us the amplifier
     * needs to notice a rate change. */
    i2s_channel_disable(s_tx);
    const i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(hz);
    if (i2s_channel_reconfig_std_clock(s_tx, &clk) != ESP_OK) {
        ESP_LOGW(TAG, "cannot set %lu Hz", (unsigned long)hz);
    }
    i2s_channel_enable(s_tx);
#endif
    s_rate = hz;
    s_dry_start_us = esp_timer_get_time();
    s_dry_played_us = 0;
}

/* Diagnostics ("amp tone"): replaces the music with a 1 kHz sine on both
 * channels, so the I2S -> amplifier -> speaker path can be tested on its own. */
static volatile bool s_tone;
static float s_tone_phase;

void player_test_tone(bool on)
{
    s_tone = on;
}

static void tone_fill(int16_t *stereo, size_t frames)
{
    const float step = 2.0f * 3.14159265f * 1000.0f / (float)(s_rate ? s_rate : 48000u);
    for (size_t i = 0; i < frames; i++) {
        const int16_t v = (int16_t)(16000.0f * sinf(s_tone_phase)); /* about -6 dBFS */
        stereo[2 * i] = v;
        stereo[2 * i + 1] = v;
        s_tone_phase += step;
        if (s_tone_phase > 2.0f * 3.14159265f) {
            s_tone_phase -= 2.0f * 3.14159265f;
        }
    }
}

static void output(const int16_t *stereo, size_t frames)
{
    if (s_tone) {
        tone_fill((int16_t *)stereo, frames); /* our own output buffer */
    }
#if HAVE_I2S
    size_t written = 0;
    if (i2s_channel_write(s_tx, stereo, frames * 2u * sizeof(int16_t), &written, 1000) != ESP_OK) {
        ESP_LOGW(TAG, "I2S write timed out");
    }
#else
    /* Dry run: keep real time so the bench behaves like the real device. */
    (void)stereo;
    s_dry_played_us += (uint64_t)frames * 1000000u / s_rate;
    const int64_t ahead_us = (int64_t)s_dry_played_us - (esp_timer_get_time() - s_dry_start_us);
    if (ahead_us > 20000) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(ahead_us / 1000)));
    }
#endif
}

/* ------------------------------------------------------------ SD card --- */

static bool sd_mount(void)
{
    if (s_mounted) {
        return true;
    }
    if (!s_bus_ready) {
        const spi_bus_config_t bus = {
            .mosi_io_num = PIN_SD_MOSI,
            .miso_io_num = PIN_SD_MISO,
            .sclk_io_num = PIN_SD_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus init failed");
            return false;
        }
        s_bus_ready = true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = PIN_SD_CS;
    dev.host_id = SPI2_HOST;
    const esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false, /* never erase someone's music */
        .max_files = 2,
        .allocation_unit_size = 0,
    };
    const esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &dev, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s): FAT32 card inserted? wiring?", esp_err_to_name(err));
        s_card = NULL;
        return false;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "SD card: %s, %llu MB", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024u * 1024u));
    return true;
}

static void sd_unmount(void)
{
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_mounted = false;
        s_card = NULL;
    }
}

static void scan(void)
{
    playlist_clear(s_pl);
    if (sd_mount()) {
        DIR *dir = opendir(MOUNT_POINT);
        unsigned skipped = 0;
        struct dirent *e;
        while (dir != NULL && (e = readdir(dir)) != NULL) {
            const playlist_add_result_t r = playlist_add(s_pl, e->d_name, e->d_type == DT_DIR);
            if (r == PLAYLIST_SKIPPED_TOO_LONG) {
                ESP_LOGW(TAG, "name too long, skipped: %s", e->d_name);
            }
            if (r != PLAYLIST_ADDED) {
                skipped++;
            }
        }
        if (dir != NULL) {
            closedir(dir);
        }
        ESP_LOGI(TAG, "%u MP3 files found (%u other entries ignored)", (unsigned)playlist_count(s_pl) , skipped);
    }
    playlist_finalize(s_pl);
    publish();
}

/* -------------------------------------------------------------- tracks --- */

static void close_track(void)
{
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    publish();
}

/* Skip an ID3v2 tag: it can hold large cover images the decoder would
 * otherwise have to search through. */
static void skip_id3v2(FILE *f)
{
    uint8_t h[10];
    if (fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, "ID3", 3) == 0 &&
        ((h[6] | h[7] | h[8] | h[9]) & 0x80) == 0) {
        long size = ((long)h[6] << 21) | ((long)h[7] << 14) | ((long)h[8] << 7) | (long)h[9];
        size += 10;
        if (h[5] & 0x10) {
            size += 10; /* footer */
        }
        fseek(f, size, SEEK_SET);
    } else {
        fseek(f, 0, SEEK_SET);
    }
}

/* Open the next playable track. Returns false if there is none. */
static bool open_next(void)
{
    close_track();
    if (playlist_count(s_pl) == 0) {
        scan(); /* maybe the card was inserted or swapped since */
    }
    for (size_t tries = playlist_count(s_pl) + 1; tries > 0; tries--) {
        const char *name = playlist_next(s_pl);
        if (name == NULL) {
            break;
        }
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof(path), MOUNT_POINT "/%s", name);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            ESP_LOGW(TAG, "cannot open %s", name);
            playlist_mark_bad(s_pl, name);
            continue;
        }
        skip_id3v2(f);
        s_file = f;
        app_status_copy(s_name, sizeof(s_name), name);
        s_eof = false;
        s_need_more = false;
        s_in_len = 0;
        s_frames_ok = 0;
        mp3_decoder_reset(&s_dec);
        ESP_LOGI(TAG, "playing %s", name);
        publish();
        return true;
    }
    ESP_LOGW(TAG, "no music: put MP3 files in the card's root folder");
    publish();
    return false;
}

static void end_track(void)
{
    if (s_frames_ok == 0) {
        ESP_LOGW(TAG, "%s: no MP3 audio found, skipped for this session", s_name);
        playlist_mark_bad(s_pl, s_name);
    }
    close_track();
}

/* Decode and output one MP3 frame. */
static void play_step(void)
{
    /* Refill in large reads when the buffer runs low, or whenever the
     * decoder could not find a complete frame in what is there. */
    if (!s_eof && s_in_len < IN_BUF_BYTES && (s_in_len < IN_BUF_BYTES / 2 || s_need_more)) {
        s_need_more = false;
        const size_t got = fread(s_in + s_in_len, 1, IN_BUF_BYTES - s_in_len, s_file);
        s_in_len += got;
        if (got == 0) {
            if (ferror(s_file)) {
                ESP_LOGE(TAG, "read error: card removed?");
                close_track();
                sd_unmount();
                playlist_clear(s_pl);
                playlist_finalize(s_pl);
                publish();
                return;
            }
            s_eof = true;
        }
    }

    mp3_frame_info_t info;
    const int samples = mp3_decoder_decode(&s_dec, s_in, s_in_len, s_pcm, &info);
    if (info.frame_bytes == 0) {
        if (s_eof) {
            end_track();
        } else if (s_in_len == IN_BUF_BYTES) {
            s_in_len = 0; /* a full buffer without a frame: junk, drop it */
        } else {
            s_need_more = true;
        }
        return;
    }
    memmove(s_in, s_in + info.frame_bytes, s_in_len - (size_t)info.frame_bytes);
    s_in_len -= (size_t)info.frame_bytes;
    if (samples <= 0 || (info.channels != 1 && info.channels != 2)) {
        return; /* skipped data (e.g. a tag) */
    }
    s_frames_ok++;
    set_rate((uint32_t)info.hz);
    const size_t frames = pcm_mix_to_output(s_pcm, (size_t)samples, (unsigned)info.channels, PCM_RIGHT_SILENT, s_out);
    output(s_out, frames);
}

/* ---------------------------------------------------------------- task --- */

static void handle(cmd_t cmd)
{
    switch (cmd) {
    case CMD_PLAY:
        if (!s_playing) {
            s_playing = true;
            s_dry_start_us = esp_timer_get_time();
            s_dry_played_us = 0;
            open_next(); /* continues the shuffle round */
        }
        break;
    case CMD_STOP:
        if (s_playing) {
            s_playing = false;
            close_track();
            ESP_LOGI(TAG, "stopped");
        }
        break;
    case CMD_NEXT:
        if (s_playing) {
            open_next();
        }
        break;
    case CMD_RESCAN:
        close_track();
        sd_unmount();
        scan();
        if (s_playing) {
            open_next();
        }
        break;
    }
    publish();
}

static void player_task(void *arg)
{
    (void)arg;
#if HAVE_I2S
    i2s_start();
#else
    ESP_LOGI(TAG, "no amplifier on this board: playback is a dry run (decode only)");
#endif
    scan();

    for (;;) {
        cmd_t cmd;
        const bool active = s_playing && s_file != NULL;
        if (xQueueReceive(s_cmds, &cmd, active ? 0 : portMAX_DELAY) == pdTRUE) {
            handle(cmd);
            continue;
        }
        if (active) {
            play_step();
            if (s_file == NULL && s_playing) {
                if (!open_next()) {
                    /* nothing playable: wait for the next command */
                }
            }
        }
    }
}

void player_task_start(void)
{
    playlist_config_t cfg;
    playlist_config_default(&cfg);
    cfg.shuffle = true;          /* AUD-03 */
    cfg.seed = esp_random();
    cfg.max_name_len = 128;
    s_pl = playlist_create(&cfg);
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    if (s_pl == NULL || s_cmds == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    mp3_decoder_init(&s_dec);
    xTaskCreatePinnedToCore(player_task, "player", PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO, NULL,
                            PLAYER_TASK_CORE);
}

static void send(cmd_t cmd)
{
    if (s_cmds == NULL || xQueueSend(s_cmds, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command %d dropped", (int)cmd);
    }
}

void player_play(bool on)
{
    send(on ? CMD_PLAY : CMD_STOP);
}

void player_next(void)
{
    send(CMD_NEXT);
}

void player_rescan(void)
{
    send(CMD_RESCAN);
}
