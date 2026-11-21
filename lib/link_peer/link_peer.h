/*
 * link_peer – reliable delivery to one other node and "is it still there?"
 * (pure C, no radio; one object per peer).
 *
 * Requirements: LINK-05 ... LINK-09 (see the requirements doc).
 * Tests:        test/test_link_peer/
 *
 * ESP-NOW tells the sender whether the radio got a MAC-level acknowledgement,
 * but not whether the other firmware took the message. Commands such as
 * "stop the alarm" therefore carry LINK_FLAG_ACK_REQ and are repeated until
 * the peer answers with LINK_ACK. This module keeps the frames that wait for
 * an ack, says when to (re)send them, filters duplicates on the receiving
 * side and tracks whether the peer is alive. Time is a wrapping uint32
 * millisecond counter.
 *
 * Sending
 *   link_peer_next_seq()  1, 2, ... 65535, 1, ... (never 0: seq 0 means "not
 *                         acknowledged").
 *   link_peer_send()      Queues a complete frame (already encoded with that
 *                         seq and LINK_FLAG_ACK_REQ; the module copies it and
 *                         reads the seq from bytes 4-5). Up to
 *                         LINK_PEER_QUEUE frames wait; they go out strictly
 *                         in order, one at a time.
 *   link_peer_take_tx()   Hands out the frame at the head of the queue when
 *                         it is due: at once for its first transmission, then
 *                         every cfg.retry_ms after the previous one. Returns
 *                         0 when nothing is due.
 *   link_peer_on_ack()    An ack for the head's seq removes it (event
 *                         DELIVERED) and the next frame becomes due at once.
 *                         Any other seq is ignored.
 *   Giving up             When the head has been handed out cfg.max_tries
 *                         times and retry_ms pass without its ack, it is
 *                         dropped (event FAILED) and the next frame becomes
 *                         due.
 *
 * Receiving
 *   link_peer_on_rx(seq)  Call for every frame from this peer (also acks and
 *                         heartbeats). It marks the peer as heard. The return
 *                         value says whether to act on the frame: true for
 *                         seq 0 and for a seq not among the last
 *                         LINK_PEER_DEDUPE acknowledged ones; false for a
 *                         repeat (the peer missed our ack: ack again, do not
 *                         act again).
 *   link_peer_reset_rx()  Forget the remembered seqs: call when the peer says
 *                         HELLO, because it restarted and counts from 1 again.
 *
 * Liveness
 *   The peer is alive from the first frame heard until cfg.alive_timeout_ms
 *   pass without one. Changes are reported once as events UP and DOWN (DOWN
 *   is found by link_peer_tick()). A peer that was never heard is not alive
 *   and causes no DOWN.
 *
 * Heartbeat
 *   link_peer_heartbeat_due() is true once per cfg.heartbeat_ms, the first
 *   time on the first call. It does not send anything itself.
 *
 * Events are queued (LINK_PEER_EVENTS; the oldest is dropped if nobody takes
 * them) and taken in order.
 */
#ifndef LINK_PEER_H
#define LINK_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_PEER_QUEUE 4
#define LINK_PEER_FRAME_MAX 34 /* = LINK_FRAME_MAX: an AUDIO frame with its folder */
#define LINK_PEER_DEDUPE 8
#define LINK_PEER_EVENTS 8

typedef struct {
    uint16_t retry_ms;         /* 10 ... 1000             default 60    */
    uint8_t max_tries;         /* 1 ... 20                default 5     */
    uint32_t alive_timeout_ms; /* 1000 ... 600000         default 15000 */
    uint32_t heartbeat_ms;     /* 500 ... 60000, must be less than
                                  alive_timeout_ms        default 5000  */
} link_peer_config_t;

typedef enum {
    LINK_PEER_DELIVERED = 1,
    LINK_PEER_FAILED,
    LINK_PEER_UP,
    LINK_PEER_DOWN,
} link_peer_event_id_t;

typedef struct {
    link_peer_event_id_t id;
    uint16_t seq; /* DELIVERED, FAILED: the frame; otherwise 0 */
} link_peer_event_t;

typedef struct link_peer link_peer_t;

void link_peer_config_default(link_peer_config_t *cfg);

/* Returns NULL if cfg is NULL or a value is out of range. */
link_peer_t *link_peer_create(const link_peer_config_t *cfg, uint32_t now_ms);

/* Accepts NULL. */
void link_peer_destroy(link_peer_t *lp);

uint16_t link_peer_next_seq(link_peer_t *lp);

/* Returns false (nothing queued) if the queue is full, frame is NULL, len is
 * below 6 or above LINK_PEER_FRAME_MAX, or the frame's seq is 0. */
bool link_peer_send(link_peer_t *lp, const uint8_t *frame, size_t len, uint32_t now_ms);

/* Copy the frame that is due into out and return its length; 0 if none is
 * due or out is too small. */
size_t link_peer_take_tx(link_peer_t *lp, uint32_t now_ms, uint8_t *out, size_t out_size);

void link_peer_on_ack(link_peer_t *lp, uint16_t acked_seq, uint32_t now_ms);

bool link_peer_on_rx(link_peer_t *lp, uint16_t seq, uint32_t now_ms);

void link_peer_reset_rx(link_peer_t *lp);

/* Finds give-ups and the DOWN transition. Call regularly (e.g. every 20 ms). */
void link_peer_tick(link_peer_t *lp, uint32_t now_ms);

bool link_peer_alive(const link_peer_t *lp);

/* Frames waiting, including the one in flight. */
size_t link_peer_pending(const link_peer_t *lp);

bool link_peer_heartbeat_due(link_peer_t *lp, uint32_t now_ms);

bool link_peer_take_event(link_peer_t *lp, link_peer_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LINK_PEER_H */
