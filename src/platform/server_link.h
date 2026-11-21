/*
 * server_link – everything a node says to the server over mqtt_link, and the
 * settings it takes from it (docs/PROTOCOL.md, MQTT).
 *
 *   status     shower/<id>/status, retained: lib/status_json with the common
 *              members filled here (fw, built, boot, reset, uptime_s,
 *              wifi_rssi, ip, heap_min, groups, settings_version) and the
 *              node members from app->build_status. Sent on connect, at most
 *              every 2 s after server_link_status_changed(), and every 60 s.
 *   settings   one lib/settings_sync over a chained store: the v1 keys
 *              (app_settings, group "settings") when app->v1_settings (all,
 *              or only app->v1_keys: the bedside needs Wi-Fi and Hue, not
 *              the clap keys), the net
 *              group (enroll_task) and the app's groups (cfg_store). Desired
 *              documents are applied, the version is kept in NVS, reported is
 *              published on connect, after every apply and after a local
 *              change (server_link_settings_changed_locally: console).
 *              app->on_settings(group) runs for every group with a changed
 *              value. Wi-Fi keys reconnect Wi-Fi; mqtt_* keys reconnect MQTT.
 *   records    lib/record_outbox (NVS "srv"/"outbox"): server_link_record()
 *              keeps a record until cmd/ack {"type":..,"id":..} confirms it.
 *   events     server_link_event() publishes one event document.
 *   commands   "ack" is handled here; every other command goes to
 *              app->on_cmd; false from it (unknown) gives the event
 *              {"type":"cmd","name":..,"ok":false,"why":"unknown"}.
 *
 * Call server_link_start() after enroll_task_start(); it starts mqtt_link.
 * All functions may be called from any task. Callbacks into the app run in
 * the server_link task (status, on_settings) or in the MQTT task (on_cmd,
 * on_connect): keep them short.
 */
#ifndef SERVER_LINK_H
#define SERVER_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kv_table.h"
#include "status_json.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVER_MAX_GROUPS 3 /* app groups, besides "settings" and "net" */

typedef struct {
    const kv_table_t *table;
    void *data; /* the app's group struct (written under cfg_store_lock) */
} server_group_t;

typedef struct {
    const char *node; /* "bath", "bedside", "bedroom_speaker", "crate" */
    bool v1_settings; /* the lib/settings keys (app_settings) are part of the store */
    const char *const *v1_keys; /* only these v1 keys (NULL-terminated), or NULL for all */
    server_group_t groups[SERVER_MAX_GROUPS];
    size_t group_count;
    /* Write the whole status document (status_json_<node>(c, ...)); return
     * its length, 0 if it could not be built. */
    size_t (*build_status)(const status_common_t *c, char *out, size_t out_size);
    bool (*on_cmd)(const char *name, const char *payload, size_t len); /* may be NULL */
    void (*on_connect)(void);                                          /* may be NULL */
    void (*on_settings)(const char *group);                            /* may be NULL */
} server_app_t;

/* app must stay valid. */
void server_link_start(const server_app_t *app);

bool server_link_connected(void);

/* Something in the status changed: publish soon (at most every 2 s). */
void server_link_status_changed(void);

/* Publish an event document as it is. false if not connected. */
bool server_link_event(const char *json);

/* {"type":"cmd","name":..,"ok":..,"why":..} (why may be NULL). */
void server_link_cmd_result(const char *name, bool ok, const char *why);

/* Keep a record until the server acknowledges it (id != 0, unique). */
bool server_link_record(uint32_t id, const char *json);

/* A setting changed on the device (console): publish reported. */
void server_link_settings_changed_locally(void);

/* Scene list from hue_task (hue_task_set_scene_sink). */
void server_link_hue_scenes(const char *json);

/* UTC seconds, or -1 while the clock is not set (for event "t"). */
int64_t server_link_utc(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_LINK_H */
