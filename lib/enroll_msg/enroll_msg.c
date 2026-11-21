/*
 * enroll_msg – JSON of the pairing exchange.
 * Contract in enroll_msg.h, tests in test/test_enroll_msg/.
 */
#include "enroll_msg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "kv_table.h"
#include "node_config.h"

/* ---- validation ---------------------------------------------------------- */

static bool is_lower_hex(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return s[len] == '\0';
}

static bool chip_ok(const char *chip)
{
    return chip != NULL && strnlen(chip, ENROLL_CHIP_LEN + 1) == ENROLL_CHIP_LEN && is_lower_hex(chip, ENROLL_CHIP_LEN);
}

static bool secret_ok(const char *secret)
{
    return secret != NULL && strnlen(secret, ENROLL_SECRET_LEN + 1) == ENROLL_SECRET_LEN
           && is_lower_hex(secret, ENROLL_SECRET_LEN);
}

static bool node_ok(const char *node)
{
    if (node == NULL) {
        return false;
    }
    const size_t n = strnlen(node, 25);
    if (n < 1 || n > 24) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((node[i] >= 'a' && node[i] <= 'z') || node[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool fw_ok(const char *fw)
{
    if (fw == NULL) {
        return false;
    }
    const size_t n = strnlen(fw, 32);
    if (n < 1 || n > 31) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)fw[i] < 0x20 || (unsigned char)fw[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

/* ---- builders ------------------------------------------------------------ */

static void clear_out(char *out, size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
}

/* Prints obj unformatted into out and deletes it. 0 if it does not fit. */
static size_t print_into(cJSON *obj, char *out, size_t out_size)
{
    char *text = (obj != NULL) ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (text == NULL) {
        return 0;
    }
    const size_t len = strlen(text);
    if (len + 1 > out_size) {
        cJSON_free(text);
        return 0;
    }
    memcpy(out, text, len + 1);
    cJSON_free(text);
    return len;
}

size_t enroll_msg_hello(const char *chip, const char *node, const char *fw, const char *secret, char *out,
                        size_t out_size)
{
    clear_out(out, out_size);
    if (out == NULL || !chip_ok(chip) || !node_ok(node) || !fw_ok(fw) || !secret_ok(secret)) {
        return 0;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL || !cJSON_AddStringToObject(o, "chip", chip) || !cJSON_AddStringToObject(o, "node", node)
        || !cJSON_AddStringToObject(o, "fw", fw) || !cJSON_AddStringToObject(o, "secret", secret)) {
        cJSON_Delete(o);
        return 0;
    }
    return print_into(o, out, out_size);
}

size_t enroll_msg_confirm(const char *chip, const char *secret, char *out, size_t out_size)
{
    clear_out(out, out_size);
    if (out == NULL || !chip_ok(chip) || !secret_ok(secret)) {
        return 0;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL || !cJSON_AddStringToObject(o, "chip", chip) || !cJSON_AddStringToObject(o, "secret", secret)
        || !cJSON_AddTrueToObject(o, "confirm")) {
        cJSON_Delete(o);
        return 0;
    }
    return print_into(o, out, out_size);
}

bool enroll_msg_hex(const uint8_t *bytes, size_t n, char *out, size_t out_size)
{
    static const char digits[] = "0123456789abcdef";
    if (bytes == NULL || out == NULL || out_size < 2 * n + 1) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0x0F];
    }
    out[2 * n] = '\0';
    return true;
}

/* ---- parser -------------------------------------------------------------- */

/* An integral JSON number within [min, max]. */
static bool get_int(const cJSON *item, double min, double max, long *v)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double d = item->valuedouble;
    if (!(d >= min && d <= max) || floor(d) != d) {
        return false;
    }
    *v = (long)d;
    return true;
}

/* A non-empty string member that the "net" group accepts for key. */
static bool net_string(const cJSON *root, const char *key, cfg_net_t *net)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring != NULL && item->valuestring[0] != '\0'
           && kv_set(&cfg_net_table, net, key, item->valuestring) == KV_OK;
}

static bool parse_claimed(const cJSON *root, enroll_creds_t *creds)
{
    cfg_net_t net;
    memset(&net, 0, sizeof(net));
    if (!net_string(root, "device_id", &net) || !net_string(root, "mqtt_host", &net)
        || !net_string(root, "mqtt_user", &net) || !net_string(root, "mqtt_pass", &net)) {
        return false;
    }
    long port;
    char port_text[8];
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "mqtt_port"), 0, 65535, &port)) {
        return false;
    }
    snprintf(port_text, sizeof(port_text), "%ld", port);
    if (kv_set(&cfg_net_table, &net, "mqtt_port", port_text) != KV_OK) {
        return false;
    }

    _Static_assert(sizeof(creds->device_id) == sizeof(net.device_id), "device_id size");
    _Static_assert(sizeof(creds->mqtt_host) == sizeof(net.mqtt_host), "mqtt_host size");
    _Static_assert(sizeof(creds->mqtt_user) == sizeof(net.mqtt_user), "mqtt_user size");
    _Static_assert(sizeof(creds->mqtt_pass) == sizeof(net.mqtt_pass), "mqtt_pass size");
    memcpy(creds->device_id, net.device_id, sizeof(creds->device_id));
    memcpy(creds->mqtt_host, net.mqtt_host, sizeof(creds->mqtt_host));
    memcpy(creds->mqtt_user, net.mqtt_user, sizeof(creds->mqtt_user));
    memcpy(creds->mqtt_pass, net.mqtt_pass, sizeof(creds->mqtt_pass));
    creds->mqtt_port = net.mqtt_port;
    return true;
}

static bool parse_root(const cJSON *root, enroll_reply_t *r)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(state) || state->valuestring == NULL) {
        return false;
    }
    const char *s = state->valuestring;

    if (strcmp(s, "wait") == 0) {
        r->kind = ENROLL_REPLY_WAIT;
        return true;
    }
    if (strcmp(s, "confirm") == 0) {
        long w;
        if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "window_s"), ENROLL_WINDOW_MIN_S, ENROLL_WINDOW_MAX_S,
                     &w)) {
            return false;
        }
        r->kind = ENROLL_REPLY_CONFIRM;
        r->window_s = (uint16_t)w;
        return true;
    }
    if (strcmp(s, "claimed") == 0) {
        r->kind = ENROLL_REPLY_CLAIMED;
        return parse_claimed(root, &r->creds);
    }
    if (strcmp(s, "rejected") == 0) {
        r->kind = ENROLL_REPLY_REJECTED;
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        if (cJSON_IsString(reason) && reason->valuestring != NULL
            && strlen(reason->valuestring) < sizeof(r->reason)) {
            strcpy(r->reason, reason->valuestring);
        }
        return true;
    }
    return false;
}

bool enroll_msg_parse(const char *json, size_t len, enroll_reply_t *out)
{
    if (json == NULL || out == NULL || len == 0) {
        return false;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return false;
    }
    enroll_reply_t r;
    memset(&r, 0, sizeof(r));
    const bool ok = parse_root(root, &r);
    cJSON_Delete(root);
    if (ok) {
        *out = r;
    }
    return ok;
}
