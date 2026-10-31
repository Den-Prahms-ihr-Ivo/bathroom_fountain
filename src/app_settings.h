/*
 * app_settings – the one shared settings_t of the firmware, guarded by a
 * mutex and persisted in NVS (CLAP-12, NFR-10, NFR-15).
 *
 * NVS stores each key as the text settings_format(..., reveal=true) produces,
 * in namespace "settings". NVS key names are limited to 15 characters, so a
 * longer settings key is stored under its first 15 characters
 * ("clap_quiet_before_ms" → "clap_quiet_befo"); app_settings_load() checks
 * that these stay unique.
 */
#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings.h"

/* Room for any formatted value (the longest is hue_key, 64 chars). */
#define APP_SETTINGS_VALUE_MAX 80

/* Boot: defaults, then every stored key fed back through settings_set().
 * Call once, after nvs_flash_init(), before any task uses the settings. */
esp_err_t app_settings_load(void);

/* settings_set() on the shared settings; on success the key is saved to NVS.
 * *saved (may be NULL) gets the NVS result, ESP_OK if nothing was tried. */
settings_result_t app_settings_set(const char *key, const char *value, esp_err_t *saved);

/* settings_format() on the shared settings. */
bool app_settings_format(const char *key, bool reveal_secrets, char *out, size_t out_size);

/* A copy of all settings, and the audio generation it belongs to. */
void app_settings_snapshot(settings_t *out, uint32_t *audio_gen);

/* Increases whenever a clap_* or mic_* key changes. */
uint32_t app_settings_audio_gen(void);

#endif /* APP_SETTINGS_H */
