/*
 * hue_api – Hue Bridge requests and responses as data.
 *
 * The contract is in hue_api.h, the tests in test/test_hue_api/.
 * JSON parsing uses the vendored cJSON in lib/cjson.
 */
#include "hue_api.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

#define SCENE_ID_LEN          36
#define DURATION_MAX_MS       60000u
#define INSTANCE_MAX_LEN      19
#define LINK_BUTTON_ERR_TYPE  101
#define RETRY_BASE_MS         500u
#define RETRY_MAX_MS          30000u

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* 8-4-4-4-12 hex digits with hyphens. The strict shape also rules out
 * anything that could change the request path. */
static bool scene_id_valid(const char *id)
{
    for (size_t i = 0; i < SCENE_ID_LEN; i++) {
        const bool hyphen_pos = (i == 8 || i == 13 || i == 18 || i == 23);
        if (hyphen_pos ? (id[i] != '-') : !is_hex(id[i])) {
            return false; /* also stops at an early NUL */
        }
    }
    return id[SCENE_ID_LEN] == '\0';
}

static bool instance_valid(const char *s)
{
    size_t n = 0;
    for (; s[n] != '\0'; n++) {
        const char c = s[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                     || c == '_' || c == '-';
        if (!ok || n >= INSTANCE_MAX_LEN) {
            return false;
        }
    }
    return n > 0;
}

/* snprintf into a fixed buffer; false if it did not fit. */
static bool format_fits(int written, size_t size)
{
    return written >= 0 && (size_t)written < size;
}

bool hue_build_scene_recall(const char *scene_id, uint32_t duration_ms, hue_request_t *out)
{
    if (scene_id == NULL || out == NULL || !scene_id_valid(scene_id) || duration_ms > DURATION_MAX_MS) {
        return false;
    }
    strcpy(out->method, "PUT");
    return format_fits(snprintf(out->path, sizeof(out->path), "/clip/v2/resource/scene/%s", scene_id),
                       sizeof(out->path))
        && format_fits(snprintf(out->body, sizeof(out->body),
                                "{\"recall\":{\"action\":\"active\",\"duration\":%lu}}",
                                (unsigned long)duration_ms),
                       sizeof(out->body));
}

bool hue_build_pairing(const char *instance, hue_request_t *out)
{
    if (instance == NULL || out == NULL || !instance_valid(instance)) {
        return false;
    }
    strcpy(out->method, "POST");
    strcpy(out->path, "/api");
    return format_fits(snprintf(out->body, sizeof(out->body),
                                "{\"devicetype\":\"bathroom_ambience#%s\",\"generateclientkey\":true}",
                                instance),
                       sizeof(out->body));
}

/* Mapping for everything except 2xx (which needs the body). */
static hue_result_t status_result(int http_status)
{
    if (http_status <= 0) {
        return HUE_ERR_TRANSPORT;
    }
    if (http_status == 401 || http_status == 403) {
        return HUE_ERR_AUTH;
    }
    if (http_status == 404) {
        return HUE_ERR_NOT_FOUND;
    }
    if (http_status == 429) {
        return HUE_ERR_RATE_LIMITED;
    }
    if (http_status >= 500 && http_status <= 599) {
        return HUE_ERR_BRIDGE_BUSY;
    }
    return HUE_ERR_REJECTED;
}

static bool is_2xx(int http_status)
{
    return http_status >= 200 && http_status <= 299;
}

/*
 * Parse exactly body[0 … body_len). Returns NULL unless the whole range is one
 * complete JSON value, optionally surrounded by whitespace. Free with cJSON_Delete.
 */
static cJSON *parse_json(const char *body, size_t body_len)
{
    if (body == NULL || body_len == 0) {
        return NULL;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, body_len, &end, false);
    if (root == NULL) {
        return NULL;
    }
    for (const char *p = end; p < body + body_len; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

hue_result_t hue_parse_response(int http_status, const char *body, size_t body_len)
{
    if (!is_2xx(http_status)) {
        return status_result(http_status);
    }

    cJSON *root = parse_json(body, body_len);
    hue_result_t result = HUE_ERR_BAD_RESPONSE;

    const cJSON *errors = cJSON_GetObjectItemCaseSensitive(root, "errors");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(root) && cJSON_IsArray(errors)) {
        if (cJSON_GetArraySize(errors) > 0) {
            result = HUE_ERR_REJECTED;
        } else if (cJSON_IsArray(data)) {
            result = HUE_OK;
        }
    }

    cJSON_Delete(root);
    return result;
}

hue_result_t hue_parse_pairing(int http_status, const char *body, size_t body_len,
                               char *key_out, size_t key_out_size)
{
    if (!is_2xx(http_status)) {
        return status_result(http_status);
    }

    cJSON *root = parse_json(body, body_len);
    hue_result_t result = HUE_ERR_BAD_RESPONSE;

    const cJSON *item = (cJSON_IsArray(root) && cJSON_GetArraySize(root) == 1)
                      ? cJSON_GetArrayItem(root, 0) : NULL;
    const cJSON *success = cJSON_GetObjectItemCaseSensitive(item, "success");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(item, "error");

    if (cJSON_IsObject(success)) {
        const cJSON *username = cJSON_GetObjectItemCaseSensitive(success, "username");
        if (cJSON_IsString(username) && key_out != NULL) {
            const size_t len = strlen(username->valuestring);
            if (len > 0 && len < key_out_size) {
                memcpy(key_out, username->valuestring, len + 1);
                result = HUE_OK;
            }
        }
    } else if (cJSON_IsObject(error)) {
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(error, "type");
        result = (cJSON_IsNumber(type) && type->valueint == LINK_BUTTON_ERR_TYPE)
               ? HUE_ERR_LINK_BUTTON : HUE_ERR_REJECTED;
    }

    cJSON_Delete(root);
    return result;
}

bool hue_result_is_retryable(hue_result_t r)
{
    switch (r) {
    case HUE_ERR_TRANSPORT:
    case HUE_ERR_RATE_LIMITED:
    case HUE_ERR_BRIDGE_BUSY:
    case HUE_ERR_LINK_BUTTON:
        return true;
    default:
        return false;
    }
}

uint32_t hue_retry_delay_ms(uint32_t attempt)
{
    if (attempt == 0) {
        return 0;
    }
    /* Double until the cap instead of shifting: large shifts are undefined. */
    uint32_t delay = RETRY_BASE_MS;
    for (uint32_t a = 1; a < attempt && delay < RETRY_MAX_MS; a++) {
        delay *= 2u;
    }
    return (delay < RETRY_MAX_MS) ? delay : RETRY_MAX_MS;
}
