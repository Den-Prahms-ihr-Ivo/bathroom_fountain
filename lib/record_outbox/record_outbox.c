/*
 * record_outbox – records kept until the server confirms them.
 * Contract in record_outbox.h, tests in test/test_record_outbox/.
 */
#include "record_outbox.h"

#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE 4u
#define ITEM_HEADER 6u
#define BLOB_VERSION 1u

typedef struct {
    uint32_t id;
    uint16_t len;
    bool sent;
    uint32_t sent_ms;
    uint8_t *data; /* item_max bytes */
} item_t;

struct record_outbox {
    record_outbox_config_t cfg;
    bool connected;
    size_t count;
    item_t *items;  /* oldest first */
    uint8_t *pool;  /* capacity * item_max */
};

void record_outbox_config_default(record_outbox_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->capacity = 8;
        cfg->item_max = 256;
        cfg->retry_ms = 60000;
    }
}

record_outbox_t *record_outbox_create(const record_outbox_config_t *cfg)
{
    if (cfg == NULL || cfg->capacity < 1 || cfg->capacity > 16 || cfg->item_max < 16 || cfg->item_max > 512 ||
        cfg->retry_ms < 1000 || cfg->retry_ms > 3600000u) {
        return NULL;
    }
    record_outbox_t *ob = calloc(1, sizeof(*ob));
    if (ob == NULL) {
        return NULL;
    }
    ob->cfg = *cfg;
    ob->items = calloc(cfg->capacity, sizeof(item_t));
    ob->pool = malloc((size_t)cfg->capacity * cfg->item_max);
    if (ob->items == NULL || ob->pool == NULL) {
        record_outbox_destroy(ob);
        return NULL;
    }
    return ob;
}

void record_outbox_destroy(record_outbox_t *ob)
{
    if (ob != NULL) {
        free(ob->items);
        free(ob->pool);
        free(ob);
    }
}

static int find(const record_outbox_t *ob, uint32_t id)
{
    for (size_t i = 0; i < ob->count; i++) {
        if (ob->items[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/* Remove item i, keeping the order; its data buffer moves to the free end. */
static void remove_at(record_outbox_t *ob, size_t i)
{
    item_t gone = ob->items[i];
    memmove(&ob->items[i], &ob->items[i + 1], (ob->count - i - 1) * sizeof(item_t));
    ob->count--;
    ob->items[ob->count] = gone; /* keeps the buffer for reuse */
}

/* Data buffer for slot count (the next free slot). */
static uint8_t *free_buffer(record_outbox_t *ob)
{
    item_t *slot = &ob->items[ob->count];
    if (slot->data == NULL) {
        /* hand out pool buffers that no item uses yet */
        for (size_t b = 0; b < ob->cfg.capacity; b++) {
            uint8_t *candidate = ob->pool + b * ob->cfg.item_max;
            bool used = false;
            for (size_t i = 0; i < ob->cfg.capacity; i++) {
                if (i != ob->count && ob->items[i].data == candidate) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                slot->data = candidate;
                break;
            }
        }
    }
    return slot->data;
}

static void append(record_outbox_t *ob, uint32_t id, const void *data, size_t len)
{
    uint8_t *buf = free_buffer(ob);
    memcpy(buf, data, len);
    item_t *it = &ob->items[ob->count++];
    it->id = id;
    it->len = (uint16_t)len;
    it->sent = false;
    it->sent_ms = 0;
    it->data = buf;
}

bool record_outbox_push(record_outbox_t *ob, uint32_t id, const void *data, size_t len, uint32_t *dropped_id)
{
    if (dropped_id != NULL) {
        *dropped_id = 0;
    }
    if (ob == NULL || id == 0 || data == NULL || len == 0 || len > ob->cfg.item_max || find(ob, id) >= 0) {
        return false;
    }
    if (ob->count == ob->cfg.capacity) {
        if (dropped_id != NULL) {
            *dropped_id = ob->items[0].id;
        }
        remove_at(ob, 0);
    }
    append(ob, id, data, len);
    return true;
}

bool record_outbox_ack(record_outbox_t *ob, uint32_t id)
{
    if (ob == NULL || id == 0) {
        return false;
    }
    const int i = find(ob, id);
    if (i < 0) {
        return false;
    }
    remove_at(ob, (size_t)i);
    return true;
}

void record_outbox_set_connected(record_outbox_t *ob, bool connected, uint32_t now_ms)
{
    (void)now_ms;
    if (ob == NULL) {
        return;
    }
    if (connected && !ob->connected) {
        for (size_t i = 0; i < ob->count; i++) {
            ob->items[i].sent = false;
        }
    }
    ob->connected = connected;
}

size_t record_outbox_take_due(record_outbox_t *ob, uint32_t now_ms, uint32_t *id, void *out, size_t out_size)
{
    if (ob == NULL || !ob->connected || out == NULL) {
        return 0;
    }
    for (size_t i = 0; i < ob->count; i++) {
        item_t *it = &ob->items[i];
        const bool due = !it->sent || (uint32_t)(now_ms - it->sent_ms) >= ob->cfg.retry_ms;
        if (!due) {
            continue;
        }
        if (it->len > out_size) {
            return 0;
        }
        memcpy(out, it->data, it->len);
        if (id != NULL) {
            *id = it->id;
        }
        it->sent = true;
        it->sent_ms = now_ms;
        return it->len;
    }
    return 0;
}

size_t record_outbox_count(const record_outbox_t *ob)
{
    return ob ? ob->count : 0;
}

size_t record_outbox_saved_size(const record_outbox_t *ob)
{
    if (ob == NULL) {
        return 0;
    }
    size_t n = HEADER_SIZE;
    for (size_t i = 0; i < ob->count; i++) {
        n += ITEM_HEADER + ob->items[i].len;
    }
    return n;
}

size_t record_outbox_save(const record_outbox_t *ob, void *out, size_t out_size)
{
    const size_t need = record_outbox_saved_size(ob);
    if (ob == NULL || out == NULL || out_size < need) {
        return 0;
    }
    uint8_t *p = out;
    *p++ = 'R';
    *p++ = 'O';
    *p++ = BLOB_VERSION;
    *p++ = (uint8_t)ob->count;
    for (size_t i = 0; i < ob->count; i++) {
        const item_t *it = &ob->items[i];
        for (int b = 0; b < 4; b++) {
            *p++ = (uint8_t)(it->id >> (8 * b));
        }
        *p++ = (uint8_t)(it->len & 0xFF);
        *p++ = (uint8_t)(it->len >> 8);
        memcpy(p, it->data, it->len);
        p += it->len;
    }
    return need;
}

bool record_outbox_load(record_outbox_t *ob, const void *blob, size_t len)
{
    if (ob == NULL || blob == NULL || len < HEADER_SIZE) {
        return false;
    }
    const uint8_t *b = blob;
    if (b[0] != 'R' || b[1] != 'O' || b[2] != BLOB_VERSION || b[3] > ob->cfg.capacity) {
        return false;
    }
    const size_t count = b[3];
    /* pass 1: check everything */
    uint32_t ids[16];
    size_t pos = HEADER_SIZE;
    for (size_t i = 0; i < count; i++) {
        if (len - pos < ITEM_HEADER) {
            return false;
        }
        const uint32_t id = (uint32_t)b[pos] | ((uint32_t)b[pos + 1] << 8) | ((uint32_t)b[pos + 2] << 16) |
                            ((uint32_t)b[pos + 3] << 24);
        const size_t n = (size_t)b[pos + 4] | ((size_t)b[pos + 5] << 8);
        pos += ITEM_HEADER;
        if (id == 0 || n == 0 || n > ob->cfg.item_max || len - pos < n) {
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (ids[j] == id) {
                return false;
            }
        }
        ids[i] = id;
        pos += n;
    }
    if (pos != len) {
        return false;
    }
    /* pass 2: replace the content */
    while (ob->count > 0) {
        remove_at(ob, ob->count - 1);
    }
    pos = HEADER_SIZE;
    for (size_t i = 0; i < count; i++) {
        const size_t n = (size_t)b[pos + 4] | ((size_t)b[pos + 5] << 8);
        append(ob, ids[i], b + pos + ITEM_HEADER, n);
        pos += ITEM_HEADER + n;
    }
    return true;
}
