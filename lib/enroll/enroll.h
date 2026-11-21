/*
 * enroll – the device side of pairing: an unclaimed device checks in with
 * the server until someone adds it in the web app and the button is held
 * for 3 s (pure C, no network).
 *
 * Requirements: PAIR-01 ... PAIR-08 (see the requirements doc).
 * Tests:        test/test_enroll/
 * Messages:     lib/enroll_msg; protocol in docs/PROTOCOL.md, "Pairing".
 *
 * The app (mqtt_link) owns HTTPS and NVS; this module only decides:
 *   ENROLL_ACT_HELLO    POST the hello, feed the parsed answer to
 *                       enroll_reply(). No answer / HTTP error: feed nothing.
 *   ENROLL_ACT_CONFIRM  POST the confirm, same.
 *   ENROLL_ACT_SAVE     write enroll_creds() into the "net" group (device_id,
 *                       mqtt_host, mqtt_port, mqtt_user, mqtt_pass), save to
 *                       NVS, (re)start MQTT.
 *   ENROLL_ACT_CLEAR    erase those five keys from the group and NVS, stop
 *                       MQTT. The secret stays.
 *   outputs.blink       true while the device waits for the 3 s hold: show
 *                       it (bath: candles pulse; bedside: LEDs pulse;
 *                       speaker: status LED). false otherwise.
 *
 * States (outputs.state)
 *   CLAIMED     has credentials: talks MQTT, nothing to do here.
 *   UNCLAIMED   HELLO every cfg.hello_ms.
 *   CONFIRM     the server said "confirm": the window (window_s from the
 *               answer) runs, blink on, HELLO every cfg.poll_ms (to see a
 *               cancel or a renewed window).
 *   CONFIRMING  the button was held: CONFIRM every cfg.poll_ms until an
 *               answer decides or the window ends. Blink stays on.
 *
 * enroll_reply(reply, now) – returns true if the answer changed something:
 *   CLAIMED     everything ignored (false).
 *   UNCLAIMED   WAIT: nothing (false). CONFIRM: -> CONFIRM, window =
 *               window_s from now, next HELLO in poll_ms. CLAIMED: ignored
 *               (credentials only count after a confirm; false). REJECTED:
 *               outputs.rejected = true, stays UNCLAIMED (true).
 *   CONFIRM     WAIT: the user cancelled: -> UNCLAIMED, next HELLO in
 *               hello_ms. CONFIRM: window renewed (window_s from now).
 *               CLAIMED: ignored (false; the hold is required). REJECTED:
 *               -> UNCLAIMED, rejected = true.
 *   CONFIRMING  CLAIMED: SAVE, -> CLAIMED (enroll_creds() holds the
 *               credentials). WAIT or REJECTED: -> UNCLAIMED (rejected =
 *               true for REJECTED). CONFIRM: nothing (false; keep sending).
 *   Any change of state away from UNCLAIMED clears outputs.rejected; a
 *   REJECTED sets it.
 * enroll_button_held(now) – the app calls it when its pairing button has
 *   been held for 3 s. Returns true if it was used:
 *   CONFIRM: CONFIRM action at once, -> CONFIRMING, next CONFIRM in poll_ms.
 *   CONFIRMING: true, nothing else (a second hold changes nothing).
 *   CLAIMED, UNCLAIMED: false (the app does what the hold does otherwise:
 *   bath = Hue pairing, speaker = standby).
 *   Priority is the app's: the speaker calls this only when no alarm rings.
 * enroll_tick(now)
 *   UNCLAIMED: HELLO when due; the next one hello_ms after this one.
 *   CONFIRM / CONFIRMING: the window ended -> UNCLAIMED and a HELLO at once
 *   (then every hello_ms). Otherwise HELLO (CONFIRM) or CONFIRM (CONFIRMING)
 *   when due, the next poll_ms after it.
 *   A late tick sends at most one message and schedules the next from now.
 * enroll_auth_result(ok, now) – the broker accepted (true) or refused as
 *   "not authorised" (false) the device's credentials. Ignored unless
 *   CLAIMED. cfg.auth_fail_limit refusals in a row: CLEAR, -> UNCLAIMED,
 *   HELLO at once. An accept resets the count. Network errors, timeouts and
 *   other broker errors must NOT be reported here: a server outage must not
 *   erase the credentials.
 * enroll_forget(now) – cmd/forget from the server or "forget" on the
 *   console: CLAIMED -> CLEAR, UNCLAIMED, HELLO at once, returns true. In
 *   the other states returns false and changes nothing.
 *
 * Times: wrapping uint32 ms. Every function taking now first does what
 * enroll_tick would.
 */
#ifndef ENROLL_H
#define ENROLL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "enroll_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENROLL_ACTS 8

typedef enum {
    ENROLL_CLAIMED = 0,
    ENROLL_UNCLAIMED,
    ENROLL_CONFIRM,
    ENROLL_CONFIRMING,
} enroll_state_t;

typedef enum {
    ENROLL_ACT_HELLO = 1,
    ENROLL_ACT_CONFIRM,
    ENROLL_ACT_SAVE,
    ENROLL_ACT_CLEAR,
} enroll_act_t;

typedef struct {
    uint32_t hello_ms;        /* 1000 ... 600000   default 10000 */
    uint32_t poll_ms;         /* 500 ... 60000     default 2000  */
    uint8_t auth_fail_limit;  /* 1 ... 20          default 3     */
} enroll_config_t;

typedef struct {
    enroll_state_t state;
    bool blink;
    bool rejected;
} enroll_outputs_t;

typedef struct enroll enroll_t;

void enroll_config_default(enroll_config_t *cfg);

/* claimed: the device has credentials in NVS. Unclaimed: the first HELLO is
 * due at now. NULL if cfg is NULL or out of range. */
enroll_t *enroll_create(const enroll_config_t *cfg, bool claimed, uint32_t now_ms);

/* Accepts NULL. */
void enroll_destroy(enroll_t *e);

bool enroll_reply(enroll_t *e, const enroll_reply_t *reply, uint32_t now_ms);
bool enroll_button_held(enroll_t *e, uint32_t now_ms);
void enroll_tick(enroll_t *e, uint32_t now_ms);
void enroll_auth_result(enroll_t *e, bool ok, uint32_t now_ms);
bool enroll_forget(enroll_t *e, uint32_t now_ms);

/* Valid for the object's lifetime; NULL for NULL. */
const enroll_outputs_t *enroll_outputs(const enroll_t *e);

/* The credentials of the last CLAIMED answer that was accepted (all zero
 * before). NULL for NULL. */
const enroll_creds_t *enroll_creds(const enroll_t *e);

/* Oldest first, at most ENROLL_ACTS waiting (the oldest is dropped when
 * full). false if none or an argument is NULL. */
bool enroll_take(enroll_t *e, enroll_act_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ENROLL_H */
