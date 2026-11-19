/*
 * settings_sync – settings between device and server as JSON (pure C, uses
 * cJSON; no MQTT, no NVS).
 *
 * Requirements: SRV-05 ... SRV-09 (see the requirements doc).
 * Tests:        test/test_settings_sync/
 *
 * Settings live on the device. The server publishes what it wants ("desired",
 * retained); the device validates every value with the rules it already has,
 * keeps what passes and answers with what it now holds ("reported"). This
 * module does the two JSON documents; the platform moves them over MQTT and
 * writes changed values to NVS.
 *
 * Store
 *   The settings themselves are reached through four callbacks, so the module
 *   does not care that they are spread over lib/settings and several
 *   kv_table groups. src/platform chains those into one store.
 *
 * Desired document (server -> device)
 *     {"version": 12, "values": {"transition_ms": "4000", "hype_voice": "off"}}
 *   version  integer 1 ... 2147483647, raised by the server with every change
 *   values   object; each member is a setting key with its value as a JSON
 *            string in the same text form the console uses. Keys that are
 *            missing keep their value.
 *
 * settings_sync_apply()
 *   SYNC_ERR_FORMAT  not JSON, not an object, version missing or not such an
 *                    integer, values missing or not an object. Nothing
 *                    changes.
 *   SYNC_STALE       version is not greater than the version applied last
 *                    (a retained message seen again, or an old one). Nothing
 *                    changes.
 *   SYNC_APPLIED     otherwise. The members of values are handled in document
 *                    order:
 *                      - value not a JSON string       -> rejected "invalid"
 *                      - store->format(key, reveal) gives exactly this text
 *                                                      -> nothing to do
 *                      - a secret whose new value is "********"
 *                                                      -> nothing to do (the
 *                        server echoes the mask for secrets it never saw)
 *                      - otherwise store->set(); SYNC_SET_OK counts as
 *                        changed, any other result as rejected with the
 *                        reason "unknown", "invalid", "range" or "conflict".
 *                    The applied version becomes the document's version, even
 *                    if values were rejected. The rejected list of the
 *                    previous apply is replaced; at most SYNC_MAX_REJECTED
 *                    entries are kept (further ones are only counted).
 *
 * Reported document (device -> server), settings_sync_report()
 *     {"version": 12,
 *      "values": {"wifi_ssid": "home", "wifi_pass": "********", ...},
 *      "rejected": {"volume_max": "range"}}
 *   version   the applied version (0 before the first apply)
 *   values    every key of the store in store order, formatted without
 *             revealing secrets
 *   rejected  the list of the last apply; {} if none
 *   The same document is sent after a local change (console, fallback page):
 *   the server then sees values that differ from its desired set and adopts
 *   them.
 */
#ifndef SETTINGS_SYNC_H
#define SETTINGS_SYNC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_MAX_REJECTED 16
#define SYNC_VALUE_MAX 96 /* longest value text including the NUL */

typedef enum {
    SYNC_SET_OK = 0,
    SYNC_SET_UNKNOWN,
    SYNC_SET_INVALID,
    SYNC_SET_RANGE,
    SYNC_SET_CONFLICT,
} sync_set_t;

typedef struct {
    void *ctx;
    size_t (*count)(void *ctx);
    const char *(*name)(void *ctx, size_t index);
    bool (*is_secret)(void *ctx, const char *key);
    sync_set_t (*set)(void *ctx, const char *key, const char *value);
    bool (*format)(void *ctx, const char *key, bool reveal_secrets, char *out, size_t out_size);
} sync_store_t;

typedef enum {
    SYNC_APPLIED = 0,
    SYNC_STALE,
    SYNC_ERR_FORMAT,
} sync_status_t;

typedef struct {
    sync_status_t status;
    uint32_t version;  /* version of the document (0 on SYNC_ERR_FORMAT) */
    uint16_t changed;  /* values that were set                           */
    uint16_t rejected; /* values that were refused (all of them)         */
} sync_result_t;

typedef struct settings_sync settings_sync_t;

/* The store must outlive the object; its callbacks must all be set.
 * Returns NULL if store or a callback is NULL. */
settings_sync_t *settings_sync_create(const sync_store_t *store);

/* Accepts NULL. */
void settings_sync_destroy(settings_sync_t *sync);

/* Applied version: 0 after create. The platform restores the value it kept
 * in NVS with settings_sync_set_version() at boot. */
uint32_t settings_sync_version(const settings_sync_t *sync);
void settings_sync_set_version(settings_sync_t *sync, uint32_t version);

/* json need not be NUL-terminated. */
sync_result_t settings_sync_apply(settings_sync_t *sync, const char *json, size_t len);

/* Write the reported document as compact JSON, NUL-terminated. Returns its
 * length, or -1 if out is too small or memory ran out (out is then empty if
 * out_size > 0). */
int settings_sync_report(const settings_sync_t *sync, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_SYNC_H */
