/*
 * player_task – see player_task.h.
 */
#include "player_task.h"

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

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
#include "resume_restore.h"
#include "voice_mix.h"
#include "wav_clip.h"

#if defined(PIN_AMP_I2S_BCLK)
#include "driver/i2s_std.h"
#define HAVE_I2S 1
#else
#define HAVE_I2S 0
#endif

#define MOUNT_POINT        PLAYER_MOUNT_POINT
#define SD_FREQ_KHZ        10000  /* plenty for MP3; raise only if a card needs it */
#define SD_MAX_FILES       5      /* track, voice clip, music sync file, 2 directory scans */
#define IN_BUF_BYTES       16384u
#define DEFAULT_RATE_HZ    44100u
#define DMA_DESC_NUM       8
#define DMA_FRAME_NUM      512    /* 8 x 512 frames: ~93 ms of audio buffered */
#define PLAYER_TASK_STACK  (24u * 1024u) /* minimp3 keeps ~16 KB of scratch on the stack */
#define PLAYER_TASK_PRIO   7u
#define PLAYER_TASK_CORE   0
#define NAME_MAX_LEN       128    /* playlist names (v1); KEEP playlists use the resume limit */
#define PATH_MAX_LEN       (sizeof(MOUNT_POINT) + 1 + PLAYER_FOLDER_MAX + 1 + NAME_MAX_LEN)
#define CMD_QUEUE_LEN      8
#define SILENCE_FRAMES     256    /* blocks of silence under a voice clip */
#define CLIP_PATH_MAX      48
#define WAV_HEAD_BYTES     512
#define BEEP_RATE_HZ       16000u
#define BEEP_HZ            1000.0f
#define BEEP_MS            220u
#define BEEP_AMPLITUDE     9000.0f
#define VOICE_FOLDER       "V"

static const char *TAG = "player";

typedef enum {
    CMD_PLAY,
    CMD_STOP,
    CMD_NEXT,
    CMD_RESCAN,
    CMD_FOLDER,
    CMD_SHUFFLE,
    CMD_HOLD,
    CMD_RELEASE,
    CMD_CUE,
    CMD_VOICE_STOP,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    bool keep;   /* CMD_FOLDER */
    bool has_rp; /* CMD_FOLDER */
    bool beep;   /* CMD_CUE    */
    uint8_t cue; /* CMD_CUE    */
    char folder[PLAYER_FOLDER_MAX + 1];
    resume_point_t rp;
} cmd_t;

typedef struct {
    char name[PLAYER_FOLDER_MAX + 1];
    uint16_t count;
} folder_info_t;

typedef struct {
    bool valid;
    wav_clip_info_t info;
} clip_t;

static QueueHandle_t s_cmds;

/* ------------------------------------------------------- shared snapshot --- */
/* Written by the player task, read by anyone: under s_lock (short copies). */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static player_pos_t s_pos;
static folder_info_t s_folders[PLAYER_MAX_FOLDERS];
static size_t s_nfolders;
static uint16_t s_root_tracks;
static uint32_t s_clip_ms[PLAYER_VOICE_MAX];
static char s_default_folder[PLAYER_FOLDER_MAX + 1];
static char s_clip_paths[PLAYER_VOICE_MAX][CLIP_PATH_MAX];
static size_t s_clip_count;
static volatile bool s_ended;
static volatile bool s_mounted;

/* ------------------------------------------------------ player-task state --- */

static playlist_t *s_pl;
static char s_pl_folder[PLAYER_FOLDER_MAX + 1];
static bool s_pl_keep;
static uint32_t s_seed;
static uint32_t s_index;          /* playlist_next() calls since finalize   */

static char s_cur_folder[PLAYER_FOLDER_MAX + 1]; /* what is wanted now   */
static bool s_cur_keep;

static sdmmc_card_t *s_card;
static bool s_bus_ready;

static bool s_playing;            /* wanted by the app                      */
static bool s_hold;               /* music sync holds the card              */
static FILE *s_file;
static char s_name[NAME_MAX_LEN]; /* current track (name inside the folder) */
static bool s_eof;
static bool s_need_more;          /* the decoder asked for more data        */
static uint32_t s_frames_ok;      /* decoded MP3 frames of this track       */
static uint32_t s_file_pos;       /* bytes read from the file so far        */

static uint8_t s_in[IN_BUF_BYTES];
static size_t s_in_len;           /* valid bytes in s_in                    */
static int16_t s_pcm[MP3_DECODER_MAX_SAMPLES];
static int16_t s_out[MP3_DECODER_MAX_SAMPLES];
static mp3_decoder_t s_dec;

static uint32_t s_rate = DEFAULT_RATE_HZ;
static int64_t s_dry_start_us;    /* dry run pacing                         */
static uint64_t s_dry_played_us;

static voice_mix_t *s_vm;
static clip_t s_clips[PLAYER_VOICE_MAX];
static FILE *s_clip_file;
static uint32_t s_clip_left;      /* samples still to read                  */
static uint32_t s_beep_left;
static float s_beep_phase;

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

static void pos_track(const char *folder, const char *track)
{
    portENTER_CRITICAL(&s_lock);
    s_pos.playing = (track[0] != '\0');
    s_pos.keep = s_pl_keep;
    strlcpy(s_pos.folder, folder, sizeof(s_pos.folder));
    strlcpy(s_pos.track, track, sizeof(s_pos.track));
    s_pos.seed = s_seed;
    s_pos.index = s_index;
    s_pos.offset = s_file_pos;
    portEXIT_CRITICAL(&s_lock);
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
    voice_mix_set_out_rate(s_vm, hz);
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

/* Voice over the block, then the right channel back to silence (the voice
 * mixer writes both channels; this build plays on the left only). */
static void mix_voice(int16_t *stereo, size_t frames)
{
    voice_mix_process(s_vm, stereo, frames);
    for (size_t i = 0; i < frames; i++) {
        stereo[2 * i + 1] = 0;
    }
}

static void output(int16_t *stereo, size_t frames)
{
    if (s_tone) {
        tone_fill(stereo, frames);
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
        .max_files = SD_MAX_FILES,
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

static void voice_close(void);

static void sd_unmount(void)
{
    if (s_mounted) {
        voice_close();
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_mounted = false;
        s_card = NULL;
    }
}

static bool ends_with_ci(const char *name, const char *ext)
{
    const size_t n = strlen(name);
    const size_t m = strlen(ext);
    return n > m && strcasecmp(name + n - m, ext) == 0;
}

/* Audio files directly in dir (MP3, or WAV when wav is set). */
static uint16_t count_files(const char *dir, bool wav)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    uint16_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR && e->d_name[0] != '.' && ends_with_ci(e->d_name, wav ? ".wav" : ".mp3")) {
            n++;
        }
    }
    closedir(d);
    return n;
}

static int folder_cmp(const void *a, const void *b)
{
    return strcmp(((const folder_info_t *)a)->name, ((const folder_info_t *)b)->name);
}

/* Read the WAV headers of the configured clips (lib/wav_clip). */
static void load_clips(void)
{
    uint32_t ms[PLAYER_VOICE_MAX] = {0};
    char paths[PLAYER_VOICE_MAX][CLIP_PATH_MAX];
    size_t count;
    portENTER_CRITICAL(&s_lock);
    count = s_clip_count;
    memcpy(paths, s_clip_paths, sizeof(paths));
    portEXIT_CRITICAL(&s_lock);
    for (size_t i = 0; i < PLAYER_VOICE_MAX; i++) {
        s_clips[i].valid = false;
        if (i >= count || !s_mounted) {
            continue;
        }
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof(path), MOUNT_POINT "/%.47s", paths[i]);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            ESP_LOGW(TAG, "voice clip %s missing: beep instead", paths[i]);
            continue;
        }
        struct stat st;
        uint8_t head[WAV_HEAD_BYTES];
        const size_t got = fread(head, 1, sizeof(head), f);
        const bool sized = (fstat(fileno(f), &st) == 0);
        fclose(f);
        wav_clip_info_t info;
        const wav_clip_result_t r = sized ? wav_clip_parse(head, got, (uint32_t)st.st_size, &info)
                                          : WAV_CLIP_ERR_TRUNCATED;
        if (r != WAV_CLIP_OK) {
            ESP_LOGW(TAG, "voice clip %s not usable (%d): must be 16-bit mono PCM WAV", paths[i], (int)r);
            continue;
        }
        s_clips[i].valid = true;
        s_clips[i].info = info;
        ms[i] = info.duration_ms;
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(s_clip_ms, ms, sizeof(ms));
    portEXIT_CRITICAL(&s_lock);
}

/* Mount if needed, count tracks per root folder, read the voice clips. The
 * playlists are rebuilt at the next start. */
static void scan(void)
{
    static folder_info_t found[PLAYER_MAX_FOLDERS];
    size_t n = 0;
    uint16_t root = 0;
    if (sd_mount()) {
        DIR *dir = opendir(MOUNT_POINT);
        struct dirent *e;
        while (dir != NULL && (e = readdir(dir)) != NULL) {
            if (e->d_name[0] == '.') {
                continue;
            }
            if (e->d_type == DT_DIR) {
                if (strlen(e->d_name) > PLAYER_FOLDER_MAX || n >= PLAYER_MAX_FOLDERS) {
                    ESP_LOGW(TAG, "folder skipped: %s", e->d_name);
                    continue;
                }
                char path[PATH_MAX_LEN];
                snprintf(path, sizeof(path), MOUNT_POINT "/%.23s", e->d_name); /* length checked above */
                strlcpy(found[n].name, e->d_name, sizeof(found[n].name));
                found[n].count = count_files(path, strcmp(e->d_name, VOICE_FOLDER) == 0);
                n++;
            } else if (ends_with_ci(e->d_name, ".mp3")) {
                root++;
            }
        }
        if (dir != NULL) {
            closedir(dir);
        }
        qsort(found, n, sizeof(found[0]), folder_cmp);
        ESP_LOGI(TAG, "card: %u MP3 files in the root, %u folders", (unsigned)root, (unsigned)n);
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(s_folders, found, n * sizeof(found[0]));
    s_nfolders = n;
    s_root_tracks = root;
    portEXIT_CRITICAL(&s_lock);
    load_clips();
    playlist_destroy(s_pl); /* folders may have changed: rebuilt at the next start */
    s_pl = NULL;
    publish();
}

/* -------------------------------------------------------------- tracks --- */

static void close_track(void)
{
    if (s_file != NULL) {
        fclose(s_file);
        s_file = NULL;
    }
    pos_track(s_cur_folder, "");
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

static void track_path(char *out, size_t n, const char *folder, const char *name)
{
    if (folder[0] == '\0') {
        snprintf(out, n, MOUNT_POINT "/%.127s", name);
    } else {
        snprintf(out, n, MOUNT_POINT "/%.23s/%.127s", folder, name);
    }
}

/* Open name in the playlist's folder at offset (0 = start, after the tag). */
static bool open_track(const char *name, uint32_t offset)
{
    char path[PATH_MAX_LEN];
    track_path(path, sizeof(path), s_pl_folder, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot open %s", path);
        return false;
    }
    skip_id3v2(f);
    if (offset > (uint32_t)ftell(f)) {
        fseek(f, (long)offset, SEEK_SET); /* the decoder finds the next frame */
    }
    s_file = f;
    s_file_pos = (uint32_t)ftell(f);
    app_status_copy(s_name, sizeof(s_name), name);
    s_eof = false;
    s_need_more = false;
    s_in_len = 0;
    s_frames_ok = 0;
    mp3_decoder_reset(&s_dec);
    ESP_LOGI(TAG, "playing %s%s%s%s", s_pl_folder, s_pl_folder[0] ? "/" : "", name, offset ? " (resumed)" : "");
    pos_track(s_pl_folder, name);
    publish();
    return true;
}

/* Rebuild the playlist of folder with this seed. */
static bool build_playlist(const char *folder, bool keep, uint32_t seed)
{
    playlist_destroy(s_pl);
    s_pl = NULL;
    playlist_config_t cfg;
    playlist_config_default(&cfg);
    cfg.shuffle = true; /* AUD-03 */
    cfg.seed = seed;
    cfg.max_name_len = keep ? (RESUME_TRACK_MAX + 1) : NAME_MAX_LEN;
    s_pl = playlist_create(&cfg);
    if (s_pl == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return false;
    }
    strlcpy(s_pl_folder, folder, sizeof(s_pl_folder));
    s_pl_keep = keep;
    s_seed = seed;
    s_index = 0;
    char path[PATH_MAX_LEN];
    track_path(path, sizeof(path), "", folder);
    DIR *dir = sd_mount() ? opendir(path) : NULL;
    struct dirent *e;
    unsigned skipped = 0;
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
    playlist_finalize(s_pl);
    ESP_LOGI(TAG, "%s: %u tracks", folder[0] ? folder : "(root)", (unsigned)playlist_count(s_pl));
    return true;
}

/* Open the next playable track of the playlist. false if there is none. */
static bool open_next(void)
{
    close_track();
    if (s_pl == NULL) {
        return false;
    }
    for (size_t tries = playlist_count(s_pl) + 1; tries > 0; tries--) {
        const char *name = playlist_next(s_pl);
        if (name == NULL) {
            break;
        }
        s_index++;
        if (open_track(name, 0)) {
            return true;
        }
        playlist_mark_bad(s_pl, name);
    }
    ESP_LOGW(TAG, "no music in %s", s_pl_folder[0] ? s_pl_folder : "the card's root");
    s_ended = true;
    publish();
    return false;
}

/* The folder player_play(true) means. */
static void default_folder(char *out, size_t n)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(out, s_default_folder, n);
    portEXIT_CRITICAL(&s_lock);
    if (out[0] != '\0' && player_folder_tracks(out) == 0) {
        out[0] = '\0'; /* v1 card: MP3s in the root */
    }
}

/* Start folder (see player_play_folder). */
static void start_folder(const char *folder, bool keep, const resume_point_t *rp)
{
    close_track();
    strlcpy(s_cur_folder, folder, sizeof(s_cur_folder));
    s_cur_keep = keep;
    if (!s_mounted) {
        scan(); /* maybe the card was inserted or swapped since */
    }
    /* the default folder's playlist continues its shuffle round */
    if (!keep && rp == NULL && s_pl != NULL && !s_pl_keep && strcmp(s_pl_folder, folder) == 0 &&
        playlist_count(s_pl) > 0) {
        open_next();
        return;
    }
    if (rp != NULL) {
        if (build_playlist(folder, true, rp->seed)) {
            uint32_t offset = 0;
            const char *name = resume_restore(s_pl, rp, folder, &offset);
            if (name != NULL) {
                s_index = rp->index;
                char copy[RESUME_TRACK_MAX + 1];
                strlcpy(copy, name, sizeof(copy));
                if (open_track(copy, offset)) {
                    return;
                }
            }
            ESP_LOGI(TAG, "resume point of %s not found: fresh shuffle", folder);
        }
        keep = true;
    }
    if (!build_playlist(folder, keep, esp_random())) {
        return;
    }
    if (playlist_count(s_pl) == 0 && s_mounted) {
        scan(); /* card swapped? one more try */
        if (!build_playlist(folder, keep, esp_random())) {
            return;
        }
    }
    open_next();
}

/* CMD_SHUFFLE: the playlist of `seed`, `played` tracks skipped, the next one. */
static void start_shuffle(const char *folder, uint32_t seed, uint32_t played)
{
    close_track();
    strlcpy(s_cur_folder, folder, sizeof(s_cur_folder));
    s_cur_keep = true;
    if (!s_mounted) {
        scan();
    }
    if (!build_playlist(folder, true, seed)) {
        return;
    }
    if (played > RESUME_REPLAY_MAX) {
        played = RESUME_REPLAY_MAX;
    }
    for (uint32_t i = 0; i < played && playlist_count(s_pl) > 0; i++) {
        if (playlist_next(s_pl) == NULL) {
            break;
        }
        s_index++;
    }
    open_next();
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
        s_file_pos += (uint32_t)got;
        if (got == 0) {
            if (ferror(s_file)) {
                ESP_LOGE(TAG, "read error: card removed?");
                close_track();
                sd_unmount();
                scan();
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
    s_pos.offset = s_file_pos - (uint32_t)s_in_len; /* one aligned word: no lock needed */
    if (samples <= 0 || (info.channels != 1 && info.channels != 2)) {
        return; /* skipped data (e.g. a tag) */
    }
    s_frames_ok++;
    set_rate((uint32_t)info.hz);
    const size_t frames = pcm_mix_to_output(s_pcm, (size_t)samples, (unsigned)info.channels, PCM_RIGHT_SILENT, s_out);
    mix_voice(s_out, frames);
    output(s_out, frames);
}

/* ---------------------------------------------------------------- voice --- */

static void voice_close(void)
{
    voice_mix_stop(s_vm);
    if (s_clip_file != NULL) {
        fclose(s_clip_file);
        s_clip_file = NULL;
    }
    s_beep_left = 0;
}

static size_t clip_read(void *ctx, int16_t *dst, size_t max)
{
    (void)ctx;
    if (s_clip_file == NULL || s_clip_left == 0) {
        return 0;
    }
    const size_t want = (max < s_clip_left) ? max : s_clip_left;
    const size_t got = fread(dst, sizeof(int16_t), want, s_clip_file); /* WAV is little-endian, so is the S3 */
    s_clip_left -= (uint32_t)got;
    if (got == 0 || s_clip_left == 0) {
        fclose(s_clip_file);
        s_clip_file = NULL;
        s_clip_left = 0;
    }
    return got;
}

static size_t beep_read(void *ctx, int16_t *dst, size_t max)
{
    (void)ctx;
    const size_t n = (max < s_beep_left) ? max : s_beep_left;
    const float step = 2.0f * 3.14159265f * BEEP_HZ / (float)BEEP_RATE_HZ;
    for (size_t i = 0; i < n; i++) {
        dst[i] = (int16_t)(BEEP_AMPLITUDE * sinf(s_beep_phase));
        s_beep_phase += step;
        if (s_beep_phase > 2.0f * 3.14159265f) {
            s_beep_phase -= 2.0f * 3.14159265f;
        }
    }
    s_beep_left -= (uint32_t)n;
    return n;
}

static void start_beep(void)
{
    s_beep_left = BEEP_RATE_HZ * BEEP_MS / 1000u;
    s_beep_phase = 0.0f;
    voice_mix_start(s_vm, BEEP_RATE_HZ, beep_read, NULL);
}

static void start_cue(size_t index, bool beep)
{
    voice_close();
    if (!s_mounted || beep || index >= PLAYER_VOICE_MAX || !s_clips[index].valid) {
        start_beep();
        return;
    }
    char paths[CLIP_PATH_MAX];
    portENTER_CRITICAL(&s_lock);
    strlcpy(paths, s_clip_paths[index], sizeof(paths));
    portEXIT_CRITICAL(&s_lock);
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), MOUNT_POINT "/%.47s", paths);
    s_clip_file = fopen(path, "rb");
    if (s_clip_file == NULL || fseek(s_clip_file, (long)s_clips[index].info.data_offset, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "voice clip %s not readable: beep", paths);
        voice_close();
        start_beep();
        return;
    }
    s_clip_left = s_clips[index].info.frames;
    voice_mix_start(s_vm, s_clips[index].info.sample_rate_hz, clip_read, NULL);
}

/* A voice clip without music: silence underneath, at the current rate. */
static void silence_step(void)
{
    memset(s_out, 0, SILENCE_FRAMES * 2u * sizeof(int16_t));
    mix_voice(s_out, SILENCE_FRAMES);
    output(s_out, SILENCE_FRAMES);
}

/* ---------------------------------------------------------------- task --- */

static void handle(const cmd_t *cmd)
{
    switch (cmd->type) {
    case CMD_PLAY:
        if (!s_playing) {
            s_playing = true;
            s_dry_start_us = esp_timer_get_time();
            s_dry_played_us = 0;
            char folder[PLAYER_FOLDER_MAX + 1];
            default_folder(folder, sizeof(folder));
            if (s_hold) {
                strlcpy(s_cur_folder, folder, sizeof(s_cur_folder)); /* started at release */
                s_cur_keep = false;
            } else {
                start_folder(folder, false, NULL);
            }
        }
        break;
    case CMD_FOLDER:
        s_playing = true;
        if (s_hold) {
            strlcpy(s_cur_folder, cmd->folder, sizeof(s_cur_folder)); /* started at release */
            s_cur_keep = cmd->keep;
        } else {
            start_folder(cmd->folder, cmd->keep, cmd->has_rp ? &cmd->rp : NULL);
        }
        break;
    case CMD_SHUFFLE:
        s_playing = true;
        if (s_hold) {
            strlcpy(s_cur_folder, cmd->folder, sizeof(s_cur_folder)); /* started fresh at release */
            s_cur_keep = true;
        } else {
            start_shuffle(cmd->folder, cmd->rp.seed, cmd->rp.index);
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
        if (s_playing && !s_hold) {
            open_next();
        }
        break;
    case CMD_RESCAN:
        close_track();
        voice_close();
        sd_unmount();
        scan();
        if (s_playing && !s_hold) {
            start_folder(s_cur_folder, s_cur_keep, NULL);
        }
        break;
    case CMD_HOLD:
        if (!s_hold) {
            s_hold = true;
            close_track();
            voice_close();
            ESP_LOGI(TAG, "card held for a music sync");
        }
        break;
    case CMD_RELEASE:
        if (s_hold) {
            s_hold = false;
            sd_unmount();
            scan();
            if (s_playing) {
                start_folder(s_cur_folder, s_cur_keep, NULL);
            }
        }
        break;
    case CMD_CUE:
        if (!s_hold) {
            start_cue(cmd->cue, cmd->beep);
        }
        break;
    case CMD_VOICE_STOP:
        voice_close();
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
        const bool music = s_playing && s_file != NULL && !s_hold;
        const bool voice = !s_hold && voice_mix_active(s_vm);
        if (xQueueReceive(s_cmds, &cmd, (music || voice) ? 0 : portMAX_DELAY) == pdTRUE) {
            handle(&cmd);
            continue;
        }
        if (music) {
            play_step();
            if (s_file == NULL && s_playing && !s_hold) {
                open_next(); /* nothing playable: waits for the next command */
            }
        } else if (voice) {
            silence_step();
        }
    }
}

void player_task_start(void)
{
    s_cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_t));
    voice_mix_config_t vcfg;
    voice_mix_config_default(&vcfg);
    vcfg.out_rate_hz = DEFAULT_RATE_HZ;
    s_vm = voice_mix_create(&vcfg);
    if (s_cmds == NULL || s_vm == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }
    mp3_decoder_init(&s_dec);
    xTaskCreatePinnedToCore(player_task, "player", PLAYER_TASK_STACK, NULL, PLAYER_TASK_PRIO, NULL,
                            PLAYER_TASK_CORE);
}

/* ------------------------------------------------------------- requests --- */

static void send(const cmd_t *cmd)
{
    if (s_cmds == NULL || xQueueSend(s_cmds, cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command %d dropped", (int)cmd->type);
    }
}

static void send_type(cmd_type_t type)
{
    cmd_t cmd = {.type = type};
    send(&cmd);
}

void player_play(bool on)
{
    send_type(on ? CMD_PLAY : CMD_STOP);
}

void player_set_default_folder(const char *folder)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_default_folder, folder != NULL ? folder : "", sizeof(s_default_folder));
    portEXIT_CRITICAL(&s_lock);
}

void player_play_folder(const char *folder, bool keep, const resume_point_t *rp)
{
    cmd_t cmd = {.type = CMD_FOLDER, .keep = keep, .has_rp = (rp != NULL)};
    strlcpy(cmd.folder, folder != NULL ? folder : "", sizeof(cmd.folder));
    if (rp != NULL) {
        cmd.rp = *rp;
    }
    send(&cmd);
}

void player_play_shuffle(const char *folder, uint32_t seed, uint32_t played)
{
    cmd_t cmd = {.type = CMD_SHUFFLE};
    strlcpy(cmd.folder, folder != NULL ? folder : "", sizeof(cmd.folder));
    cmd.rp.seed = seed;
    cmd.rp.index = played;
    send(&cmd);
}

void player_next(void)
{
    send_type(CMD_NEXT);
}

void player_rescan(void)
{
    send_type(CMD_RESCAN);
}

void player_hold(bool on)
{
    send_type(on ? CMD_HOLD : CMD_RELEASE);
}

void player_position(player_pos_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_pos;
    portEXIT_CRITICAL(&s_lock);
}

bool player_take_ended(void)
{
    if (!s_ended) {
        return false;
    }
    s_ended = false;
    return true;
}

bool player_sd_mounted(void)
{
    return s_mounted;
}

uint16_t player_folder_tracks(const char *folder)
{
    if (folder == NULL) {
        return 0;
    }
    uint16_t n = 0;
    portENTER_CRITICAL(&s_lock);
    if (folder[0] == '\0') {
        n = s_root_tracks;
    } else {
        for (size_t i = 0; i < s_nfolders; i++) {
            if (strcmp(s_folders[i].name, folder) == 0) {
                n = s_folders[i].count;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

size_t player_folders(char names[][PLAYER_FOLDER_MAX + 1], size_t max)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_lock);
    for (; n < s_nfolders && n < max; n++) {
        memcpy(names[n], s_folders[n].name, PLAYER_FOLDER_MAX + 1);
    }
    portEXIT_CRITICAL(&s_lock);
    return n;
}

uint32_t player_sd_free_mb(void)
{
    uint64_t total = 0, free_bytes = 0;
    if (!s_mounted || esp_vfs_fat_info(MOUNT_POINT, &total, &free_bytes) != ESP_OK) {
        return 0;
    }
    return (uint32_t)(free_bytes / (1024u * 1024u));
}

void player_voice_set_clips(const char *const paths[], size_t count)
{
    portENTER_CRITICAL(&s_lock);
    s_clip_count = (count < PLAYER_VOICE_MAX) ? count : PLAYER_VOICE_MAX;
    for (size_t i = 0; i < s_clip_count; i++) {
        strlcpy(s_clip_paths[i], paths[i], CLIP_PATH_MAX);
    }
    portEXIT_CRITICAL(&s_lock);
}

void player_voice_lengths(uint32_t ms[PLAYER_VOICE_MAX])
{
    portENTER_CRITICAL(&s_lock);
    memcpy(ms, s_clip_ms, sizeof(s_clip_ms));
    portEXIT_CRITICAL(&s_lock);
}

void player_voice_cue(size_t index, bool beep)
{
    cmd_t cmd = {.type = CMD_CUE, .cue = (uint8_t)index, .beep = beep};
    send(&cmd);
}

void player_voice_stop(void)
{
    send_type(CMD_VOICE_STOP);
}
