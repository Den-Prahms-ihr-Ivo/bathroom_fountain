/*
 * mqtt_link – the device's MQTT connection to the server (docs/PROTOCOL.md,
 * MQTT). esp-mqtt over TLS (certificate bundle), credentials from the net
 * group (enroll_task_net()).
 *
 *   connect    client id = device_id, user / pass = mqtt_user / mqtt_pass,
 *              mqtts://mqtt_host:mqtt_port, keepalive 30 s;
 *              last will shower/<id>/status = {"online":false,"node":..},
 *              retained, QoS 1.
 *   on connect subscribe shower/<id>/cmd/# and shower/<id>/settings/desired;
 *              publish every group's schema (settings/schema/<group>,
 *              retained); call hooks->on_connect (the app publishes its
 *              status and reported settings there).
 *   commands   "forget" goes to enroll_task_forget(); "restart" reboots;
 *              everything else to hooks->on_cmd(name, payload, len).
 *   desired    hooks->on_desired(payload, len) (lib/settings_sync).
 *   auth       CONNACK "not authorised" / "bad user name or password" ->
 *              enroll_task_auth_result(false); a successful connect -> true.
 *
 * Callbacks run in the esp-mqtt task: keep them short (copy, post to a
 * queue). Without credentials the link stays idle until mqtt_link_restart().
 */
#ifndef MQTT_LINK_H
#define MQTT_LINK_H

#include <stdbool.h>
#include <stddef.h>

#include "kv_table.h"
#include "mqtt_topics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_LINK_MAX_GROUPS 4

typedef struct {
    const char *node;                               /* "bath", ...            */
    const kv_table_t *groups[MQTT_LINK_MAX_GROUPS]; /* schemas to publish     */
    size_t group_count;
    void (*on_connect)(void);
    void (*on_cmd)(const char *name, const char *payload, size_t len); /* name NUL-terminated */
    void (*on_desired)(const char *payload, size_t len);
} mqtt_link_hooks_t;

/* Call after enroll_task_start(). hooks must stay valid. */
void mqtt_link_start(const mqtt_link_hooks_t *hooks);

/* New credentials were saved: reconnect with them. */
void mqtt_link_restart(void);

/* Credentials were erased: disconnect and stay idle. */
void mqtt_link_stop(void);

bool mqtt_link_connected(void);

/* Publish to shower/<id>/<kind>[/suffix]. QoS and retain follow the topic
 * table: STATUS, REPORTED, HUE_SCENES, NODE retained QoS 1; EVENT QoS 1;
 * LOG QoS 0. false if not connected or the topic is invalid. */
bool mqtt_link_publish(mqtt_topic_t kind, const char *suffix, const char *payload);

/* Publish a retained message and wait (up to timeout_ms) until the broker
 * has it – for the crate's asleep status right before deep sleep. */
bool mqtt_link_publish_and_wait(mqtt_topic_t kind, const char *payload, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_LINK_H */
