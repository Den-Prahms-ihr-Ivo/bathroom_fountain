/*
 * record_outbox – records that must reach the server's database, kept until
 * the server confirms them, also across a reboot (pure C).
 *
 * Requirements: COLD-18, COLD-19 (see the requirements doc).
 * Tests:        test/test_record_outbox/
 *
 * MQTT's own acknowledgement only says the broker got a message, not that the
 * backend stored it, and the device may be offline when a cold shower ends.
 * So every record (today: cold_session; later anything else worth keeping)
 * goes in here. The platform sends what take_due() hands out as an event,
 * the backend stores it and answers on cmd/ack with the record's id
 * (docs/PROTOCOL.md), and ack() removes it. After every push and ack the
 * platform writes save() to NVS; at boot it calls load().
 *
 * Records are opaque bytes (the JSON text) with a 32-bit id. Ids must be
 * non-zero and unique per device; the app takes them from the hardware
 * random generator (esp_random), so an erased NVS cannot lead to an id the
 * server has seen before.
 *
 * Sending
 *   Nothing is due while disconnected. set_connected(true) makes every record
 *   due at once. A record that was handed out is due again cfg.retry_ms
 *   later if no ack came. take_due() hands out one due record per call, the
 *   oldest first, and marks it sent at now_ms.
 *
 * Space
 *   At most cfg.capacity records of at most cfg.item_max bytes. A push into a
 *   full outbox drops the oldest record and reports its id.
 *
 * Saved form (save / load), all integers little-endian:
 *   'R' 'O' 1 (version)  count (u8)  then per record: id (u32), len (u16), bytes
 *   load() accepts exactly that, with count <= capacity and every len
 *   <= item_max and the bytes adding up to the length given; anything else is
 *   refused and leaves the outbox as it was. Loaded records are not yet sent.
 *
 * Time is a wrapping uint32 millisecond counter.
 */
#ifndef RECORD_OUTBOX_H
#define RECORD_OUTBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t capacity;  /* 1 ... 16                  default 8     */
    uint16_t item_max; /* 16 ... 512 bytes          default 256   */
    uint32_t retry_ms; /* 1000 ... 3600000          default 60000 */
} record_outbox_config_t;

typedef struct record_outbox record_outbox_t;

void record_outbox_config_default(record_outbox_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is out of range. */
record_outbox_t *record_outbox_create(const record_outbox_config_t *cfg);

/* Accepts NULL. */
void record_outbox_destroy(record_outbox_t *ob);

/* Returns false (nothing changes) if id is 0 or already queued, data is NULL,
 * or len is 0 or above item_max. *dropped_id (may be NULL) is the id of the
 * record that had to go, or 0. */
bool record_outbox_push(record_outbox_t *ob, uint32_t id, const void *data, size_t len, uint32_t *dropped_id);

/* Returns true if a record with that id was removed. */
bool record_outbox_ack(record_outbox_t *ob, uint32_t id);

void record_outbox_set_connected(record_outbox_t *ob, bool connected, uint32_t now_ms);

/* Copy the oldest due record to out and return its length; *id gets its id.
 * 0 if nothing is due or out is too small (the record then stays due). */
size_t record_outbox_take_due(record_outbox_t *ob, uint32_t now_ms, uint32_t *id, void *out, size_t out_size);

size_t record_outbox_count(const record_outbox_t *ob);

/* Size save() needs for the current content. */
size_t record_outbox_saved_size(const record_outbox_t *ob);

/* Returns the number of bytes written, or 0 if out is too small. */
size_t record_outbox_save(const record_outbox_t *ob, void *out, size_t out_size);

bool record_outbox_load(record_outbox_t *ob, const void *blob, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RECORD_OUTBOX_H */
