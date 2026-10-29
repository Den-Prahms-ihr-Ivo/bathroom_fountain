/*
 * hue_sender – when to send which Hue command, and what to do on failure.
 *
 * The contract is in hue_sender.h, the tests in test/test_hue_sender/.
 */
#include "hue_sender.h"

#include <stdlib.h>

struct hue_sender {
    hue_sender_state_t state;
    hue_command_t cmd;       /* pending, in flight, waiting for retry or kept for pairing */
    uint32_t attempts;
    uint32_t due_ms;
    bool has_newer;          /* submitted while cmd was in flight */
    hue_command_t newer;
    hue_result_t last_result;
};

hue_sender_t *hue_sender_create(void)
{
    hue_sender_t *hs = calloc(1, sizeof(*hs));
    if (hs != NULL) {
        hs->state = HUE_SENDER_IDLE;
        hs->last_result = HUE_OK;
    }
    return hs;
}

void hue_sender_destroy(hue_sender_t *hs)
{
    free(hs);
}

static void make_due_now(hue_sender_t *hs, const hue_command_t *cmd, uint32_t now_ms)
{
    hs->cmd = *cmd;
    hs->attempts = 0;
    hs->due_ms = now_ms;
    hs->state = HUE_SENDER_PENDING;
}

void hue_sender_submit(hue_sender_t *hs, const hue_command_t *cmd, uint32_t now_ms)
{
    if (hs == NULL || cmd == NULL) {
        return;
    }

    switch (hs->state) {
    case HUE_SENDER_IN_FLIGHT:
        /* One request at a time: this one goes out after the report. */
        hs->newer = *cmd;
        hs->has_newer = true;
        break;
    case HUE_SENDER_NEEDS_PAIRING:
        hs->cmd = *cmd; /* replace the kept command, keep waiting for paired() */
        hs->attempts = 0;
        break;
    default:
        make_due_now(hs, cmd, now_ms); /* latest wins, also over a retry wait */
        break;
    }
}

bool hue_sender_poll(hue_sender_t *hs, uint32_t now_ms, hue_command_t *out)
{
    if (hs == NULL || out == NULL) {
        return false;
    }
    if (hs->state != HUE_SENDER_PENDING && hs->state != HUE_SENDER_WAITING_RETRY) {
        return false;
    }
    if ((int32_t)(now_ms - hs->due_ms) < 0) {
        return false; /* not due yet; signed difference survives the wrap */
    }

    *out = hs->cmd;
    hs->attempts++;
    hs->state = HUE_SENDER_IN_FLIGHT;
    return true;
}

void hue_sender_report(hue_sender_t *hs, hue_result_t result, uint32_t now_ms)
{
    if (hs == NULL || hs->state != HUE_SENDER_IN_FLIGHT) {
        return;
    }
    hs->last_result = result;

    if (hs->has_newer) {
        /* The newer command supersedes the reported one, whatever the result.
         * A rejected key would reject it too, so it waits for pairing. */
        hs->has_newer = false;
        make_due_now(hs, &hs->newer, now_ms);
        if (result == HUE_ERR_AUTH) {
            hs->state = HUE_SENDER_NEEDS_PAIRING;
        }
        return;
    }

    if (result == HUE_OK) {
        hs->state = HUE_SENDER_IDLE;
        hs->attempts = 0;
    } else if (hue_result_is_retryable(result)) {
        hs->state = HUE_SENDER_WAITING_RETRY; /* never gives up (NFR-09) */
        hs->due_ms = now_ms + hue_retry_delay_ms(hs->attempts);
    } else if (result == HUE_ERR_AUTH) {
        hs->state = HUE_SENDER_NEEDS_PAIRING;
    } else {
        hs->state = HUE_SENDER_IDLE; /* retrying cannot help */
        hs->attempts = 0;
    }
}

void hue_sender_paired(hue_sender_t *hs, uint32_t now_ms)
{
    if (hs != NULL && hs->state == HUE_SENDER_NEEDS_PAIRING) {
        make_due_now(hs, &hs->cmd, now_ms);
    }
}

hue_sender_state_t hue_sender_state(const hue_sender_t *hs)
{
    return (hs != NULL) ? hs->state : HUE_SENDER_IDLE;
}

uint32_t hue_sender_attempts(const hue_sender_t *hs)
{
    return (hs != NULL) ? hs->attempts : 0;
}

hue_result_t hue_sender_last_result(const hue_sender_t *hs)
{
    return (hs != NULL) ? hs->last_result : HUE_OK;
}
