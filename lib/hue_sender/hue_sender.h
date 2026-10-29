/*
 * hue_sender – when to send which Hue command, and what to do on failure
 * (pure C, no network).
 *
 * Requirements: LIGHT-02, NFR-09 (see the requirements doc).
 * Tests:        test/test_hue_sender/
 * Depends on:   mode_controller.h (hue_command_t), hue_api.h (hue_result_t,
 *               hue_retry_delay_ms)
 *
 * The hue_task loop:
 *   - mode_controller_take_hue_command() → hue_sender_submit()
 *   - hue_sender_poll() → true: build the request with hue_api, send it over
 *     HTTPS, parse the answer, then hue_sender_report() with the result
 *
 * Rules
 *   Latest wins: submit() replaces any command that has not been sent yet or
 *   is waiting for a retry. Its attempt count starts at 0 and it is due
 *   immediately.
 *
 *   One request at a time: poll() returns a command only if none is in
 *   flight, a command is pending, and now has reached its due time. It then
 *   counts an attempt and the command is in flight until report().
 *
 *   A command submitted while another is in flight waits for that report and
 *   is then due immediately, whatever the in-flight result was. The in-flight
 *   command is never retried in that case: the newer one supersedes it.
 *
 *   report() of the in-flight command, with no newer command waiting:
 *     HUE_OK                       done, IDLE, attempts back to 0
 *     retryable (hue_api)          WAITING_RETRY, due at now + hue_retry_delay_ms(attempts)
 *     HUE_ERR_AUTH                 NEEDS_PAIRING: the command is kept but
 *                                  nothing is sent until hue_sender_paired()
 *     anything else                dropped (retrying cannot help), IDLE
 *   Retries never give up: the bridge must end up in the latest mode's scene
 *   whenever it becomes reachable again.
 *
 *   NEEDS_PAIRING holds until hue_sender_paired(). submit() during it
 *   replaces the kept command but does not leave the state. paired() makes
 *   the kept command due immediately (attempts 0), or goes IDLE if none.
 *
 *   report() without a command in flight is ignored. last_result() is the
 *   result of the most recent accepted report (HUE_OK before any).
 *
 * Time is a wrapping uint32 millisecond counter.
 */
#ifndef HUE_SENDER_H
#define HUE_SENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "hue_api.h"
#include "mode_controller.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HUE_SENDER_IDLE = 0,       /* nothing to send                       */
    HUE_SENDER_PENDING,        /* a command is due (poll will send it)  */
    HUE_SENDER_IN_FLIGHT,      /* sent, waiting for report()            */
    HUE_SENDER_WAITING_RETRY,  /* failed, retry scheduled               */
    HUE_SENDER_NEEDS_PAIRING,  /* key rejected, waiting for paired()    */
} hue_sender_state_t;

typedef struct hue_sender hue_sender_t;

/* Returns NULL only if memory runs out. */
hue_sender_t *hue_sender_create(void);

/* Accepts NULL. */
void hue_sender_destroy(hue_sender_t *hs);

void hue_sender_submit(hue_sender_t *hs, const hue_command_t *cmd, uint32_t now_ms);

/* If a command should be sent now, copy it to out and return true. */
bool hue_sender_poll(hue_sender_t *hs, uint32_t now_ms, hue_command_t *out);

void hue_sender_report(hue_sender_t *hs, hue_result_t result, uint32_t now_ms);

void hue_sender_paired(hue_sender_t *hs, uint32_t now_ms);

/* State as of the last call. A command in flight with a newer one waiting
 * reports IN_FLIGHT. */
hue_sender_state_t hue_sender_state(const hue_sender_t *hs);

/* Sends of the current command so far (0 when idle or freshly submitted). */
uint32_t hue_sender_attempts(const hue_sender_t *hs);

hue_result_t hue_sender_last_result(const hue_sender_t *hs);

#ifdef __cplusplus
}
#endif

#endif /* HUE_SENDER_H */
