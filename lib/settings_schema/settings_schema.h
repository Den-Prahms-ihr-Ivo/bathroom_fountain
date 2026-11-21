/*
 * settings_schema – describes a settings group so the web app can build its
 * form without knowing the device type (pure C over cJSON).
 *
 * Requirements: SRV-14 (see the requirements doc).
 * Tests:        test/test_settings_schema/
 *
 * Published retained on shower/<id>/settings/schema/<group> at every
 * connect, one message per group the node has (docs/PROTOCOL.md). The v1
 * keys of lib/settings have no table and therefore no schema; the web app
 * shows them as text fields and relies on "rejected" in the reported
 * settings.
 *
 * {"group":"bath","keys":[<key>,...]} with the keys in table order, each
 *   {"key":<name>,"type":<type>[,"min":n,"max":n][,"choices":[...]]
 *    [,"unit":<unit>][,"default":<text>]}
 *   type     "uint", "bool", "choice", "text", "secret", "uuid"
 *   min/max  uint: value range; text and secret: length range. Not for the
 *            other types.
 *   choices  choice only: the names in order ("easy|normal|hard" ->
 *            ["easy","normal","hard"]).
 *   unit     from the name's ending: "_ms" -> "ms", "_s" -> "s",
 *            "_min" -> "min", "_level" -> "%"; none otherwise.
 *   default  the table's default text, if it has one; never for secret.
 * Format: one line, members in this order, no spaces (cJSON_PrintUnformatted).
 * Returns the length without NUL, or 0 (out[0] = '\0' if out_size > 0) if an
 * argument is NULL, kv_table_valid() refuses the table, or the text would
 * not fit out_size or be longer than SETTINGS_SCHEMA_MAX.
 */
#ifndef SETTINGS_SCHEMA_H
#define SETTINGS_SCHEMA_H

#include <stddef.h>

#include "kv_table.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_SCHEMA_MAX 4095 /* MQTT buffer 4096 incl. NUL */

size_t settings_schema_json(const kv_table_t *table, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_SCHEMA_H */
