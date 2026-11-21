/*
 * enroll_task – pairing with the server (docs/PROTOCOL.md, Pairing) and the
 * owner of the "net" settings group (cfg_net_t).
 *
 * Wraps lib/enroll and lib/enroll_msg:
 *   - chip id  = factory MAC as 12 lower-case hex digits (printed at boot,
 *     the fallback on the Add page);
 *   - secret   = 16 random bytes as 32 hex digits, made once and kept in NVS
 *     ("enroll"/"secret"); a flash erase makes a new one;
 *   - HELLO / CONFIRM = HTTPS POST https://<server_host>/api/enroll, the
 *     answer goes back into enroll_reply();
 *   - SAVE  = the five net keys from enroll_creds() into NVS, then
 *     mqtt_link_restart();
 *   - CLEAR = the five net keys reset, mqtt_link_stop().
 *
 * Thread safety: every function here may be called from any task.
 *
 * Wi-Fi must be started before enroll_task_start(); the task waits for a
 * connection before every request.
 */
#ifndef ENROLL_TASK_H
#define ENROLL_TASK_H

#include <stdbool.h>

#include "enroll.h"
#include "node_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build default for server_host, written at the first boot. Put
 * #define SERVER_HOST_DEFAULT "ambience.example.org" into secrets.h, or set
 * server_host on the console. */
#ifndef SERVER_HOST_DEFAULT
#define SERVER_HOST_DEFAULT ""
#endif

/* Loads the net group (call after nvs_flash_init(), before mqtt_link_start)
 * and starts the task. node = "bath", "bedside", "bedroom_speaker", "crate". */
void enroll_task_start(const char *node);

/* The pairing button was held 3 s. true: it was used for pairing (a confirm
 * window is open); false: the app does what the hold does otherwise. */
bool enroll_task_button_held(void);

/* From mqtt_link: the broker accepted (true) or refused as not authorised
 * (false). Network errors are not reported. */
void enroll_task_auth_result(bool ok);

/* cmd/forget or "forget" on the console. */
void enroll_task_forget(void);

/* For LEDs / the crate strip: blink while a confirm window is open. */
bool enroll_task_blink(void);
enroll_state_t enroll_task_state(void);

/* A copy of the net group. */
void enroll_task_net(cfg_net_t *out);

/* Set one key of the net group (console, settings/desired). */
kv_result_t enroll_task_net_set(const char *key, const char *value);

/* "a1b2c3d4e5f6" */
const char *enroll_task_chip_id(void);

#ifdef __cplusplus
}
#endif

#endif /* ENROLL_TASK_H */
