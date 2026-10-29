/*
 * hue_sender – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in hue_sender.h, the tests in test/test_hue_sender/.
 */
#include "hue_sender.h"

hue_sender_t *hue_sender_create(void)
{
    return NULL; /* TODO */
}

void hue_sender_destroy(hue_sender_t *hs)
{
    (void)hs;
}

void hue_sender_submit(hue_sender_t *hs, const hue_command_t *cmd, uint32_t now_ms)
{
    (void)hs;
    (void)cmd;
    (void)now_ms;
}

bool hue_sender_poll(hue_sender_t *hs, uint32_t now_ms, hue_command_t *out)
{
    (void)hs;
    (void)now_ms;
    (void)out;
    return false;
}

void hue_sender_report(hue_sender_t *hs, hue_result_t result, uint32_t now_ms)
{
    (void)hs;
    (void)result;
    (void)now_ms;
}

void hue_sender_paired(hue_sender_t *hs, uint32_t now_ms)
{
    (void)hs;
    (void)now_ms;
}

hue_sender_state_t hue_sender_state(const hue_sender_t *hs)
{
    (void)hs;
    return HUE_SENDER_IDLE;
}

uint32_t hue_sender_attempts(const hue_sender_t *hs)
{
    (void)hs;
    return 0;
}

hue_result_t hue_sender_last_result(const hue_sender_t *hs)
{
    (void)hs;
    return HUE_OK;
}
