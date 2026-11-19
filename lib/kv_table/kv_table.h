/*
 * kv_table – text key/value access to a struct, driven by a table
 * (pure C, no NVS, no console).
 *
 * Requirements: CFG-01 ... CFG-04 (see the requirements doc).
 * Tests:        test/test_kv_table/
 *
 * lib/settings holds the v1 keys (Wi-Fi, Hue, clap) with hand-written code.
 * Everything new is a "group": a plain struct plus a const table that
 * describes each field. This module is the engine that reads and writes any
 * such struct as text, with the same rules as lib/settings, so the console,
 * NVS, the fallback page and the MQTT settings sync treat every group alike.
 * The groups themselves are in lib/node_config.
 *
 * Field types and their storage in the struct
 *   KV_UINT    uint8_t, uint16_t or uint32_t (size 1, 2 or 4)
 *   KV_BOOL    bool
 *   KV_CHOICE  uint8_t: index into the '|'-separated list in .choices
 *   KV_TEXT    char[size], NUL-terminated
 *   KV_SECRET  char[size], NUL-terminated; masked by kv_format()
 *   KV_UUID    char[size] with size >= 37
 *
 * Value syntax (checked first; failure -> KV_ERR_INVALID)
 *   uint    one to ten ASCII digits, nothing else
 *   bool    "on" or "off"
 *   choice  exactly one of the names in .choices
 *   text,   bytes 0x20 ... 0x7E only; if .check is set it must return true
 *   secret  (check is not called for the empty string)
 *   uuid    "" (unset) or 8-4-4-4-12 hex digits with hyphens, either case
 * Then the range (failure -> KV_ERR_RANGE)
 *   uint    min <= value <= max; a number too large for uint32 is out of
 *           range, never wrapped
 *   text,   min <= length <= max. The empty string is always accepted and
 *   secret  means "unset", whatever min says.
 * key or value NULL -> KV_ERR_INVALID; a key not in the table ->
 * KV_ERR_UNKNOWN_KEY (checked before the value). A rejected set changes
 * nothing. Strings are stored exactly as given.
 *
 * Output (kv_format)
 *   uint as decimal, bool as "on"/"off", choice as its name, strings as
 *   stored; a non-empty secret as "********" unless reveal_secrets is true.
 *   kv_format(reveal) followed by kv_set() reproduces every value.
 *
 * Defaults
 *   .def is the default as text, in the syntax above; NULL means "" for
 *   strings, 0 for uint (min must then be 0), off for bool and the first
 *   choice.
 */
#ifndef KV_TABLE_H
#define KV_TABLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KV_UINT = 0,
    KV_BOOL,
    KV_CHOICE,
    KV_TEXT,
    KV_SECRET,
    KV_UUID,
} kv_type_t;

typedef enum {
    KV_OK = 0,
    KV_ERR_UNKNOWN_KEY,
    KV_ERR_INVALID,
    KV_ERR_RANGE,
} kv_result_t;

typedef struct {
    const char *name;    /* 1 ... 31 characters of a-z, 0-9 and '_'            */
    kv_type_t type;
    uint16_t offset;     /* offsetof(group struct, field)                      */
    uint16_t size;       /* sizeof(field)                                      */
    uint32_t min;        /* uint: smallest value; text/secret: shortest length */
    uint32_t max;        /* uint: largest value;  text/secret: longest length  */
    const char *def;     /* default as text, or NULL                           */
    const char *choices; /* KV_CHOICE only: "easy|normal|hard"                 */
    bool (*check)(const char *value); /* text/secret only, or NULL             */
} kv_key_t;

typedef struct {
    const char *name; /* group name, e.g. "net" */
    const kv_key_t *keys;
    size_t count;
    size_t data_size; /* sizeof(group struct) */
} kv_table_t;

/* Helper for writing tables:  KV_FIELD(cfg_net_t, mqtt_port)  expands to the
 * offset and size initialisers. */
#define KV_FIELD(type, field) (uint16_t)offsetof(type, field), (uint16_t)sizeof(((type *)0)->field)

/*
 * Check a table for mistakes (used by the tests of every group): at least one
 * key; names well-formed and unique; each field inside data_size; size fits
 * the type (uint 1/2/4, bool sizeof(bool), choice 1, uuid >= 37, text/secret
 * >= 2); min <= max; uint max fits the field; text/secret max <= size - 1;
 * choice has 1 ... 255 non-empty names without duplicates; the default is a
 * value kv_set() would accept. NULL is invalid.
 */
bool kv_table_valid(const kv_table_t *table);

/* Write every default into data. */
void kv_defaults(const kv_table_t *table, void *data);

kv_result_t kv_set(const kv_table_t *table, void *data, const char *key, const char *value);

/* Write the value of key into out (NUL-terminated). Returns false for an
 * unknown key or if out is too small; out is then left empty if out_size > 0. */
bool kv_format(const kv_table_t *table, const void *data, const char *key, bool reveal_secrets, char *out,
               size_t out_size);

/* Index of key in the table, or -1. */
int kv_find(const kv_table_t *table, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* KV_TABLE_H */
