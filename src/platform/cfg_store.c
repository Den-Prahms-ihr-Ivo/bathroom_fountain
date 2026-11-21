/*
 * cfg_store – see cfg_store.h.
 */
#include "cfg_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define NVS_KEY_MAX 15
#define VALUE_MAX   128 /* longest formatted value: alarm text / mqtt_pass (64) */

static const char *TAG = "cfg_store";

static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_buf;

void cfg_store_lock(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mutex_buf); /* first call is from app_main */
    }
    xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY);
}

void cfg_store_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

static void ns_name(const kv_table_t *table, char *out, size_t n)
{
    snprintf(out, n, "g_%s", table->name); /* NVS namespace: 15 characters */
}

static void nvs_key(const char *key, char *out)
{
    strncpy(out, key, NVS_KEY_MAX);
    out[NVS_KEY_MAX] = '\0';
}

esp_err_t cfg_store_load(const kv_table_t *table, void *data)
{
    if (table == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg_store_lock();
    kv_defaults(table, data);
    char ns[16];
    ns_name(table, ns, sizeof(ns));
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        cfg_store_unlock();
        return ESP_OK; /* nothing stored yet */
    }
    if (err != ESP_OK) {
        cfg_store_unlock();
        return err;
    }
    for (size_t i = 0; i < table->count; i++) {
        const char *key = table->keys[i].name;
        char k[NVS_KEY_MAX + 1];
        nvs_key(key, k);
        char value[VALUE_MAX];
        size_t len = sizeof(value);
        if (nvs_get_str(h, k, value, &len) != ESP_OK) {
            continue;
        }
        if (kv_set(table, data, key, value) != KV_OK) {
            ESP_LOGW(TAG, "%s.%s: stored value refused, default kept", table->name, key);
        }
    }
    nvs_close(h);
    cfg_store_unlock();
    return ESP_OK;
}

static esp_err_t save_key(const kv_table_t *table, const void *data, const char *key)
{
    char value[VALUE_MAX];
    if (!kv_format(table, data, key, true, value, sizeof(value))) {
        return ESP_ERR_INVALID_SIZE;
    }
    char ns[16];
    ns_name(table, ns, sizeof(ns));
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char k[NVS_KEY_MAX + 1];
    nvs_key(key, k);
    err = nvs_set_str(h, k, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

kv_result_t cfg_store_set(const kv_table_t *table, void *data, const char *key, const char *value,
                          esp_err_t *saved)
{
    if (saved != NULL) {
        *saved = ESP_OK;
    }
    if (table == NULL || data == NULL) {
        return KV_ERR_INVALID;
    }
    cfg_store_lock();
    const kv_result_t r = kv_set(table, data, key, value);
    if (r == KV_OK) {
        const esp_err_t err = save_key(table, data, key);
        if (saved != NULL) {
            *saved = err;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s.%s not saved: %s", table->name, key, esp_err_to_name(err));
        }
    }
    cfg_store_unlock();
    return r;
}

esp_err_t cfg_store_reset_key(const kv_table_t *table, void *data, const char *key)
{
    if (table == NULL || data == NULL || kv_find(table, key) < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg_store_lock();
    const kv_key_t *kk = &table->keys[kv_find(table, key)];
    if (kk->def != NULL) {
        (void)kv_set(table, data, key, kk->def);
    } else {
        memset((char *)data + kk->offset, 0, kk->size);
    }
    char ns[16];
    ns_name(table, ns, sizeof(ns));
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        char k[NVS_KEY_MAX + 1];
        nvs_key(key, k);
        err = nvs_erase_key(h, k);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    cfg_store_unlock();
    return err;
}
