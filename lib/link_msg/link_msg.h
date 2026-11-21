/*
 * link_msg – the messages between the nodes, as bytes (pure C codec for the
 * ESP-NOW link; no radio).
 *
 * Requirements: LINK-01 ... LINK-04 (see the requirements doc).
 * Tests:        test/test_link_msg/
 *
 * Bedside, speaker, gun and targets talk in small frames over ESP-NOW. This
 * module turns a message struct into a frame and back. Delivery (acks,
 * retries, duplicates, liveness) is lib/link_peer. docs/PROTOCOL.md lists
 * who sends what and when.
 *
 * Frame (all multi-byte fields little-endian)
 *   byte 0     0xBA            magic
 *   byte 1     1               protocol version (LINK_PROTO_VERSION)
 *   byte 2     type            link_type_t
 *   byte 3     flags           bit 0 = LINK_FLAG_ACK_REQ, other bits 0
 *   byte 4-5   seq             sender's sequence number; 0 for frames that
 *                              are not acknowledged
 *   byte 6...  payload         fixed layout per type, see the structs below:
 *                              the fields in struct order, without padding;
 *                              bool as one byte 0/1; name as 12 bytes and
 *                              folder as 23 bytes, NUL-padded (no NUL on
 *                              the wire when the text fills the field)
 *   The sender is known from the ESP-NOW source address, so no node id
 *   travels in the frame.
 *
 * Payload sizes: HELLO 18, ACK 2, HEARTBEAT 8, TIME_ALARM 18, RING 7,
 * RING_STATE 5, AUDIO 28, GAME_CFG 4, TARGET_CMD 4, TARGET_EVT 5, GUN_EVT 3,
 * FOLDER 25.
 *
 * link_msg_encode()
 *   Returns the frame length, or 0 if msg or out is NULL, the type is
 *   unknown or out is too small (LINK_FRAME_MAX always fits). Flag bits
 *   other than LINK_FLAG_ACK_REQ are sent as 0, and so are audio.flags bits
 *   other than LINK_AUDIO_FLAG_RESUME / _KEEP. hello.name, audio.folder and
 *   folder.name are sent up to their first NUL or the field's length.
 *
 * link_msg_decode()
 *   Returns false (out untouched) for: NULL, fewer than 6 bytes, wrong
 *   magic, a version other than 1, an unknown type, or a payload shorter
 *   than the type needs. Extra bytes after the payload are ignored, and
 *   unknown flag bits are dropped (in the header and in audio.flags): a
 *   later version may append fields without breaking older nodes.
 *   hello.name, audio.folder and folder.name always come back
 *   NUL-terminated.
 */
#ifndef LINK_MSG_H
#define LINK_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINK_MAGIC 0xBA
#define LINK_PROTO_VERSION 1
#define LINK_HEADER_SIZE 6
#define LINK_FRAME_MAX 34 /* longest frame of this version (AUDIO) */
#define LINK_FLAG_ACK_REQ 0x01
#define LINK_NAME_LEN 12
#define LINK_FOLDER_LEN 23 /* folder names on the speaker's SD card */

typedef enum {
    LINK_HELLO = 1,   /* any node, at start and when asked: who am I            */
    LINK_ACK,         /* answer to a frame with LINK_FLAG_ACK_REQ               */
    LINK_HEARTBEAT,   /* every node, periodically                               */
    LINK_TIME_ALARM,  /* bedside -> all: time of day and the next alarm         */
    LINK_RING,        /* bedside -> speaker: start, snooze, stop, escalate      */
    LINK_RING_STATE,  /* speaker -> bedside: what the alarm sound is doing      */
    LINK_AUDIO,       /* bedside -> speaker: play, stop, next, volume           */
    LINK_GAME_CFG,    /* bedside -> gun, targets: difficulty values they need   */
    LINK_TARGET_CMD,  /* bedside -> target                                      */
    LINK_TARGET_EVT,  /* target -> bedside                                      */
    LINK_GUN_EVT,     /* gun -> bedside                                         */
    LINK_FOLDER,      /* speaker -> bedside: one folder name of the SD card     */
} link_type_t;

typedef enum {
    LINK_NODE_BEDSIDE = 1,
    LINK_NODE_SPEAKER,
    LINK_NODE_GUN,
    LINK_NODE_TARGET,
} link_node_t;

/* hello.caps bits: what a node can do, so new kinds need no protocol change */
#define LINK_CAP_AUDIO 0x0001
#define LINK_CAP_LIGHT 0x0002
#define LINK_CAP_LASER 0x0004
#define LINK_CAP_HIT_SENSOR 0x0008
#define LINK_CAP_MOVING 0x0010
#define LINK_CAP_MAGAZINE 0x0020
#define LINK_CAP_BATTERY 0x0040

typedef struct {
    uint8_t node;                 /* link_node_t */
    uint16_t caps;
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    char name[LINK_NAME_LEN + 1]; /* e.g. "target-2" */
} link_hello_t;

typedef struct {
    uint16_t acked_seq;
} link_ack_t;

typedef struct {
    uint32_t uptime_s;
    uint16_t battery_mv; /* 0 = mains powered */
    int8_t rssi_dbm;     /* of the last frame received from the peer, 0 = unknown */
    uint8_t state;       /* node-specific, see docs/PROTOCOL.md */
} link_heartbeat_t;

typedef struct {
    uint32_t version;     /* of the alarm copy; older ones are discarded      */
    uint32_t utc_now_s;   /* bedside clock at sending                         */
    uint32_t alarm_utc_s; /* next alarm, 0 = none                             */
    uint8_t sound;        /* alarm sound number on the speaker's SD card      */
    uint8_t volume;       /* 1 ... 32                                         */
    uint16_t fade_s;      /* sound fade-in                                    */
    uint8_t ring_max_min; /* maximum ring time                                */
    bool game;            /* the alarm game is on for this alarm              */
} link_time_alarm_t;

typedef enum {
    LINK_RING_START = 1,
    LINK_RING_SNOOZE,   /* arg = minutes */
    LINK_RING_STOP,
    LINK_RING_ESCALATE, /* arg = level 0 ... 5 */
} link_ring_action_t;

typedef struct {
    uint8_t action; /* link_ring_action_t */
    uint32_t version;
    uint16_t arg;
} link_ring_t;

typedef struct {
    uint8_t state;  /* armed_state_t of lib/armed_alarm */
    uint8_t reason; /* armed_stop_t of the last stop    */
    uint8_t spare;
    uint16_t ring_s; /* seconds the current ring has lasted */
} link_ring_state_t;

/*
 * AUDIO (bedside -> speaker). The bedside decides; the speaker plays.
 *   STOP    fade out over fade_ds, then stop (and power the amplifier down).
 *   PLAY    play `folder`, shuffled, fading in over fade_ds to `volume`.
 *           Replaces whatever plays. flags:
 *             RESUME  continue where the last KEEP session of this folder
 *                     stopped (lib/resume_point); a fresh shuffle if there is
 *                     none or it does not match the card any more. A RESUME
 *                     PLAY of the folder that is playing (also while it fades
 *                     out) cancels the fade and plays on from where it is.
 *             KEEP    remember where this session stops.
 *   VOLUME  set `volume` (applies under any running fade).
 *   NEXT    next track of the running session.
 *   Fields a command does not use are sent as 0.
 */
typedef enum {
    LINK_AUDIO_STOP = 1,
    LINK_AUDIO_PLAY,
    LINK_AUDIO_VOLUME,
    LINK_AUDIO_NEXT,
} link_audio_cmd_t;

#define LINK_AUDIO_FLAG_RESUME 0x01
#define LINK_AUDIO_FLAG_KEEP 0x02

typedef struct {
    uint8_t cmd;     /* link_audio_cmd_t                       */
    uint8_t flags;   /* LINK_AUDIO_FLAG_*                      */
    uint8_t volume;  /* step 0 ... 32 (lib/volume)             */
    uint16_t fade_ds; /* fade time in tenths of a second        */
    char folder[LINK_FOLDER_LEN + 1];
} link_audio_t;

/*
 * FOLDER (speaker -> bedside): the folders in the root of the speaker's SD
 * card, one per frame, in the order the speaker lists them, after every scan
 * and whenever the bedside says HELLO. index 0 ... count-1; a card without
 * folders is sent as one frame with count 0, index 0, name "".
 */
typedef struct {
    uint8_t index;
    uint8_t count;
    char name[LINK_FOLDER_LEN + 1];
} link_folder_t;

typedef struct {
    uint8_t sights; /* 0 ... 100 percent      */
    uint8_t zone;   /* 1 ... 3                */
    uint8_t ammo;   /* 0 = no magazine        */
    uint8_t gun_id; /* 0 ... 15, the gun's code */
} link_game_cfg_t;

typedef enum {
    LINK_TARGET_LIGHT = 1, /* arg_ms = lit time                 */
    LINK_TARGET_OFF,
    LINK_TARGET_IDENTIFY,  /* blink, arg_ms = how long          */
    LINK_TARGET_CALIBRATE, /* measure room light, set threshold */
} link_target_cmd_id_t;

typedef struct {
    uint8_t cmd;     /* link_target_cmd_id_t */
    uint8_t round;   /* echoed in the event  */
    uint16_t arg_ms;
} link_target_cmd_t;

typedef enum {
    LINK_TARGET_HIT = 1,
    LINK_TARGET_TIMEOUT,
    LINK_TARGET_CALIBRATED, /* value = ambient level */
} link_target_evt_id_t;

typedef struct {
    uint8_t evt;    /* link_target_evt_id_t */
    uint8_t round;
    uint8_t gun_id;
    uint16_t value; /* hit: signal strength 0 ... 1000 */
} link_target_evt_t;

typedef enum {
    LINK_GUN_SHOT = 1,
    LINK_GUN_DRY,
    LINK_GUN_RELOAD,
    LINK_GUN_DEMO_REQUEST,
} link_gun_evt_id_t;

typedef struct {
    uint8_t evt; /* link_gun_evt_id_t */
    uint8_t shot;
    uint8_t ammo_left; /* 255 = no magazine */
} link_gun_evt_t;

typedef struct {
    link_type_t type;
    uint8_t flags;
    uint16_t seq;
    union {
        link_hello_t hello;
        link_ack_t ack;
        link_heartbeat_t heartbeat;
        link_time_alarm_t time_alarm;
        link_ring_t ring;
        link_ring_state_t ring_state;
        link_audio_t audio;
        link_game_cfg_t game_cfg;
        link_target_cmd_t target_cmd;
        link_target_evt_t target_evt;
        link_gun_evt_t gun_evt;
        link_folder_t folder;
    } u;
} link_msg_t;

size_t link_msg_encode(const link_msg_t *msg, uint8_t *out, size_t out_size);

bool link_msg_decode(const uint8_t *frame, size_t len, link_msg_t *out);

/* Payload size of a type, or 0 for an unknown type. */
size_t link_msg_payload_size(link_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* LINK_MSG_H */
