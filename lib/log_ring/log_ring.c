/*
 * log_ring – the last log lines in RAM.
 * Contract in log_ring.h, tests in test/test_log_ring/.
 *
 * Storage: one byte ring of capacity bytes. Each line is a 2-byte length
 * (little endian) followed by its bytes; both may wrap around the end.
 */
#include "log_ring.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define CAP_MIN 256u
#define CAP_MAX 65536u
#define ESC 0x1B

struct log_ring {
    uint8_t *buf;
    size_t cap;
    size_t tail;    /* offset of the oldest line */
    size_t used;    /* bytes held */
    uint32_t first; /* seq of the oldest line, 0 when empty */
    uint32_t count; /* lines held */
    uint32_t next;  /* seq of the next line */
};

log_ring_t *log_ring_create(size_t capacity_bytes)
{
    if (capacity_bytes < CAP_MIN || capacity_bytes > CAP_MAX) {
        return NULL;
    }
    log_ring_t *lr = calloc(1, sizeof(*lr));
    if (lr == NULL) {
        return NULL;
    }
    lr->buf = malloc(capacity_bytes);
    if (lr->buf == NULL) {
        free(lr);
        return NULL;
    }
    lr->cap = capacity_bytes;
    lr->next = 1;
    return lr;
}

void log_ring_destroy(log_ring_t *lr)
{
    if (lr != NULL) {
        free(lr->buf);
        free(lr);
    }
}

static void ring_write(log_ring_t *lr, size_t pos, const void *src, size_t n)
{
    const uint8_t *s = src;
    pos %= lr->cap;
    const size_t first = n < lr->cap - pos ? n : lr->cap - pos;
    memcpy(lr->buf + pos, s, first);
    memcpy(lr->buf, s + first, n - first);
}

static void ring_read(const log_ring_t *lr, size_t pos, void *dst, size_t n)
{
    uint8_t *d = dst;
    pos %= lr->cap;
    const size_t first = n < lr->cap - pos ? n : lr->cap - pos;
    memcpy(d, lr->buf + pos, first);
    memcpy(d + first, lr->buf, n - first);
}

static size_t entry_len(const log_ring_t *lr, size_t pos)
{
    uint8_t h[2];
    ring_read(lr, pos, h, 2);
    return (size_t)h[0] | ((size_t)h[1] << 8);
}

static void drop_oldest(log_ring_t *lr)
{
    const size_t n = entry_len(lr, lr->tail) + LOG_RING_OVERHEAD;
    lr->tail = (lr->tail + n) % lr->cap;
    lr->used -= n;
    lr->count--;
    lr->first = lr->count ? lr->first + 1 : 0;
}

/* Skip an ANSI sequence starting at text[i] == ESC, text[i+1] == '['.
 * Returns the index after its final letter (or len). */
static size_t skip_ansi(const char *text, size_t len, size_t i)
{
    for (i += 2; i < len; i++) {
        const char c = text[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            return i + 1;
        }
    }
    return len;
}

/* Clean one line into out (LOG_RING_LINE_MAX bytes); returns its length. */
static size_t clean(const char *text, size_t len, char *out)
{
    size_t n = 0;
    size_t pending_breaks = 0; /* '\r' / '\n' seen but not yet known to be trailing */
    size_t i = 0;
    while (i < len && n < LOG_RING_LINE_MAX) {
        const unsigned char c = (unsigned char)text[i];
        if (c == ESC && i + 1 < len && text[i + 1] == '[') {
            i = skip_ansi(text, len, i);
            continue;
        }
        i++;
        if (c == '\r' || c == '\n') {
            pending_breaks++;
            continue;
        }
        while (pending_breaks > 0 && n < LOG_RING_LINE_MAX) {
            out[n++] = ' ';
            pending_breaks--;
        }
        pending_breaks = 0;
        if (n < LOG_RING_LINE_MAX) {
            out[n++] = c < 0x20 ? ' ' : (char)c;
        }
    }
    return n;
}

uint32_t log_ring_append(log_ring_t *lr, const char *text, size_t len)
{
    if (lr == NULL || text == NULL) {
        return 0;
    }
    char line[LOG_RING_LINE_MAX];
    const size_t n = clean(text, len, line);
    if (n == 0) {
        return 0;
    }
    const size_t need = n + LOG_RING_OVERHEAD;
    while (lr->count > 0 && lr->used + need > lr->cap) {
        drop_oldest(lr);
    }
    const size_t head = (lr->tail + lr->used) % lr->cap;
    const uint8_t h[2] = {(uint8_t)(n & 0xFF), (uint8_t)(n >> 8)};
    ring_write(lr, head, h, 2);
    ring_write(lr, head + 2, line, n);
    lr->used += need;
    if (lr->count == 0) {
        lr->first = lr->next;
    }
    lr->count++;
    return lr->next++;
}

uint32_t log_ring_first_seq(const log_ring_t *lr)
{
    return (lr != NULL && lr->count) ? lr->first : 0;
}

uint32_t log_ring_last_seq(const log_ring_t *lr)
{
    return (lr != NULL && lr->count) ? lr->first + lr->count - 1 : 0;
}

int log_ring_read(const log_ring_t *lr, uint32_t seq, char *out, size_t out_size)
{
    if (lr == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    if (lr->count == 0 || seq < lr->first || seq - lr->first >= lr->count) {
        return -1;
    }
    size_t pos = lr->tail;
    for (uint32_t s = lr->first; s < seq; s++) {
        pos = (pos + entry_len(lr, pos) + LOG_RING_OVERHEAD) % lr->cap;
    }
    const size_t n = entry_len(lr, pos);
    const size_t copy = n < out_size - 1 ? n : out_size - 1;
    ring_read(lr, pos + 2, out, copy);
    out[copy] = '\0';
    return (int)n;
}

char log_line_level(const char *line)
{
    if (line == NULL) {
        return 0;
    }
    const size_t len = strlen(line);
    size_t i = 0;
    if (len >= 2 && (unsigned char)line[0] == ESC && line[1] == '[') {
        i = skip_ansi(line, len, 0);
    }
    const char c = line[i];
    if ((c == 'E' || c == 'W' || c == 'I' || c == 'D' || c == 'V') && line[i + 1] == ' ' && line[i + 2] == '(') {
        return c;
    }
    return 0;
}
