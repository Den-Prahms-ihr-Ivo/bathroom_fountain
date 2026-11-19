/*
 * link_peer – reliable delivery to one other node, duplicate filter and
 * liveness. Contract in link_peer.h, tests in test/test_link_peer/.
 */
#include "link_peer.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t data[LINK_PEER_FRAME_MAX];
    uint8_t len;
    uint16_t seq;
} frame_t;

struct link_peer {
    link_peer_config_t cfg;
    uint16_t next_seq;

    /* transmit queue, head first */
    frame_t queue[LINK_PEER_QUEUE];
    uint8_t q_count;
    uint8_t tries;      /* transmissions of the head */
    uint32_t last_tx;   /* time of the head's last transmission */

    /* receive */
    uint16_t seen[LINK_PEER_DEDUPE];
    uint8_t seen_count;
    uint8_t seen_next;

    /* liveness */
    bool alive;
    uint32_t last_heard;

    /* heartbeat */
    bool hb_started;
    uint32_t hb_last;

    link_peer_event_t events[LINK_PEER_EVENTS];
    uint8_t ev_head;
    uint8_t ev_count;
};

void link_peer_config_default(link_peer_config_t *cfg)
{
    if (cfg != NULL) {
        cfg->retry_ms = 60;
        cfg->max_tries = 5;
        cfg->alive_timeout_ms = 15000;
        cfg->heartbeat_ms = 5000;
    }
}

link_peer_t *link_peer_create(const link_peer_config_t *cfg, uint32_t now_ms)
{
    (void)now_ms;
    if (cfg == NULL || cfg->retry_ms < 10 || cfg->retry_ms > 1000 || cfg->max_tries < 1 || cfg->max_tries > 20 ||
        cfg->alive_timeout_ms < 1000 || cfg->alive_timeout_ms > 600000u || cfg->heartbeat_ms < 500 ||
        cfg->heartbeat_ms > 60000 || cfg->heartbeat_ms >= cfg->alive_timeout_ms) {
        return NULL;
    }
    link_peer_t *lp = calloc(1, sizeof(*lp));
    if (lp != NULL) {
        lp->cfg = *cfg;
        lp->next_seq = 1;
    }
    return lp;
}

void link_peer_destroy(link_peer_t *lp)
{
    free(lp);
}

static void push_event(link_peer_t *lp, link_peer_event_id_t id, uint16_t seq)
{
    if (lp->ev_count == LINK_PEER_EVENTS) {
        lp->ev_head = (uint8_t)((lp->ev_head + 1) % LINK_PEER_EVENTS);
        lp->ev_count--;
    }
    link_peer_event_t *e = &lp->events[(lp->ev_head + lp->ev_count) % LINK_PEER_EVENTS];
    e->id = id;
    e->seq = seq;
    lp->ev_count++;
}

uint16_t link_peer_next_seq(link_peer_t *lp)
{
    if (lp == NULL) {
        return 0;
    }
    const uint16_t s = lp->next_seq;
    lp->next_seq = (uint16_t)(s == 65535u ? 1u : s + 1u);
    return s;
}

static void pop_head(link_peer_t *lp)
{
    memmove(&lp->queue[0], &lp->queue[1], (size_t)(lp->q_count - 1) * sizeof(frame_t));
    lp->q_count--;
    lp->tries = 0;
}

bool link_peer_send(link_peer_t *lp, const uint8_t *frame, size_t len, uint32_t now_ms)
{
    (void)now_ms;
    if (lp == NULL || frame == NULL || len < 6 || len > LINK_PEER_FRAME_MAX || lp->q_count >= LINK_PEER_QUEUE) {
        return false;
    }
    const uint16_t seq = (uint16_t)(frame[4] | (frame[5] << 8));
    if (seq == 0) {
        return false;
    }
    frame_t *f = &lp->queue[lp->q_count];
    memcpy(f->data, frame, len);
    f->len = (uint8_t)len;
    f->seq = seq;
    if (lp->q_count == 0) {
        lp->tries = 0;
    }
    lp->q_count++;
    return true;
}

/* Drop the head if it has used all its tries and the last retry_ms passed. */
static void check_give_up(link_peer_t *lp, uint32_t now_ms)
{
    while (lp->q_count > 0 && lp->tries >= lp->cfg.max_tries &&
           (uint32_t)(now_ms - lp->last_tx) >= lp->cfg.retry_ms) {
        push_event(lp, LINK_PEER_FAILED, lp->queue[0].seq);
        pop_head(lp);
    }
}

size_t link_peer_take_tx(link_peer_t *lp, uint32_t now_ms, uint8_t *out, size_t out_size)
{
    if (lp == NULL || out == NULL) {
        return 0;
    }
    check_give_up(lp, now_ms);
    if (lp->q_count == 0) {
        return 0;
    }
    const bool due = lp->tries == 0 || (uint32_t)(now_ms - lp->last_tx) >= lp->cfg.retry_ms;
    if (!due) {
        return 0;
    }
    const frame_t *f = &lp->queue[0];
    if (f->len > out_size) {
        return 0;
    }
    memcpy(out, f->data, f->len);
    lp->tries++;
    lp->last_tx = now_ms;
    return f->len;
}

void link_peer_on_ack(link_peer_t *lp, uint16_t acked_seq, uint32_t now_ms)
{
    (void)now_ms;
    if (lp == NULL || lp->q_count == 0 || acked_seq == 0 || lp->queue[0].seq != acked_seq || lp->tries == 0) {
        return;
    }
    push_event(lp, LINK_PEER_DELIVERED, acked_seq);
    pop_head(lp);
}

bool link_peer_on_rx(link_peer_t *lp, uint16_t seq, uint32_t now_ms)
{
    if (lp == NULL) {
        return false;
    }
    lp->last_heard = now_ms;
    if (!lp->alive) {
        lp->alive = true;
        push_event(lp, LINK_PEER_UP, 0);
    }
    if (seq == 0) {
        return true;
    }
    for (uint8_t i = 0; i < lp->seen_count; i++) {
        if (lp->seen[i] == seq) {
            return false;
        }
    }
    lp->seen[lp->seen_next] = seq;
    lp->seen_next = (uint8_t)((lp->seen_next + 1) % LINK_PEER_DEDUPE);
    if (lp->seen_count < LINK_PEER_DEDUPE) {
        lp->seen_count++;
    }
    return true;
}

void link_peer_reset_rx(link_peer_t *lp)
{
    if (lp != NULL) {
        lp->seen_count = 0;
        lp->seen_next = 0;
    }
}

void link_peer_tick(link_peer_t *lp, uint32_t now_ms)
{
    if (lp == NULL) {
        return;
    }
    check_give_up(lp, now_ms);
    if (lp->alive && (uint32_t)(now_ms - lp->last_heard) >= lp->cfg.alive_timeout_ms) {
        lp->alive = false;
        push_event(lp, LINK_PEER_DOWN, 0);
    }
}

bool link_peer_alive(const link_peer_t *lp)
{
    return lp != NULL && lp->alive;
}

size_t link_peer_pending(const link_peer_t *lp)
{
    return lp ? lp->q_count : 0;
}

bool link_peer_heartbeat_due(link_peer_t *lp, uint32_t now_ms)
{
    if (lp == NULL) {
        return false;
    }
    if (!lp->hb_started || (uint32_t)(now_ms - lp->hb_last) >= lp->cfg.heartbeat_ms) {
        lp->hb_started = true;
        lp->hb_last = now_ms;
        return true;
    }
    return false;
}

bool link_peer_take_event(link_peer_t *lp, link_peer_event_t *out)
{
    if (lp == NULL || out == NULL || lp->ev_count == 0) {
        return false;
    }
    *out = lp->events[lp->ev_head];
    lp->ev_head = (uint8_t)((lp->ev_head + 1) % LINK_PEER_EVENTS);
    lp->ev_count--;
    return true;
}
