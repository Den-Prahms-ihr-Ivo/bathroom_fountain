/*
 * settings – every runtime setting as validated text key/value pairs
 * (pure C, no NVS, no console).
 *
 * Requirements: CLAP-12, NFR-10, NFR-15 (see the requirements doc).
 * Tests:        test/test_settings/
 *
 * One table of keys serves three users:
 *   - the serial console:  "set clap_onset_db 10.5", "get transition_ms", "show"
 *   - NVS persistence:     store settings_format(..., reveal=true) per key,
 *                          and at boot feed each stored text back through
 *                          settings_set(); a corrupt value is rejected and the
 *                          default stays
 *   - tools/clap_lab.py:   prints "set ..." lines with the same key names
 *
 * Keys, ranges and defaults (in this order for settings_key_name()):
 *
 *   key                    type    range                 default
 *   wifi_ssid              text    1–32 bytes            ""  (unset)
 *   wifi_pass              secret  "" or 8–63 chars      ""
 *   hue_host               text    1–63 chars            ""  (unset)
 *   hue_key                secret  1–64 chars            ""  (unset)
 *   scene_normal           uuid    36 chars              ""  (unset)
 *   scene_ambient          uuid    36 chars              ""  (unset)
 *   transition_ms          uint    3000–5000             4000
 *   volume_max             uint    1–32                  32
 *   clap_min_gap_ms        uint    50–1000               150
 *   clap_max_gap_ms        uint    100–2000              600
 *   clap_level_diff_db     float   0–20                  18.0  (tuned)
 *   clap_quiet_before_ms   uint    0–2000                300
 *   clap_quiet_after_ms    uint    0–2000                400
 *   clap_gap_factor        float   0–4                   1.5
 *   clap_drop_db           float   1–40                  12.0
 *   clap_onset_db          float   1–40                  20.0  (tuned)
 *   clap_min_peak_dbfs     float   -90–0                 -30.0 (tuned)
 *   mic_hp_hz              float   0–1000                20.0
 *   mic_shift              uint    8–24                  16
 *
 * Value syntax (checked first; failure → SETTINGS_ERR_INVALID)
 *   uint   one or more ASCII digits, nothing else (no sign, no spaces)
 *   float  optional '-', one or more digits, optionally '.' and 1–2 digits
 *          ("12", "-40.5", "1.25"; not "12.", ".5", "1e1", "nan", " 1")
 *   text   wifi_ssid: any bytes except control characters (< 0x20, 0x7F);
 *          hue_host: letters, digits, '.' and '-'
 *   secret wifi_pass: printable ASCII 0x20–0x7E;
 *          hue_key: letters, digits, '_' and '-'
 *   uuid   8-4-4-4-12 hex digits with hyphens, either case
 *   Strings are stored exactly as given (spaces in an SSID are kept).
 * Then the range (failure → SETTINGS_ERR_RANGE; a number too large for any
 * integer type is out of range, never wrapped), then cross-key rules
 * (failure → SETTINGS_ERR_CONFLICT): clap_min_gap_ms < clap_max_gap_ms.
 * A rejected set changes nothing.
 *
 * Output format (settings_format)
 *   uint as decimal; float with the fewest decimals (1 or 2) that represent
 *   it to 2 places ("12.0", "1.5", "1.25", "-40.0"); strings as stored;
 *   secrets as "********" unless reveal is true or the value is empty.
 *   format(reveal=true) followed by set() reproduces every value exactly.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[64];
    char hue_host[64];
    char hue_key[65];
    char scene_normal[37];
    char scene_ambient[37];
    uint32_t transition_ms;
    uint8_t volume_max;
    uint16_t clap_min_gap_ms;
    uint16_t clap_max_gap_ms;
    float clap_level_diff_db;
    uint16_t clap_quiet_before_ms;
    uint16_t clap_quiet_after_ms;
    float clap_gap_factor;
    float clap_drop_db;
    float clap_onset_db;
    float clap_min_peak_dbfs;
    float mic_hp_hz;
    uint8_t mic_shift;
} settings_t;

typedef enum {
    SETTINGS_OK = 0,
    SETTINGS_ERR_UNKNOWN_KEY,
    SETTINGS_ERR_INVALID,  /* syntax, or key/value NULL */
    SETTINGS_ERR_RANGE,
    SETTINGS_ERR_CONFLICT,
} settings_result_t;

void settings_defaults(settings_t *s);

settings_result_t settings_set(settings_t *s, const char *key, const char *value);

/* Write the value of key into out (NUL-terminated). Returns false for an
 * unknown key or if out is too small; out is then left empty if out_size > 0. */
bool settings_format(const settings_t *s, const char *key, bool reveal_secrets, char *out, size_t out_size);

size_t settings_key_count(void);

/* Key name by index (table order above), or NULL past the end. */
const char *settings_key_name(size_t index);

/* True when Wi-Fi SSID, Hue host and key and both scenes are set. */
bool settings_is_complete(const settings_t *s);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_H */
