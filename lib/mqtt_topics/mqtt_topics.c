/*
 * mqtt_topics – build and recognise the MQTT topics of one device.
 * Contract in mqtt_topics.h, tests in test/test_mqtt_topics/.
 */
#include "mqtt_topics.h"

#include <string.h>

#define PREFIX "shower/"
#define PREFIX_LEN 7u
#define DEVICE_ID_MAX 24u
#define SUFFIX_MAX 32u

typedef struct {
    const char *tail;  /* after "shower/<id>/" */
    bool has_suffix;   /* tail is followed by "/<suffix>" */
} kind_info_t;

static const kind_info_t KINDS[] = {
    [MQTT_TOPIC_STATUS] = {"status", false},
    [MQTT_TOPIC_DESIRED] = {"settings/desired", false},
    [MQTT_TOPIC_REPORTED] = {"settings/reported", false},
    [MQTT_TOPIC_CMD] = {"cmd", true},
    [MQTT_TOPIC_EVENT] = {"event", false},
    [MQTT_TOPIC_LOG] = {"log", false},
    [MQTT_TOPIC_HUE_SCENES] = {"hue/scenes", false},
    [MQTT_TOPIC_NODE] = {"node", true},
};
#define KIND_COUNT (sizeof(KINDS) / sizeof(KINDS[0]))

bool mqtt_device_id_valid(const char *device_id)
{
    if (device_id == NULL) {
        return false;
    }
    size_t n = 0;
    for (const char *p = device_id; *p; p++, n++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return n >= 1 && n <= DEVICE_ID_MAX;
}

static bool suffix_valid(const char *s, size_t len)
{
    if (len == 0 || len > SUFFIX_MAX || s[0] == '/' || s[len - 1] == '/') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = s[i];
        if (c == '/') {
            if (s[i + 1] == '/') {
                return false; /* i + 1 < len because s[len-1] != '/' */
            }
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

bool mqtt_topic_build(char *out, size_t out_size, const char *device_id, mqtt_topic_t kind, const char *suffix)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (!mqtt_device_id_valid(device_id) || (unsigned)kind >= KIND_COUNT) {
        return false;
    }
    const kind_info_t *k = &KINDS[kind];
    if (k->has_suffix) {
        if (suffix == NULL) {
            return false;
        }
        const bool wildcard = kind == MQTT_TOPIC_CMD && strcmp(suffix, "#") == 0;
        if (!wildcard && !suffix_valid(suffix, strlen(suffix))) {
            return false;
        }
    } else if (suffix != NULL) {
        return false;
    }

    const size_t id_len = strlen(device_id);
    const size_t tail_len = strlen(k->tail);
    const size_t suf_len = k->has_suffix ? strlen(suffix) : 0;
    const size_t total = PREFIX_LEN + id_len + 1 + tail_len + (k->has_suffix ? 1 + suf_len : 0);
    if (total + 1 > out_size) {
        return false;
    }
    char *p = out;
    memcpy(p, PREFIX, PREFIX_LEN);
    p += PREFIX_LEN;
    memcpy(p, device_id, id_len);
    p += id_len;
    *p++ = '/';
    memcpy(p, k->tail, tail_len);
    p += tail_len;
    if (k->has_suffix) {
        *p++ = '/';
        memcpy(p, suffix, suf_len);
        p += suf_len;
    }
    *p = '\0';
    return true;
}

bool mqtt_topic_parse(const char *topic, size_t topic_len, const char *device_id, mqtt_topic_t *kind,
                      const char **suffix, size_t *suffix_len)
{
    if (topic == NULL || kind == NULL || suffix == NULL || suffix_len == NULL || !mqtt_device_id_valid(device_id)) {
        return false;
    }
    const size_t id_len = strlen(device_id);
    const size_t head = PREFIX_LEN + id_len + 1;
    if (topic_len <= head || memcmp(topic, PREFIX, PREFIX_LEN) != 0 ||
        memcmp(topic + PREFIX_LEN, device_id, id_len) != 0 || topic[PREFIX_LEN + id_len] != '/') {
        return false;
    }
    const char *rest = topic + head;
    const size_t rest_len = topic_len - head;

    for (size_t i = 0; i < KIND_COUNT; i++) {
        const kind_info_t *k = &KINDS[i];
        const size_t tail_len = strlen(k->tail);
        if (rest_len < tail_len || memcmp(rest, k->tail, tail_len) != 0) {
            continue;
        }
        if (!k->has_suffix) {
            if (rest_len != tail_len) {
                continue;
            }
            *kind = (mqtt_topic_t)i;
            *suffix = NULL;
            *suffix_len = 0;
            return true;
        }
        if (rest_len < tail_len + 2 || rest[tail_len] != '/') {
            continue;
        }
        const char *s = rest + tail_len + 1;
        const size_t n = rest_len - tail_len - 1;
        if (!suffix_valid(s, n)) {
            return false;
        }
        *kind = (mqtt_topic_t)i;
        *suffix = s;
        *suffix_len = n;
        return true;
    }
    return false;
}
