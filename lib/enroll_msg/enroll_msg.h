/*
 * enroll_msg – the JSON of the pairing exchange between an unclaimed device
 * and the server (pure C over cJSON).
 *
 * Requirements: PAIR-01 ... PAIR-08 (see the requirements doc).
 * Tests:        test/test_enroll_msg/
 * Protocol:     docs/PROTOCOL.md, "Pairing"
 *
 * An unclaimed device POSTs to https://<server_host>/api/enroll:
 *   hello    {"chip":"a4cf12f0e1d2","node":"bath","fw":"2.0.0","secret":"<32 hex>"}
 *   confirm  {"chip":"a4cf12f0e1d2","secret":"<32 hex>","confirm":true}
 * and the server answers each with one of
 *   {"state":"wait"}                         nobody has asked to add it
 *   {"state":"confirm","window_s":60}        the user pressed "Add": hold the
 *                                            button for 3 s within window_s
 *   {"state":"claimed","device_id":"bath-3f2a","mqtt_host":"ambience.example.org",
 *    "mqtt_port":8883,"mqtt_user":"bath-3f2a","mqtt_pass":"…"}
 *                                            only as the answer to a confirm
 *   {"state":"rejected","reason":"conflict"} the server will not pair this
 *                                            chip/secret pair
 *
 * chip   the factory MAC (esp_efuse_mac_get_default) as 12 lower-case hex digits.
 * secret 16 random bytes made at the first boot and kept in NVS, as 32
 *        lower-case hex digits. The server binds a chip id to the first secret
 *        it sees; only that secret can confirm, so a forged hello with the same
 *        chip id cannot collect the credentials.
 *
 * Output format of the builders: one line, members in the order shown, no
 * spaces (cJSON_PrintUnformatted). They return the length without the NUL,
 * or 0 (out[0] = '\0' if out_size > 0) if an argument is NULL or invalid or
 * the text does not fit.
 */
#ifndef ENROLL_MSG_H
#define ENROLL_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENROLL_CHIP_LEN 12
#define ENROLL_SECRET_LEN 32
#define ENROLL_WINDOW_MIN_S 10
#define ENROLL_WINDOW_MAX_S 300

typedef enum {
    ENROLL_REPLY_WAIT = 0,
    ENROLL_REPLY_CONFIRM,
    ENROLL_REPLY_CLAIMED,
    ENROLL_REPLY_REJECTED,
} enroll_reply_kind_t;

typedef struct {
    char device_id[25];
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_user[33];
    char mqtt_pass[65];
} enroll_creds_t;

typedef struct {
    enroll_reply_kind_t kind;
    uint16_t window_s;   /* CONFIRM */
    enroll_creds_t creds; /* CLAIMED */
    char reason[32];     /* REJECTED, "" if none or too long */
} enroll_reply_t;

/* chip: exactly ENROLL_CHIP_LEN of 0-9 a-f. node: 1 ... 24 of a-z and '_'.
 * fw: 1 ... 31 printable ASCII (0x20 ... 0x7E). secret: exactly
 * ENROLL_SECRET_LEN of 0-9 a-f. */
size_t enroll_msg_hello(const char *chip, const char *node, const char *fw, const char *secret, char *out,
                        size_t out_size);

size_t enroll_msg_confirm(const char *chip, const char *secret, char *out, size_t out_size);

/* bytes -> lower-case hex text with NUL; out must hold 2 * n + 1. false if
 * an argument is NULL or out is too small. */
bool enroll_msg_hex(const uint8_t *bytes, size_t n, char *out, size_t out_size);

/*
 * Parses one answer. json need not be NUL-terminated (len bytes). Returns
 * false (out untouched) for NULL arguments, text that is not a JSON object,
 * a missing or unknown "state", and:
 *   confirm  window_s missing, not an integer, or outside
 *            ENROLL_WINDOW_MIN_S ... ENROLL_WINDOW_MAX_S
 *   claimed  any of the five members missing or of the wrong JSON type
 *            (mqtt_port a number, the others strings), a string that is
 *            "", or a value the "net" settings group would refuse for that
 *            key (lib/node_config:
 *            device_id, mqtt_host, mqtt_port, mqtt_user, mqtt_pass)
 * Unknown members are ignored. On success *out is filled completely: fields
 * that kind does not use are zero / "". rejected without a reason, or with a
 * reason that is not a string or longer than 31 bytes: reason "".
 */
bool enroll_msg_parse(const char *json, size_t len, enroll_reply_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ENROLL_MSG_H */
