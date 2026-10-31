/*
 * wifi – station mode with the credentials from the settings (LIGHT-01).
 *
 * Network: the console setting wifi_ssid / wifi_pass if set, otherwise
 * WIFI_SSID / WIFI_PASSWORD from src/secrets.h (compiled in).
 *
 *   wifi_start()      once at boot
 *   wifi_reconnect()  after "set wifi_ssid" / "set wifi_pass" on the console
 *
 * A lost connection is retried with a growing pause (1 s doubling to 30 s).
 * Only 2.4 GHz networks work.
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stdint.h>

void wifi_start(void);
void wifi_reconnect(void);
bool wifi_is_connected(void);

/* Block up to timeout_ms for a connection. Returns wifi_is_connected(). */
bool wifi_wait_connected(uint32_t timeout_ms);

#endif /* WIFI_H */
