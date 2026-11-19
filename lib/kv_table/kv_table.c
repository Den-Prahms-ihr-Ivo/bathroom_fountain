/*
 * kv_table – text key/value access to a struct, driven by a table.
 * Contract in kv_table.h, tests in test/test_kv_table/.
 */
#include "kv_table.h"

#include <stdio.h>
#include <string.h>

#define NAME_MAX_LEN 31u

static bool name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (const char *p = name; *p; p++, n++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return n <= NAME_MAX_LEN;
}

/* Find the choice called value in "a|b|c"; returns its index or -1. */
static int choice_index(const char *choices, const char *value)
{
    if (choices == NULL || value == NULL) {
        return -1;
    }
    const size_t vlen = strlen(value);
    int index = 0;
    const char *start = choices;
    for (;;) {
        const char *bar = strchr(start, '|');
        const size_t len = bar ? (size_t)(bar - start) : strlen(start);
        if (len > 0 && len == vlen && memcmp(start, value, len) == 0) {
            return index;
        }
        if (bar == NULL) {
            return -1;
        }
        start = bar + 1;
        index++;
    }
}

/* Copy the name of choice index into out; false if there is no such choice. */
static bool choice_name(const char *choices, unsigned index, char *out, size_t out_size)
{
    if (choices == NULL) {
        return false;
    }
    const char *start = choices;
    for (unsigned i = 0;; i++) {
        const char *bar = strchr(start, '|');
        const size_t len = bar ? (size_t)(bar - start) : strlen(start);
        if (i == index) {
            if (len + 1 > out_size) {
                return false;
            }
            memcpy(out, start, len);
            out[len] = '\0';
            return true;
        }
        if (bar == NULL) {
            return false;
        }
        start = bar + 1;
    }
}

static bool choices_ok(const char *choices)
{
    if (choices == NULL || choices[0] == '\0') {
        return false;
    }
    unsigned count = 0;
    const char *start = choices;
    for (;;) {
        const char *bar = strchr(start, '|');
        const size_t len = bar ? (size_t)(bar - start) : strlen(start);
        if (len == 0) {
            return false;
        }
        /* duplicate: an earlier name with the same text */
        const char *prev = choices;
        while (prev < start) {
            const char *pbar = strchr(prev, '|');
            const size_t plen = (size_t)(pbar - prev);
            if (plen == len && memcmp(prev, start, len) == 0) {
                return false;
            }
            prev = pbar + 1;
        }
        count++;
        if (count > 255) {
            return false;
        }
        if (bar == NULL) {
            return true;
        }
        start = bar + 1;
    }
}

static bool printable(const char *v)
{
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        if (*p < 0x20 || *p > 0x7E) {
            return false;
        }
    }
    return true;
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool uuid_ok(const char *v)
{
    if (v[0] == '\0') {
        return true;
    }
    if (strlen(v) != 36) {
        return false;
    }
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (v[i] != '-') {
                return false;
            }
        } else if (!is_hex(v[i])) {
            return false;
        }
    }
    return true;
}

static uint32_t uint_field_max(uint16_t size)
{
    switch (size) {
    case 1:
        return 0xFFu;
    case 2:
        return 0xFFFFu;
    default:
        return 0xFFFFFFFFu;
    }
}

/* Check value against key without storing. For uint the parsed number goes to
 * *num, for choice the index. */
static kv_result_t parse(const kv_key_t *k, const char *value, uint32_t *num)
{
    switch (k->type) {
    case KV_UINT: {
        const size_t len = strlen(value);
        if (len == 0 || len > 10) {
            return KV_ERR_INVALID;
        }
        uint64_t n = 0;
        for (size_t i = 0; i < len; i++) {
            if (value[i] < '0' || value[i] > '9') {
                return KV_ERR_INVALID;
            }
            n = n * 10u + (uint64_t)(value[i] - '0');
        }
        if (n > 0xFFFFFFFFull || n < k->min || n > k->max) {
            return KV_ERR_RANGE;
        }
        *num = (uint32_t)n;
        return KV_OK;
    }
    case KV_BOOL:
        if (strcmp(value, "on") == 0) {
            *num = 1;
        } else if (strcmp(value, "off") == 0) {
            *num = 0;
        } else {
            return KV_ERR_INVALID;
        }
        return KV_OK;
    case KV_CHOICE: {
        const int idx = choice_index(k->choices, value);
        if (idx < 0) {
            return KV_ERR_INVALID;
        }
        *num = (uint32_t)idx;
        return KV_OK;
    }
    case KV_TEXT:
    case KV_SECRET: {
        if (value[0] == '\0') {
            return KV_OK;
        }
        if (!printable(value)) {
            return KV_ERR_INVALID;
        }
        if (k->check != NULL && !k->check(value)) {
            return KV_ERR_INVALID;
        }
        const size_t len = strlen(value);
        if (len < k->min || len > k->max || len + 1 > k->size) {
            return KV_ERR_RANGE;
        }
        return KV_OK;
    }
    case KV_UUID:
        if (!uuid_ok(value) || strlen(value) + 1 > k->size) {
            return KV_ERR_INVALID;
        }
        return KV_OK;
    default:
        return KV_ERR_INVALID;
    }
}

static void store(const kv_key_t *k, void *data, const char *value, uint32_t num)
{
    uint8_t *field = (uint8_t *)data + k->offset;
    switch (k->type) {
    case KV_UINT:
        if (k->size == 1) {
            const uint8_t v = (uint8_t)num;
            memcpy(field, &v, 1);
        } else if (k->size == 2) {
            const uint16_t v = (uint16_t)num;
            memcpy(field, &v, 2);
        } else {
            memcpy(field, &num, 4);
        }
        break;
    case KV_BOOL: {
        const bool b = num != 0;
        memcpy(field, &b, sizeof(bool));
        break;
    }
    case KV_CHOICE:
        *field = (uint8_t)num;
        break;
    default: {
        const size_t len = strlen(value);
        memcpy(field, value, len + 1);
        break;
    }
    }
}

static const char *default_text(const kv_key_t *k)
{
    if (k->def != NULL) {
        return k->def;
    }
    switch (k->type) {
    case KV_UINT:
        return "0";
    case KV_BOOL:
        return "off";
    default:
        return "";
    }
}

static bool default_ok(const kv_key_t *k)
{
    uint32_t num = 0;
    if (k->type == KV_CHOICE && k->def == NULL) {
        return true; /* first choice */
    }
    return parse(k, default_text(k), &num) == KV_OK;
}

static bool key_ok(const kv_table_t *t, size_t i)
{
    const kv_key_t *k = &t->keys[i];
    if (!name_ok(k->name)) {
        return false;
    }
    for (size_t j = 0; j < i; j++) {
        if (strcmp(t->keys[j].name, k->name) == 0) {
            return false;
        }
    }
    if ((size_t)k->offset + k->size > t->data_size || k->size == 0) {
        return false;
    }
    switch (k->type) {
    case KV_UINT:
        if (k->size != 1 && k->size != 2 && k->size != 4) {
            return false;
        }
        if (k->min > k->max || k->max > uint_field_max(k->size)) {
            return false;
        }
        break;
    case KV_BOOL:
        if (k->size != sizeof(bool)) {
            return false;
        }
        break;
    case KV_CHOICE:
        if (k->size != 1 || !choices_ok(k->choices)) {
            return false;
        }
        break;
    case KV_TEXT:
    case KV_SECRET:
        if (k->size < 2 || k->min > k->max || k->max > (uint32_t)k->size - 1u) {
            return false;
        }
        break;
    case KV_UUID:
        if (k->size < 37) {
            return false;
        }
        break;
    default:
        return false;
    }
    return default_ok(k);
}

bool kv_table_valid(const kv_table_t *table)
{
    if (table == NULL || table->keys == NULL || table->count == 0) {
        return false;
    }
    for (size_t i = 0; i < table->count; i++) {
        if (!key_ok(table, i)) {
            return false;
        }
    }
    return true;
}

void kv_defaults(const kv_table_t *table, void *data)
{
    if (table == NULL || data == NULL) {
        return;
    }
    for (size_t i = 0; i < table->count; i++) {
        const kv_key_t *k = &table->keys[i];
        uint32_t num = 0;
        const char *text = default_text(k);
        if (k->type == KV_CHOICE && k->def == NULL) {
            store(k, data, text, 0);
        } else if (parse(k, text, &num) == KV_OK) {
            store(k, data, text, num);
        } else {
            /* invalid table: still leave the field in a defined state */
            store(k, data, "", 0);
        }
    }
}

int kv_find(const kv_table_t *table, const char *key)
{
    if (table == NULL || key == NULL || table->keys == NULL) {
        return -1;
    }
    for (size_t i = 0; i < table->count; i++) {
        if (table->keys[i].name != NULL && strcmp(table->keys[i].name, key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

kv_result_t kv_set(const kv_table_t *table, void *data, const char *key, const char *value)
{
    if (table == NULL || data == NULL || key == NULL || value == NULL) {
        return KV_ERR_INVALID;
    }
    const int idx = kv_find(table, key);
    if (idx < 0) {
        return KV_ERR_UNKNOWN_KEY;
    }
    const kv_key_t *k = &table->keys[idx];
    uint32_t num = 0;
    const kv_result_t r = parse(k, value, &num);
    if (r != KV_OK) {
        return r;
    }
    store(k, data, value, num);
    return KV_OK;
}

static bool put(char *out, size_t out_size, const char *text)
{
    const size_t len = strlen(text);
    if (len + 1 > out_size) {
        out[0] = '\0';
        return false;
    }
    memcpy(out, text, len + 1);
    return true;
}

bool kv_format(const kv_table_t *table, const void *data, const char *key, bool reveal_secrets, char *out,
               size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (data == NULL) {
        return false;
    }
    const int idx = kv_find(table, key);
    if (idx < 0) {
        return false;
    }
    const kv_key_t *k = &table->keys[idx];
    const uint8_t *field = (const uint8_t *)data + k->offset;
    char buf[16];
    switch (k->type) {
    case KV_UINT: {
        uint32_t v = 0;
        if (k->size == 1) {
            v = field[0];
        } else if (k->size == 2) {
            uint16_t v16;
            memcpy(&v16, field, 2);
            v = v16;
        } else {
            memcpy(&v, field, 4);
        }
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)v);
        return put(out, out_size, buf);
    }
    case KV_BOOL: {
        bool b;
        memcpy(&b, field, sizeof(bool));
        return put(out, out_size, b ? "on" : "off");
    }
    case KV_CHOICE:
        if (!choice_name(k->choices, field[0], out, out_size)) {
            out[0] = '\0';
            return false;
        }
        return true;
    case KV_SECRET:
        if (field[0] != '\0' && !reveal_secrets) {
            return put(out, out_size, "********");
        }
        /* fall through */
    default: {
        /* stored strings are NUL-terminated inside size */
        const char *s = (const char *)field;
        if (memchr(s, '\0', k->size) == NULL) {
            return false;
        }
        return put(out, out_size, s);
    }
    }
}
