/*
 * app_settings – shared settings with NVS persistence. See app_settings.h.
 */
#include "app_settings.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define NVS_NAMESPACE "settings"
#define NVS_KEY_LEN   (NVS_KEY_NAME_MAX_SIZE - 1)

static const char *TAG = "settings";

static settings_t s_settings;
static SemaphoreHandle_t s_lock;
static nvs_handle_t s_nvs;
static bool s_nvs_open;
static uint32_t s_audio_gen;

static void nvs_key_of(const char *key, char out[NVS_KEY_NAME_MAX_SIZE])
{
    strncpy(out, key, NVS_KEY_LEN);
    out[NVS_KEY_LEN] = '\0';
}

static bool is_audio_key(const char *key)
{
    return strncmp(key, "clap_", 5) == 0 || strncmp(key, "mic_", 4) == 0;
}

static bool nvs_keys_unique(void)
{
    const size_t n = settings_key_count();
    for (size_t i = 0; i < n; i++) {
        char a[NVS_KEY_NAME_MAX_SIZE];
        nvs_key_of(settings_key_name(i), a);
        for (size_t j = i + 1; j < n; j++) {
            char b[NVS_KEY_NAME_MAX_SIZE];
            nvs_key_of(settings_key_name(j), b);
            if (strcmp(a, b) == 0) {
                ESP_LOGE(TAG, "keys %s and %s share the NVS key %s",
                         settings_key_name(i), settings_key_name(j), a);
                return false;
            }
        }
    }
    return true;
}

/* Feed one stored key back through settings_set(). Returns false only on a
 * conflict, so the caller can retry it once the other keys are loaded. */
static bool load_key(const char *key, bool last_try)
{
    char nkey[NVS_KEY_NAME_MAX_SIZE];
    char text[APP_SETTINGS_VALUE_MAX];
    size_t len = sizeof(text);
    nvs_key_of(key, nkey);
    const esp_err_t err = nvs_get_str(s_nvs, nkey, text, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true; /* never set: default stays */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: cannot read (%s), default kept", key, esp_err_to_name(err));
        return true;
    }
    const settings_result_t r = settings_set(&s_settings, key, text);
    if (r == SETTINGS_ERR_CONFLICT && !last_try) {
        return false;
    }
    if (r != SETTINGS_OK) {
        ESP_LOGW(TAG, "%s: stored value rejected (%d), default kept", key, (int)r);
    }
    return true;
}

esp_err_t app_settings_load(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    settings_defaults(&s_settings);
    if (!nvs_keys_unique()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s; running on defaults, nothing is saved", esp_err_to_name(err));
        return err;
    }
    s_nvs_open = true;

    /* A conflict (clap_min_gap_ms vs clap_max_gap_ms) can depend on the load
     * order, so conflicting keys get a second try after all the others. */
    const size_t n = settings_key_count();
    bool retry[n];
    for (size_t i = 0; i < n; i++) {
        retry[i] = !load_key(settings_key_name(i), false);
    }
    for (size_t i = 0; i < n; i++) {
        if (retry[i]) {
            load_key(settings_key_name(i), true);
        }
    }
    return ESP_OK;
}

static esp_err_t save_key(const char *key)
{
    if (!s_nvs_open) {
        return ESP_ERR_NVS_INVALID_HANDLE;
    }
    char nkey[NVS_KEY_NAME_MAX_SIZE];
    char text[APP_SETTINGS_VALUE_MAX];
    nvs_key_of(key, nkey);
    if (!settings_format(&s_settings, key, true, text, sizeof(text))) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = nvs_set_str(s_nvs, nkey, text);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    return err;
}

settings_result_t app_settings_set(const char *key, const char *value, esp_err_t *saved)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const settings_result_t r = settings_set(&s_settings, key, value);
    if (r == SETTINGS_OK) {
        err = save_key(key);
        if (is_audio_key(key)) {
            s_audio_gen++;
        }
    }
    xSemaphoreGive(s_lock);
    if (saved != NULL) {
        *saved = err;
    }
    return r;
}

bool app_settings_format(const char *key, bool reveal_secrets, char *out, size_t out_size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = settings_format(&s_settings, key, reveal_secrets, out, out_size);
    xSemaphoreGive(s_lock);
    return ok;
}

void app_settings_snapshot(settings_t *out, uint32_t *audio_gen)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    if (audio_gen != NULL) {
        *audio_gen = s_audio_gen;
    }
    xSemaphoreGive(s_lock);
}

uint32_t app_settings_audio_gen(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t gen = s_audio_gen;
    xSemaphoreGive(s_lock);
    return gen;
}
