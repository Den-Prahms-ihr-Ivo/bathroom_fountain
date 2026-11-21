/*
 * cfg_store – keeps the v2 settings groups (lib/node_config: net, bath, bed,
 * game) in NVS, one namespace per group ("g_<group>"), one entry per key as
 * the text kv_format() writes (secrets revealed). NVS keys are limited to 15
 * characters: a longer key is stored under its first 15 characters, the same
 * rule app_settings uses for the v1 keys.
 *
 * The caller owns the group struct. All calls take one shared mutex, so a
 * group struct used by several tasks is read with cfg_store_lock()/unlock()
 * around it (or copied out under the lock).
 *
 * v1 settings (settings_t, lib/settings) stay in app_settings.c.
 */
#ifndef CFG_STORE_H
#define CFG_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "kv_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Defaults, then every stored key fed back through kv_set(). A stored value
 * kv_set() refuses (old firmware, range changed) is logged and skipped.
 * Call after nvs_flash_init(). */
esp_err_t cfg_store_load(const kv_table_t *table, void *data);

/* kv_set() on data; on KV_OK the key is saved. *saved (may be NULL) gets the
 * NVS result, ESP_OK if nothing was tried. */
kv_result_t cfg_store_set(const kv_table_t *table, void *data, const char *key, const char *value,
                          esp_err_t *saved);

/* Remove key from NVS and put its default back into data. */
esp_err_t cfg_store_reset_key(const kv_table_t *table, void *data, const char *key);

void cfg_store_lock(void);
void cfg_store_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* CFG_STORE_H */
