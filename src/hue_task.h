/*
 * hue_task – talks to the Hue Bridge over HTTPS (LIGHT-01, LIGHT-02, NFR-07,
 * NFR-09, NFR-10).
 *
 * hue_sender decides when to send and how to retry; hue_api builds and parses
 * the messages; this task does the HTTPS (esp_http_client, keep-alive) and
 * reads host, key and scene ids from the settings at every request, so a
 * console "set" takes effect at once.
 *
 * The bridge uses a self-signed certificate, so the server certificate is
 * not verified (sdkconfig: CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY). The
 * connection is still encrypted; it only stays inside your home network.
 *
 * Pairing (NFR-10): press the link button on the bridge, then within 30 s
 * hold the device button for 3 s (or type "hue pair"). The new key is saved
 * as the hue_key setting.
 */
#ifndef HUE_TASK_H
#define HUE_TASK_H

#include "mode_controller.h"

void hue_task_start(void);

/* Latest wins: a command not yet sent is replaced. Never blocks. */
void hue_task_submit(const hue_command_t *cmd);

/* Try pairing for 30 s. */
void hue_task_request_pairing(void);

/* Print the bridge's scenes (id, name, room) on the console. */
void hue_task_request_scene_list(void);

#endif /* HUE_TASK_H */
