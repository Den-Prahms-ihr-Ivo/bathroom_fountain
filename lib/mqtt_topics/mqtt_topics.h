/*
 * mqtt_topics – build and recognise the MQTT topics of one device (pure C).
 *
 * Requirements: SRV-03, SRV-04 (see the requirements doc).
 * Tests:        test/test_mqtt_topics/
 *
 * Every topic of a device starts with "shower/<device_id>/". The broker's
 * access list gives each device account exactly that prefix, so a device can
 * neither read nor write another device's topics. The full list with payloads
 * is in docs/PROTOCOL.md; the server repository uses the same strings.
 *
 *   kind                     topic                                 direction
 *   MQTT_TOPIC_STATUS        shower/<id>/status                    device -> server, retained
 *   MQTT_TOPIC_DESIRED       shower/<id>/settings/desired          server -> device, retained
 *   MQTT_TOPIC_REPORTED      shower/<id>/settings/reported         device -> server, retained
 *   MQTT_TOPIC_CMD           shower/<id>/cmd/<suffix>              server -> device
 *   MQTT_TOPIC_EVENT         shower/<id>/event                     device -> server
 *   MQTT_TOPIC_LOG           shower/<id>/log                       device -> server
 *   MQTT_TOPIC_HUE_SCENES    shower/<id>/hue/scenes                device -> server, retained
 *   MQTT_TOPIC_NODE          shower/<id>/node/<suffix>             device -> server, retained
 *
 * MQTT_TOPIC_CMD and MQTT_TOPIC_NODE need a suffix; the other kinds must be
 * given NULL. A suffix is 1-32 characters of a-z, 0-9, '-', '_' and '/'
 * (a command such as "cold/start", or the name of a gun or target that the
 * bedside node reports for). It must not start or end with '/' or contain
 * "//". For subscribing, the suffix "#" is also allowed with MQTT_TOPIC_CMD.
 *
 * A device id is 1-24 characters of a-z, 0-9 and '-' (the same rule as the
 * setting device_id).
 */
#ifndef MQTT_TOPICS_H
#define MQTT_TOPICS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_TOPIC_MAX 80 /* a buffer of this size holds every topic */

typedef enum {
    MQTT_TOPIC_STATUS = 0,
    MQTT_TOPIC_DESIRED,
    MQTT_TOPIC_REPORTED,
    MQTT_TOPIC_CMD,
    MQTT_TOPIC_EVENT,
    MQTT_TOPIC_LOG,
    MQTT_TOPIC_HUE_SCENES,
    MQTT_TOPIC_NODE,
} mqtt_topic_t;

bool mqtt_device_id_valid(const char *device_id);

/* Write the topic into out. Returns false and leaves out empty (if
 * out_size > 0) for an invalid device id, kind or suffix, or if out is too
 * small. */
bool mqtt_topic_build(char *out, size_t out_size, const char *device_id, mqtt_topic_t kind, const char *suffix);

/*
 * Recognise an incoming topic. topic need not be NUL-terminated (MQTT hands
 * over pointer and length). On success *kind is set and *suffix points into
 * topic at the suffix (CMD, NODE) with *suffix_len its length, or is NULL
 * with length 0 for the other kinds. Returns false, without writing
 * anything, if the topic belongs to another device, is not in the list above
 * or carries an invalid suffix ("#" is not valid here).
 */
bool mqtt_topic_parse(const char *topic, size_t topic_len, const char *device_id, mqtt_topic_t *kind,
                      const char **suffix, size_t *suffix_len);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_TOPICS_H */
