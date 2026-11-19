/*
 * settings_sync – desired / reported settings as JSON.
 * Contract in settings_sync.h, tests in test/test_settings_sync/.
 */
#include "settings_sync.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define MASK "********"
#define REJECT_KEY_MAX 48 /* longer keys are cut in the rejected list */

typedef struct {
    char key[REJECT_KEY_MAX];
    sync_set_t reason;
} rejected_t;

struct settings_sync {
    const sync_store_t *store;
    uint32_t version;
    size_t rejected_count; /* entries in rejected[] */
    rejected_t rejected[SYNC_MAX_REJECTED];
};

settings_sync_t *settings_sync_create(const sync_store_t *store)
{
    if (store == NULL || store->count == NULL || store->name == NULL || store->is_secret == NULL ||
        store->set == NULL || store->format == NULL) {
        return NULL;
    }
    settings_sync_t *s = calloc(1, sizeof(*s));
    if (s != NULL) {
        s->store = store;
    }
    return s;
}

void settings_sync_destroy(settings_sync_t *sync)
{
    free(sync);
}

uint32_t settings_sync_version(const settings_sync_t *sync)
{
    return sync ? sync->version : 0;
}

void settings_sync_set_version(settings_sync_t *sync, uint32_t version)
{
    if (sync != NULL) {
        sync->version = version;
    }
}

static const char *reason_text(sync_set_t r)
{
    switch (r) {
    case SYNC_SET_UNKNOWN:
        return "unknown";
    case SYNC_SET_RANGE:
        return "range";
    case SYNC_SET_CONFLICT:
        return "conflict";
    case SYNC_SET_INVALID:
    default:
        return "invalid";
    }
}

static void add_rejected(settings_sync_t *s, const char *key, sync_set_t reason, sync_result_t *r)
{
    r->rejected++;
    if (s->rejected_count < SYNC_MAX_REJECTED) {
        rejected_t *e = &s->rejected[s->rejected_count++];
        strncpy(e->key, key ? key : "", sizeof(e->key) - 1);
        e->key[sizeof(e->key) - 1] = '\0';
        e->reason = reason;
    }
}

/* version: integer 1 ... 2147483647 */
static bool version_of(const cJSON *v, uint32_t *out)
{
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    const double d = v->valuedouble;
    if (!(d >= 1.0 && d <= 2147483647.0)) {
        return false;
    }
    const uint32_t n = (uint32_t)d;
    if ((double)n != d) {
        return false;
    }
    *out = n;
    return true;
}

sync_result_t settings_sync_apply(settings_sync_t *sync, const char *json, size_t len)
{
    sync_result_t r = {SYNC_ERR_FORMAT, 0, 0, 0};
    if (sync == NULL || json == NULL) {
        return r;
    }
    cJSON *doc = cJSON_ParseWithLength(json, len);
    if (doc == NULL) {
        return r;
    }
    uint32_t version;
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(doc, "values");
    if (!cJSON_IsObject(doc) || !version_of(cJSON_GetObjectItemCaseSensitive(doc, "version"), &version) ||
        !cJSON_IsObject(values)) {
        cJSON_Delete(doc);
        return r;
    }
    r.version = version;
    if (version <= sync->version) {
        r.status = SYNC_STALE;
        cJSON_Delete(doc);
        return r;
    }

    r.status = SYNC_APPLIED;
    sync->rejected_count = 0;
    const sync_store_t *st = sync->store;
    const cJSON *m;
    cJSON_ArrayForEach(m, values)
    {
        const char *key = m->string;
        if (!cJSON_IsString(m) || m->valuestring == NULL) {
            add_rejected(sync, key, SYNC_SET_INVALID, &r);
            continue;
        }
        const char *value = m->valuestring;
        char current[SYNC_VALUE_MAX];
        if (st->format(st->ctx, key, true, current, sizeof(current)) && strcmp(current, value) == 0) {
            continue;
        }
        if (strcmp(value, MASK) == 0 && st->is_secret(st->ctx, key)) {
            continue;
        }
        const sync_set_t res = st->set(st->ctx, key, value);
        if (res == SYNC_SET_OK) {
            r.changed++;
        } else {
            add_rejected(sync, key, res, &r);
        }
    }
    sync->version = version;
    cJSON_Delete(doc);
    return r;
}

int settings_sync_report(const settings_sync_t *sync, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (sync == NULL) {
        return -1;
    }
    const sync_store_t *st = sync->store;
    cJSON *doc = cJSON_CreateObject();
    cJSON *values = cJSON_CreateObject();
    cJSON *rejected = cJSON_CreateObject();
    bool ok = doc != NULL && values != NULL && rejected != NULL;
    if (ok) {
        ok = cJSON_AddNumberToObject(doc, "version", (double)sync->version) != NULL;
        cJSON_AddItemToObject(doc, "values", values);
        cJSON_AddItemToObject(doc, "rejected", rejected);
        values = rejected = NULL; /* owned by doc now */
    }
    if (ok) {
        cJSON *vals = cJSON_GetObjectItemCaseSensitive(doc, "values");
        const size_t n = st->count(st->ctx);
        for (size_t i = 0; ok && i < n; i++) {
            const char *key = st->name(st->ctx, i);
            if (key == NULL) {
                continue;
            }
            char text[SYNC_VALUE_MAX];
            if (!st->format(st->ctx, key, false, text, sizeof(text))) {
                text[0] = '\0';
            }
            ok = cJSON_AddStringToObject(vals, key, text) != NULL;
        }
        cJSON *rej = cJSON_GetObjectItemCaseSensitive(doc, "rejected");
        for (size_t i = 0; ok && i < sync->rejected_count; i++) {
            ok = cJSON_AddStringToObject(rej, sync->rejected[i].key, reason_text(sync->rejected[i].reason)) != NULL;
        }
    }
    int result = -1;
    if (ok) {
        char *text = cJSON_PrintUnformatted(doc);
        if (text != NULL) {
            const size_t len = strlen(text);
            if (len + 1 <= out_size) {
                memcpy(out, text, len + 1);
                result = (int)len;
            }
            free(text);
        }
    }
    cJSON_Delete(values);
    cJSON_Delete(rejected);
    cJSON_Delete(doc);
    return result;
}
