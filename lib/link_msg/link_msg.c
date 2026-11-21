/*
 * link_msg – ESP-NOW frames to message structs and back.
 * Contract in link_msg.h, tests in test/test_link_msg/.
 */
#include "link_msg.h"

#include <string.h>

/* ---- little-endian writer / reader ------------------------------------ */

typedef struct {
    uint8_t *p;
} wr_t;

static void w8(wr_t *w, uint8_t v)
{
    *w->p++ = v;
}

static void w16(wr_t *w, uint16_t v)
{
    w8(w, (uint8_t)(v & 0xFF));
    w8(w, (uint8_t)(v >> 8));
}

static void w32(wr_t *w, uint32_t v)
{
    w16(w, (uint16_t)(v & 0xFFFF));
    w16(w, (uint16_t)(v >> 16));
}

typedef struct {
    const uint8_t *p;
} rd_t;

static uint8_t r8(rd_t *r)
{
    return *r->p++;
}

static uint16_t r16(rd_t *r)
{
    const uint16_t lo = r8(r);
    return (uint16_t)(lo | ((uint16_t)r8(r) << 8));
}

static uint32_t r32(rd_t *r)
{
    const uint32_t lo = r16(r);
    return lo | ((uint32_t)r16(r) << 16);
}

/* ---- sizes -------------------------------------------------------------- */

size_t link_msg_payload_size(link_type_t type)
{
    switch (type) {
    case LINK_HELLO:
        return 18;
    case LINK_ACK:
        return 2;
    case LINK_HEARTBEAT:
        return 8;
    case LINK_TIME_ALARM:
        return 18;
    case LINK_RING:
        return 7;
    case LINK_RING_STATE:
        return 5;
    case LINK_AUDIO:
        return 28;
    case LINK_GAME_CFG:
        return 4;
    case LINK_TARGET_CMD:
        return 4;
    case LINK_TARGET_EVT:
        return 5;
    case LINK_GUN_EVT:
        return 3;
    case LINK_FOLDER:
        return 25;
    default:
        return 0;
    }
}

/* ---- encode ------------------------------------------------------------- */

/* Text field of n bytes: the text up to its first NUL, then NUL padding. */
static void w_text(wr_t *w, const char *s, int n)
{
    bool ended = false;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\0') {
            ended = true;
        }
        w8(w, ended ? 0 : (uint8_t)s[i]);
    }
}

static void encode_payload(const link_msg_t *m, wr_t *w)
{
    switch (m->type) {
    case LINK_HELLO: {
        const link_hello_t *h = &m->u.hello;
        w8(w, h->node);
        w16(w, h->caps);
        w8(w, h->fw_major);
        w8(w, h->fw_minor);
        w8(w, h->fw_patch);
        bool ended = false;
        for (int i = 0; i < LINK_NAME_LEN; i++) {
            if (h->name[i] == '\0') {
                ended = true;
            }
            w8(w, ended ? 0 : (uint8_t)h->name[i]);
        }
        break;
    }
    case LINK_ACK:
        w16(w, m->u.ack.acked_seq);
        break;
    case LINK_HEARTBEAT:
        w32(w, m->u.heartbeat.uptime_s);
        w16(w, m->u.heartbeat.battery_mv);
        w8(w, (uint8_t)m->u.heartbeat.rssi_dbm);
        w8(w, m->u.heartbeat.state);
        break;
    case LINK_TIME_ALARM: {
        const link_time_alarm_t *t = &m->u.time_alarm;
        w32(w, t->version);
        w32(w, t->utc_now_s);
        w32(w, t->alarm_utc_s);
        w8(w, t->sound);
        w8(w, t->volume);
        w16(w, t->fade_s);
        w8(w, t->ring_max_min);
        w8(w, t->game ? 1 : 0);
        break;
    }
    case LINK_RING:
        w8(w, m->u.ring.action);
        w32(w, m->u.ring.version);
        w16(w, m->u.ring.arg);
        break;
    case LINK_RING_STATE:
        w8(w, m->u.ring_state.state);
        w8(w, m->u.ring_state.reason);
        w8(w, m->u.ring_state.spare);
        w16(w, m->u.ring_state.ring_s);
        break;
    case LINK_AUDIO:
        w8(w, m->u.audio.cmd);
        w8(w, m->u.audio.flags & (LINK_AUDIO_FLAG_RESUME | LINK_AUDIO_FLAG_KEEP));
        w8(w, m->u.audio.volume);
        w16(w, m->u.audio.fade_ds);
        w_text(w, m->u.audio.folder, LINK_FOLDER_LEN);
        break;
    case LINK_GAME_CFG:
        w8(w, m->u.game_cfg.sights);
        w8(w, m->u.game_cfg.zone);
        w8(w, m->u.game_cfg.ammo);
        w8(w, m->u.game_cfg.gun_id);
        break;
    case LINK_TARGET_CMD:
        w8(w, m->u.target_cmd.cmd);
        w8(w, m->u.target_cmd.round);
        w16(w, m->u.target_cmd.arg_ms);
        break;
    case LINK_TARGET_EVT:
        w8(w, m->u.target_evt.evt);
        w8(w, m->u.target_evt.round);
        w8(w, m->u.target_evt.gun_id);
        w16(w, m->u.target_evt.value);
        break;
    case LINK_GUN_EVT:
        w8(w, m->u.gun_evt.evt);
        w8(w, m->u.gun_evt.shot);
        w8(w, m->u.gun_evt.ammo_left);
        break;
    case LINK_FOLDER:
        w8(w, m->u.folder.index);
        w8(w, m->u.folder.count);
        w_text(w, m->u.folder.name, LINK_FOLDER_LEN);
        break;
    default:
        break;
    }
}

size_t link_msg_encode(const link_msg_t *msg, uint8_t *out, size_t out_size)
{
    if (msg == NULL || out == NULL) {
        return 0;
    }
    const size_t payload = link_msg_payload_size(msg->type);
    if (payload == 0) {
        return 0;
    }
    const size_t total = LINK_HEADER_SIZE + payload;
    if (out_size < total) {
        return 0;
    }
    wr_t w = {out};
    w8(&w, LINK_MAGIC);
    w8(&w, LINK_PROTO_VERSION);
    w8(&w, (uint8_t)msg->type);
    w8(&w, msg->flags & LINK_FLAG_ACK_REQ);
    w16(&w, msg->seq);
    encode_payload(msg, &w);
    return total;
}

/* ---- decode ------------------------------------------------------------- */

static void r_text(rd_t *r, char *s, int n)
{
    for (int i = 0; i < n; i++) {
        s[i] = (char)r8(r);
    }
    s[n] = '\0';
}

static void decode_payload(link_msg_t *m, rd_t *r)
{
    switch (m->type) {
    case LINK_HELLO: {
        link_hello_t *h = &m->u.hello;
        h->node = r8(r);
        h->caps = r16(r);
        h->fw_major = r8(r);
        h->fw_minor = r8(r);
        h->fw_patch = r8(r);
        for (int i = 0; i < LINK_NAME_LEN; i++) {
            h->name[i] = (char)r8(r);
        }
        h->name[LINK_NAME_LEN] = '\0';
        break;
    }
    case LINK_ACK:
        m->u.ack.acked_seq = r16(r);
        break;
    case LINK_HEARTBEAT:
        m->u.heartbeat.uptime_s = r32(r);
        m->u.heartbeat.battery_mv = r16(r);
        m->u.heartbeat.rssi_dbm = (int8_t)r8(r);
        m->u.heartbeat.state = r8(r);
        break;
    case LINK_TIME_ALARM: {
        link_time_alarm_t *t = &m->u.time_alarm;
        t->version = r32(r);
        t->utc_now_s = r32(r);
        t->alarm_utc_s = r32(r);
        t->sound = r8(r);
        t->volume = r8(r);
        t->fade_s = r16(r);
        t->ring_max_min = r8(r);
        t->game = r8(r) != 0;
        break;
    }
    case LINK_RING:
        m->u.ring.action = r8(r);
        m->u.ring.version = r32(r);
        m->u.ring.arg = r16(r);
        break;
    case LINK_RING_STATE:
        m->u.ring_state.state = r8(r);
        m->u.ring_state.reason = r8(r);
        m->u.ring_state.spare = r8(r);
        m->u.ring_state.ring_s = r16(r);
        break;
    case LINK_AUDIO:
        m->u.audio.cmd = r8(r);
        m->u.audio.flags = r8(r) & (LINK_AUDIO_FLAG_RESUME | LINK_AUDIO_FLAG_KEEP);
        m->u.audio.volume = r8(r);
        m->u.audio.fade_ds = r16(r);
        r_text(r, m->u.audio.folder, LINK_FOLDER_LEN);
        break;
    case LINK_GAME_CFG:
        m->u.game_cfg.sights = r8(r);
        m->u.game_cfg.zone = r8(r);
        m->u.game_cfg.ammo = r8(r);
        m->u.game_cfg.gun_id = r8(r);
        break;
    case LINK_TARGET_CMD:
        m->u.target_cmd.cmd = r8(r);
        m->u.target_cmd.round = r8(r);
        m->u.target_cmd.arg_ms = r16(r);
        break;
    case LINK_TARGET_EVT:
        m->u.target_evt.evt = r8(r);
        m->u.target_evt.round = r8(r);
        m->u.target_evt.gun_id = r8(r);
        m->u.target_evt.value = r16(r);
        break;
    case LINK_GUN_EVT:
        m->u.gun_evt.evt = r8(r);
        m->u.gun_evt.shot = r8(r);
        m->u.gun_evt.ammo_left = r8(r);
        break;
    case LINK_FOLDER:
        m->u.folder.index = r8(r);
        m->u.folder.count = r8(r);
        r_text(r, m->u.folder.name, LINK_FOLDER_LEN);
        break;
    default:
        break;
    }
}

bool link_msg_decode(const uint8_t *frame, size_t len, link_msg_t *out)
{
    if (frame == NULL || out == NULL || len < LINK_HEADER_SIZE) {
        return false;
    }
    if (frame[0] != LINK_MAGIC || frame[1] != LINK_PROTO_VERSION) {
        return false;
    }
    const link_type_t type = (link_type_t)frame[2];
    const size_t payload = link_msg_payload_size(type);
    if (payload == 0 || len - LINK_HEADER_SIZE < payload) {
        return false;
    }
    link_msg_t m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.flags = frame[3] & LINK_FLAG_ACK_REQ;
    m.seq = (uint16_t)(frame[4] | (frame[5] << 8));
    rd_t r = {frame + LINK_HEADER_SIZE};
    decode_payload(&m, &r);
    *out = m;
    return true;
}
