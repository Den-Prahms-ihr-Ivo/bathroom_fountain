/*
 * music_sync – see music_sync.h.
 */
#include "music_sync.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "enroll_task.h"
#include "player_task.h"
#include "server_link.h"
#include "sync_manifest.h"
#include "wifi.h"

#define SYNC_TASK_STACK   8192u
#define SYNC_TASK_PRIO    2u
#define SYNC_PAYLOAD_MAX  (64u * 1024u) /* 256 entries of ~170 bytes */
#define CHUNK             4096u
#define HTTP_TIMEOUT_MS   15000
#define TRIES             2
#define LOCAL_PATH_MAX    (sizeof(PLAYER_MOUNT_POINT) + 1 + SYNC_PATH_MAX + 6)
#define URL_MAX           (SYNC_BASE_MAX + 3 * SYNC_PATH_MAX)

static const char *TAG = "music_sync";
static volatile bool s_running;

typedef struct {
    bool held;          /* player_hold(true) was sent */
    unsigned have;
    unsigned missing;
    unsigned downloaded;
    unsigned deleted;
    uint8_t buf[CHUNK];
    char user[33];
    char pass[65];
} sync_ctx_t;

/* -------------------------------------------------------------- helpers --- */

static void local_path(char *out, size_t n, const char *rel, const char *suffix)
{
    snprintf(out, n, PLAYER_MOUNT_POINT "/%s%s", rel, suffix);
}

static void hold(sync_ctx_t *c)
{
    if (!c->held) {
        player_hold(true);
        c->held = true;
        vTaskDelay(pdMS_TO_TICKS(200)); /* the player closes its files */
    }
}

/* SHA-256 of the first `limit` bytes of an open file (all of it if limit is
 * UINT32_MAX) into ctx; returns the bytes hashed. */
static uint32_t hash_file(FILE *f, mbedtls_sha256_context *sha, uint8_t *buf, uint32_t limit)
{
    uint32_t total = 0;
    size_t got;
    while (total < limit && (got = fread(buf, 1, CHUNK, f)) > 0) {
        if (got > limit - total) {
            got = limit - total;
        }
        mbedtls_sha256_update(sha, buf, got);
        total += (uint32_t)got;
    }
    return total;
}

static bool file_matches(const char *path, const sync_file_t *want, uint8_t *buf)
{
    struct stat st;
    if (stat(path, &st) != 0 || (uint32_t)st.st_size != want->size) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    const uint32_t n = hash_file(f, &sha, buf, UINT32_MAX);
    fclose(f);
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    return n == want->size && memcmp(digest, want->sha256, 32) == 0;
}

static void ensure_folder(const char *rel)
{
    const char *slash = strchr(rel, '/');
    if (slash == NULL) {
        return;
    }
    char dir[LOCAL_PATH_MAX];
    snprintf(dir, sizeof(dir), PLAYER_MOUNT_POINT "/%.*s", (int)(slash - rel), rel);
    struct stat st;
    if (stat(dir, &st) != 0) {
        mkdir(dir, 0775);
    }
}

/* ------------------------------------------------------------- download --- */

typedef enum { DL_OK, DL_RETRY, DL_FAIL } dl_result_t;

/* Download (or continue) one file into its .part, verify, rename. */
static dl_result_t download(sync_ctx_t *c, const sync_manifest_t *m, const sync_file_t *want)
{
    char dest[LOCAL_PATH_MAX], part[LOCAL_PATH_MAX], url[URL_MAX];
    local_path(dest, sizeof(dest), want->path, "");
    local_path(part, sizeof(part), want->path, ".part");
    if (!sync_manifest_url(m, want->path, url, sizeof(url))) {
        return DL_FAIL;
    }
    ensure_folder(want->path);

    /* what is already there: hash it so the digest covers the whole file */
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    uint32_t have = 0;
    FILE *f = fopen(part, "rb");
    if (f != NULL) {
        have = hash_file(f, &sha, c->buf, want->size);
        fclose(f);
    }
    if (have >= want->size && want->size > 0) {
        unlink(part); /* too long or complete but unverified: start over */
        mbedtls_sha256_free(&sha);
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        have = 0;
    }

    dl_result_t result = DL_RETRY;
    esp_http_client_handle_t h = NULL;
    f = NULL;
    if (!wifi_wait_connected(10000)) {
        goto out;
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .username = c->user,
        .password = c->pass,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        .buffer_size = 2048,
    };
    h = esp_http_client_init(&cfg);
    if (h == NULL) {
        goto out;
    }
    char range[32];
    if (have > 0) {
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)have);
        esp_http_client_set_header(h, "Range", range);
    }
    if (esp_http_client_open(h, 0) != ESP_OK || esp_http_client_fetch_headers(h) < 0) {
        ESP_LOGW(TAG, "%s: no connection", want->path);
        goto out;
    }
    const int status = esp_http_client_get_status_code(h);
    if (status == 200 && have > 0) {
        /* the server ignored Range: start over */
        have = 0;
        mbedtls_sha256_free(&sha);
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
    } else if (status != 200 && status != 206) {
        ESP_LOGW(TAG, "%s: HTTP %d", want->path, status);
        result = (status == 404 || status == 401 || status == 403) ? DL_FAIL : DL_RETRY;
        goto out;
    }
    f = fopen(part, have > 0 ? "ab" : "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s: cannot write (card full or read-only?)", part);
        result = DL_FAIL;
        goto out;
    }
    for (;;) {
        const int got = esp_http_client_read(h, (char *)c->buf, CHUNK);
        if (got < 0) {
            ESP_LOGW(TAG, "%s: connection lost at %lu bytes", want->path, (unsigned long)have);
            goto out; /* the .part stays: the next try continues */
        }
        if (got == 0) {
            break;
        }
        if (have + (uint32_t)got > want->size) {
            ESP_LOGW(TAG, "%s: longer than listed", want->path);
            result = DL_FAIL;
            fclose(f);
            f = NULL;
            unlink(part);
            goto out;
        }
        if (fwrite(c->buf, 1, (size_t)got, f) != (size_t)got) {
            ESP_LOGE(TAG, "%s: write failed (card full?)", want->path);
            result = DL_FAIL;
            goto out;
        }
        mbedtls_sha256_update(&sha, c->buf, (size_t)got);
        have += (uint32_t)got;
    }
    fclose(f);
    f = NULL;
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    if (have != want->size || memcmp(digest, want->sha256, 32) != 0) {
        ESP_LOGW(TAG, "%s: %s, discarded", want->path, have != want->size ? "incomplete" : "checksum differs");
        unlink(part);
        result = (have != want->size) ? DL_RETRY : DL_FAIL;
        goto out;
    }
    struct stat st;
    if (stat(dest, &st) == 0) {
        hold(c); /* replacing a file the player may have open */
        unlink(dest);
    }
    if (rename(part, dest) != 0) {
        ESP_LOGE(TAG, "%s: rename failed (%d)", want->path, errno);
        result = DL_FAIL;
        goto out;
    }
    result = DL_OK;
out:
    if (f != NULL) {
        fclose(f);
    }
    if (h != NULL) {
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
    }
    mbedtls_sha256_free(&sha);
    return result;
}

/* --------------------------------------------------------------- delete --- */

static void delete_unlisted_in(sync_ctx_t *c, const sync_manifest_t *m, const char *folder)
{
    char dir_path[LOCAL_PATH_MAX];
    if (folder[0] == '\0') {
        snprintf(dir_path, sizeof(dir_path), PLAYER_MOUNT_POINT);
    } else {
        snprintf(dir_path, sizeof(dir_path), PLAYER_MOUNT_POINT "/%s", folder);
    }
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        return;
    }
    /* collect first: deleting while readdir walks the same folder is not safe on FAT */
    enum { BATCH = 16 };
    char victims[BATCH][SYNC_PATH_MAX + 8];
    size_t n = 0;
    bool more = false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR || e->d_name[0] == '.' || !sync_name_managed(e->d_name)) {
            continue;
        }
        if (strlen(e->d_name) > SYNC_NAME_MAX + 5) {
            continue; /* longer than any name a sync writes (+ ".part") */
        }
        char rel[SYNC_PATH_MAX + 8];
        if (folder[0] == '\0') {
            snprintf(rel, sizeof(rel), "%.100s", e->d_name);
        } else {
            snprintf(rel, sizeof(rel), "%.23s/%.100s", folder, e->d_name);
        }
        if (sync_manifest_has(m, rel)) {
            continue;
        }
        if (n == BATCH) {
            more = true;
            break;
        }
        strlcpy(victims[n++], rel, sizeof(victims[0]));
    }
    closedir(d);
    size_t gone = 0;
    for (size_t i = 0; i < n; i++) {
        char path[LOCAL_PATH_MAX + 8];
        snprintf(path, sizeof(path), PLAYER_MOUNT_POINT "/%.126s", victims[i]);
        hold(c);
        if (unlink(path) == 0) {
            c->deleted++;
            gone++;
            ESP_LOGI(TAG, "deleted %s", victims[i]);
        } else {
            ESP_LOGW(TAG, "cannot delete %s", victims[i]);
        }
    }
    if (more && gone == n) { /* only while every delete works: no endless loop */
        delete_unlisted_in(c, m, folder); /* the next batch */
    }
}

/* The root and the root folders of the last scan. A folder this sync made
 * holds listed files only, so it needs no pass. */
static void delete_unlisted(sync_ctx_t *c, const sync_manifest_t *m)
{
    delete_unlisted_in(c, m, "");
    char folders[PLAYER_MAX_FOLDERS][PLAYER_FOLDER_MAX + 1];
    const size_t n = player_folders(folders, PLAYER_MAX_FOLDERS);
    for (size_t i = 0; i < n; i++) {
        delete_unlisted_in(c, m, folders[i]);
    }
}

/* ----------------------------------------------------------------- task --- */

static void report(const sync_ctx_t *c)
{
    char json[128];
    snprintf(json, sizeof(json), "{\"type\":\"music\",\"have\":%u,\"missing\":%u,\"free_mb\":%lu}", c->have,
             c->missing, (unsigned long)player_sd_free_mb());
    server_link_event(json);
}

static void run(sync_ctx_t *c, const char *payload)
{
    sync_mf_err_t err;
    sync_manifest_t *m = sync_manifest_parse(payload, strlen(payload), &err);
    if (m == NULL) {
        ESP_LOGW(TAG, "file list refused (%s)", err == SYNC_MF_ERR_MEMORY ? "no memory" : "format");
        server_link_cmd_result("music/sync", false, err == SYNC_MF_ERR_MEMORY ? "memory" : "format");
        return;
    }
    if (sync_manifest_skipped(m) > 0) {
        ESP_LOGW(TAG, "%u entries of the file list skipped (invalid path, size or sha256)",
                 (unsigned)sync_manifest_skipped(m));
    }
    if (!player_sd_mounted()) {
        ESP_LOGW(TAG, "no SD card");
        server_link_cmd_result("music/sync", false, "no_card");
        sync_manifest_destroy(m);
        return;
    }
    cfg_net_t net;
    enroll_task_net(&net);
    strlcpy(c->user, net.mqtt_user, sizeof(c->user));
    strlcpy(c->pass, net.mqtt_pass, sizeof(c->pass));
    memset(&net, 0, sizeof(net));

    const size_t count = sync_manifest_count(m);
    ESP_LOGI(TAG, "sync: %u files listed", (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        const sync_file_t *want = sync_manifest_file(m, i);
        char dest[LOCAL_PATH_MAX];
        local_path(dest, sizeof(dest), want->path, "");
        if (file_matches(dest, want, c->buf)) {
            c->have++;
            continue;
        }
        dl_result_t r = DL_RETRY;
        for (int t = 0; t < TRIES && r == DL_RETRY; t++) {
            r = download(c, m, want);
        }
        if (r == DL_OK) {
            c->have++;
            c->downloaded++;
            ESP_LOGI(TAG, "%s: done (%u/%u)", want->path, (unsigned)(i + 1), (unsigned)count);
        } else {
            c->missing++;
        }
    }
    delete_unlisted(c, m);
    sync_manifest_destroy(m);
    memset(c->pass, 0, sizeof(c->pass));
    ESP_LOGI(TAG, "sync done: %u files on the card, %u downloaded, %u deleted, %u missing", c->have, c->downloaded,
             c->deleted, c->missing);
}

static void sync_task(void *arg)
{
    char *payload = arg;
    sync_ctx_t *c = calloc(1, sizeof(*c));
    if (c != NULL) {
        run(c, payload);
        if (c->held) {
            player_hold(false); /* rescans and continues */
        } else if (c->downloaded > 0) {
            player_rescan();    /* new files only: counts and playlists */
        }
        report(c);
        free(c);
    }
    free(payload);
    s_running = false;
    vTaskDelete(NULL);
}

bool music_sync_start(const char *payload, size_t len)
{
    if (s_running) {
        server_link_cmd_result("music/sync", false, "busy");
        return false;
    }
    if (payload == NULL || len == 0 || len > SYNC_PAYLOAD_MAX) {
        server_link_cmd_result("music/sync", false, "format");
        return false;
    }
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, payload, len);
    copy[len] = '\0';
    s_running = true;
    if (xTaskCreate(sync_task, "music_sync", SYNC_TASK_STACK, copy, SYNC_TASK_PRIO, NULL) != pdPASS) {
        free(copy);
        s_running = false;
        return false;
    }
    return true;
}

bool music_sync_running(void)
{
    return s_running;
}
