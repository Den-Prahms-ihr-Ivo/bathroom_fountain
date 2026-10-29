/*
 * settings – STUB.
 *
 * Compiles so the tests build; every test fails until this is implemented.
 * The contract is in settings.h, the tests in test/test_settings/.
 */
#include "settings.h"

#include <string.h>

void settings_defaults(settings_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s)); /* TODO */
    }
}

settings_result_t settings_set(settings_t *s, const char *key, const char *value)
{
    (void)s;
    (void)key;
    (void)value;
    return SETTINGS_ERR_UNKNOWN_KEY; /* TODO */
}

bool settings_format(const settings_t *s, const char *key, bool reveal_secrets, char *out, size_t out_size)
{
    (void)s;
    (void)key;
    (void)reveal_secrets;
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    return false; /* TODO */
}

size_t settings_key_count(void)
{
    return 0; /* TODO */
}

const char *settings_key_name(size_t index)
{
    (void)index;
    return NULL; /* TODO */
}

bool settings_is_complete(const settings_t *s)
{
    (void)s;
    return false;
}
