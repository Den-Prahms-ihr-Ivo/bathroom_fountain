/*
 * settings_schema – a settings group's description for the web app.
 * Contract in settings_schema.h, tests in test/test_settings_schema/.
 */
#include "settings_schema.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"

static const char *type_name(kv_type_t t)
{
    switch (t) {
    case KV_UINT:
        return "uint";
    case KV_BOOL:
        return "bool";
    case KV_CHOICE:
        return "choice";
    case KV_TEXT:
        return "text";
    case KV_SECRET:
        return "secret";
    case KV_UUID:
        return "uuid";
    default:
        return NULL;
    }
}

static bool ends_with(const char *s, const char *suffix)
{
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static const char *unit_of(const char *name)
{
    if (ends_with(name, "_ms")) {
        return "ms";
    }
    if (ends_with(name, "_s")) {
        return "s";
    }
    if (ends_with(name, "_min")) {
        return "min";
    }
    if (ends_with(name, "_level")) {
        return "%";
    }
    return NULL;
}

/* "easy|normal|hard" -> ["easy","normal","hard"] */
static bool add_choices(cJSON *obj, const char *choices)
{
    cJSON *arr = cJSON_AddArrayToObject(obj, "choices");
    if (arr == NULL) {
        return false;
    }
    char name[256];
    const char *p = choices;
    while (true) {
        const char *bar = strchr(p, '|');
        const size_t len = (bar != NULL) ? (size_t)(bar - p) : strlen(p);
        if (len >= sizeof(name)) {
            return false;
        }
        memcpy(name, p, len);
        name[len] = '\0';
        cJSON *s = cJSON_CreateString(name);
        if (s == NULL || !cJSON_AddItemToArray(arr, s)) {
            cJSON_Delete(s);
            return false;
        }
        if (bar == NULL) {
            return true;
        }
        p = bar + 1;
    }
}

static cJSON *key_object(const kv_key_t *k)
{
    cJSON *o = cJSON_CreateObject();
    const char *type = type_name(k->type);
    if (o == NULL || type == NULL || !cJSON_AddStringToObject(o, "key", k->name)
        || !cJSON_AddStringToObject(o, "type", type)) {
        cJSON_Delete(o);
        return NULL;
    }
    bool ok = true;
    if (k->type == KV_UINT || k->type == KV_TEXT || k->type == KV_SECRET) {
        ok = cJSON_AddNumberToObject(o, "min", (double)k->min) != NULL
             && cJSON_AddNumberToObject(o, "max", (double)k->max) != NULL;
    }
    if (ok && k->type == KV_CHOICE) {
        ok = k->choices != NULL && add_choices(o, k->choices);
    }
    const char *unit = unit_of(k->name);
    if (ok && unit != NULL) {
        ok = cJSON_AddStringToObject(o, "unit", unit) != NULL;
    }
    if (ok && k->def != NULL && k->type != KV_SECRET) {
        ok = cJSON_AddStringToObject(o, "default", k->def) != NULL;
    }
    if (!ok) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static cJSON *schema_object(const kv_table_t *table)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *keys = NULL;
    if (root == NULL || !cJSON_AddStringToObject(root, "group", table->name)
        || (keys = cJSON_AddArrayToObject(root, "keys")) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }
    for (size_t i = 0; i < table->count; i++) {
        cJSON *k = key_object(&table->keys[i]);
        if (k == NULL || !cJSON_AddItemToArray(keys, k)) {
            cJSON_Delete(k);
            cJSON_Delete(root);
            return NULL;
        }
    }
    return root;
}

size_t settings_schema_json(const kv_table_t *table, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (table == NULL || !kv_table_valid(table)) {
        return 0;
    }

    cJSON *root = schema_object(table);
    char *text = (root != NULL) ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) {
        return 0;
    }
    const size_t len = strlen(text);
    if (len > SETTINGS_SCHEMA_MAX || len + 1 > out_size) {
        cJSON_free(text);
        return 0;
    }
    memcpy(out, text, len + 1);
    cJSON_free(text);
    return len;
}
