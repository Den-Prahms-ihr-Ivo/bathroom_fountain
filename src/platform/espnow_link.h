/*
 * espnow_link – ESP-NOW transport for lib/link_msg frames between the bedside
 * and the bedroom speaker (docs/PROTOCOL.md, ESP-NOW link), with lib/link_peer
 * for retries, ACKs, duplicate filtering and the alive state.
 *
 * Skeleton scope: ONE peer (bedside <-> speaker).
 *   - The peer's MAC is learned from its first HELLO and kept in NVS
 *     ("espnow"/"peer"); until then frames go to the broadcast address.
 *   - TODO: list of peers (gun, targets), pairing window (cmd/link/pair),
 *     ESP-NOW encryption key set at pairing, dropping unknown MACs.
 *
 * Wi-Fi must be started first (ESP-NOW uses the station's channel: both nodes
 * must be on the same access point).
 *
 * rx callback runs in the link task, not in the Wi-Fi task.
 */
#ifndef ESPNOW_LINK_H
#define ESPNOW_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "link_msg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*espnow_link_rx_t)(const link_msg_t *msg);

/* self: this node's link_node_t (HELLO is sent at start and every time the
 * peer comes up). caps: LINK_CAP_* */
bool espnow_link_start(link_node_t self, uint16_t caps, const char *name, espnow_link_rx_t rx);

/* Encode and send to the peer. ack = with LINK_FLAG_ACK_REQ (retried by
 * link_peer). seq is filled in here. false if the queue is full. */
bool espnow_link_send(link_msg_t *msg, bool ack);

/* The peer answered within its alive timeout. */
bool espnow_link_peer_up(void);

/* Heartbeat payload of this node (state byte, battery), sent by the task. */
void espnow_link_set_heartbeat_state(uint8_t state);

#ifdef __cplusplus
}
#endif

#endif /* ESPNOW_LINK_H */
