/*
 * log_ring – the last log lines in RAM (pure C).
 *
 * Requirements: LOG-01 ... LOG-04 (see the requirements doc).
 * Tests:        test/test_log_ring/
 *
 * The platform hooks esp_log_set_vprintf(), formats each message into a
 * buffer and appends it here (under its own lock: this module is not
 * thread-safe). Two readers follow the ring by sequence number: the MQTT log
 * publisher and the fallback page.
 *
 * Lines
 *   log_ring_append() takes one log line. Before it is stored:
 *     - ANSI colour sequences are removed: ESC '[' ... up to and including
 *       the first letter (ESP-IDF wraps lines in "\033[0;32m" ... "\033[0m");
 *     - trailing '\r' and '\n' are removed;
 *     - other bytes below 0x20 become a space;
 *     - the result is cut to LOG_RING_LINE_MAX bytes.
 *   A line that is empty after this is not stored and returns 0.
 *
 * Sequence numbers
 *   The first stored line has sequence number 1, each following line one
 *   more, for the lifetime of the ring (they are never reused; no wrap
 *   handling is needed: 2^32 lines are years of logging).
 *
 * Space
 *   A stored line occupies its length plus LOG_RING_OVERHEAD bytes. Before a
 *   line is stored, the oldest lines are dropped until the new one fits into
 *   capacity_bytes. So the ring always holds the newest lines, and a reader
 *   that falls behind sees first_seq jump ahead of its cursor.
 */
#ifndef LOG_RING_H
#define LOG_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_RING_LINE_MAX 200
#define LOG_RING_OVERHEAD 2

typedef struct log_ring log_ring_t;

/* capacity_bytes 256 ... 65536; returns NULL outside that range or when
 * memory runs out. All memory is allocated here. */
log_ring_t *log_ring_create(size_t capacity_bytes);

/* Accepts NULL. */
void log_ring_destroy(log_ring_t *lr);

/* Store one line of len bytes (no NUL needed). Returns its sequence number,
 * or 0 if nothing was stored (empty line, NULL). */
uint32_t log_ring_append(log_ring_t *lr, const char *text, size_t len);

/* Sequence number of the oldest and newest line held; 0 when empty. */
uint32_t log_ring_first_seq(const log_ring_t *lr);
uint32_t log_ring_last_seq(const log_ring_t *lr);

/* Copy line seq into out, NUL-terminated and cut to out_size - 1 bytes.
 * Returns the full length of the line, or -1 if the line is not held (too
 * old, not written yet) or out is NULL or out_size is 0. */
int log_ring_read(const log_ring_t *lr, uint32_t seq, char *out, size_t out_size);

/*
 * Level letter of an ESP-IDF log line: 'E', 'W', 'I', 'D' or 'V' if the line
 * (after an optional colour sequence) starts with that letter followed by
 * " (", as in "I (1234) wifi: connected". Otherwise 0. NULL gives 0.
 */
char log_line_level(const char *line);

#ifdef __cplusplus
}
#endif

#endif /* LOG_RING_H */
