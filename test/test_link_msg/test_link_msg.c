/*
 * Tests for lib/link_msg – requirements LINK-01 ... LINK-04.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "link_msg.h"

static link_msg_t in;
static link_msg_t out;
static uint8_t frame[64];

void setUp(void)
{
    memset(&in, 0, sizeof(in));
    memset(&out, 0xEE, sizeof(out));
    memset(frame, 0xCC, sizeof(frame));
}

void tearDown(void) {}

static size_t encode(void)
{
    memset(frame, 0xCC, sizeof(frame));
    const size_t n = link_msg_encode(&in, frame, LINK_FRAME_MAX);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "encode failed for a valid message");
    TEST_ASSERT_EQUAL_size_t(LINK_HEADER_SIZE + link_msg_payload_size(in.type), n);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xCC, frame[n], "nothing is written past the frame");
    return n;
}

static void round_trip(void)
{
    const size_t n = encode();
    memset(&out, 0xEE, sizeof(out));
    TEST_ASSERT_TRUE(link_msg_decode(frame, n, &out));
    TEST_ASSERT_EQUAL_INT(in.type, out.type);
    TEST_ASSERT_EQUAL_HEX8(in.flags, out.flags);
    TEST_ASSERT_EQUAL_UINT16(in.seq, out.seq);
}

static void test_payload_sizes(void)
{
    const size_t sizes[] = {18, 2, 8, 18, 7, 5, 4, 4, 4, 5, 3};
    for (int t = LINK_HELLO; t <= LINK_GUN_EVT; t++) {
        TEST_ASSERT_EQUAL_size_t(sizes[t - 1], link_msg_payload_size((link_type_t)t));
        TEST_ASSERT_TRUE(LINK_HEADER_SIZE + sizes[t - 1] <= LINK_FRAME_MAX);
    }
    TEST_ASSERT_EQUAL_size_t(0, link_msg_payload_size((link_type_t)0));
    TEST_ASSERT_EQUAL_size_t(0, link_msg_payload_size((link_type_t)12));
}

static void test_hello_bytes(void)
{
    in.type = LINK_HELLO;
    in.flags = LINK_FLAG_ACK_REQ;
    in.seq = 0x1234;
    in.u.hello.node = LINK_NODE_TARGET;
    in.u.hello.caps = LINK_CAP_HIT_SENSOR | LINK_CAP_LIGHT | LINK_CAP_BATTERY;
    in.u.hello.fw_major = 2;
    in.u.hello.fw_minor = 1;
    in.u.hello.fw_patch = 7;
    strcpy(in.u.hello.name, "target-2");
    const uint8_t expected[] = {0xBA, 0x01, 0x01, 0x01, 0x34, 0x12, 0x04, 0x4A, 0x00, 0x02, 0x01, 0x07,
                                't',  'a',  'r',  'g',  'e',  't',  '-',  '2',  0,    0,    0,    0};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), encode());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(LINK_NODE_TARGET, out.u.hello.node);
    TEST_ASSERT_EQUAL_HEX16(0x004A, out.u.hello.caps);
    TEST_ASSERT_EQUAL_UINT8(2, out.u.hello.fw_major);
    TEST_ASSERT_EQUAL_UINT8(1, out.u.hello.fw_minor);
    TEST_ASSERT_EQUAL_UINT8(7, out.u.hello.fw_patch);
    TEST_ASSERT_EQUAL_STRING("target-2", out.u.hello.name);
}

static void test_hello_name_of_full_length(void)
{
    in.type = LINK_HELLO;
    in.u.hello.node = LINK_NODE_GUN;
    memcpy(in.u.hello.name, "twelve-chars", 13);
    round_trip();
    TEST_ASSERT_EQUAL_STRING("twelve-chars", out.u.hello.name);
    TEST_ASSERT_EQUAL_HEX8('s', frame[LINK_HEADER_SIZE + 17]);
}

static void test_time_alarm_bytes(void)
{
    in.type = LINK_TIME_ALARM;
    in.seq = 7;
    in.flags = LINK_FLAG_ACK_REQ;
    in.u.time_alarm.version = 0x01020304;
    in.u.time_alarm.utc_now_s = 1795000000u; /* 0x6AFD86C0 */
    in.u.time_alarm.alarm_utc_s = 1795030000u; /* 0x6AFDFBF0 */
    in.u.time_alarm.sound = 3;
    in.u.time_alarm.volume = 16;
    in.u.time_alarm.fade_s = 300;
    in.u.time_alarm.ring_max_min = 15;
    in.u.time_alarm.game = true;
    const uint8_t expected[] = {0xBA, 0x01, 0x04, 0x01, 0x07, 0x00, 0x04, 0x03, 0x02, 0x01, 0xC0, 0x86,
                                0xFD, 0x6A, 0xF0, 0xFB, 0xFD, 0x6A, 0x03, 0x10, 0x2C, 0x01, 0x0F, 0x01};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), encode());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
    round_trip();
    TEST_ASSERT_EQUAL_UINT32(0x01020304, out.u.time_alarm.version);
    TEST_ASSERT_EQUAL_UINT32(1795000000u, out.u.time_alarm.utc_now_s);
    TEST_ASSERT_EQUAL_UINT32(1795030000u, out.u.time_alarm.alarm_utc_s);
    TEST_ASSERT_EQUAL_UINT8(3, out.u.time_alarm.sound);
    TEST_ASSERT_EQUAL_UINT8(16, out.u.time_alarm.volume);
    TEST_ASSERT_EQUAL_UINT16(300, out.u.time_alarm.fade_s);
    TEST_ASSERT_EQUAL_UINT8(15, out.u.time_alarm.ring_max_min);
    TEST_ASSERT_TRUE(out.u.time_alarm.game);
}

static void test_ack_and_heartbeat(void)
{
    in.type = LINK_ACK;
    in.seq = 0;
    in.u.ack.acked_seq = 0xFFFE;
    const uint8_t expected[] = {0xBA, 0x01, 0x02, 0x00, 0x00, 0x00, 0xFE, 0xFF};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), encode());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
    round_trip();
    TEST_ASSERT_EQUAL_UINT16(0xFFFE, out.u.ack.acked_seq);

    setUp();
    in.type = LINK_HEARTBEAT;
    in.u.heartbeat.uptime_s = 90061;
    in.u.heartbeat.battery_mv = 3987;
    in.u.heartbeat.rssi_dbm = -67;
    in.u.heartbeat.state = 2;
    round_trip();
    TEST_ASSERT_EQUAL_UINT32(90061, out.u.heartbeat.uptime_s);
    TEST_ASSERT_EQUAL_UINT16(3987, out.u.heartbeat.battery_mv);
    TEST_ASSERT_EQUAL_INT8(-67, out.u.heartbeat.rssi_dbm);
    TEST_ASSERT_EQUAL_UINT8(2, out.u.heartbeat.state);
    TEST_ASSERT_EQUAL_HEX8(0xBD, frame[LINK_HEADER_SIZE + 6]); /* -67 as one byte */
}

static void test_ring_messages(void)
{
    in.type = LINK_RING;
    in.flags = LINK_FLAG_ACK_REQ;
    in.seq = 513;
    in.u.ring.action = LINK_RING_SNOOZE;
    in.u.ring.version = 77;
    in.u.ring.arg = 9;
    const uint8_t expected[] = {0xBA, 0x01, 0x05, 0x01, 0x01, 0x02, 0x02, 0x4D, 0x00, 0x00, 0x00, 0x09, 0x00};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), encode());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(LINK_RING_SNOOZE, out.u.ring.action);
    TEST_ASSERT_EQUAL_UINT32(77, out.u.ring.version);
    TEST_ASSERT_EQUAL_UINT16(9, out.u.ring.arg);

    setUp();
    in.type = LINK_RING_STATE;
    in.u.ring_state.state = 2;
    in.u.ring_state.reason = 3;
    in.u.ring_state.ring_s = 600;
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(2, out.u.ring_state.state);
    TEST_ASSERT_EQUAL_UINT8(3, out.u.ring_state.reason);
    TEST_ASSERT_EQUAL_UINT16(600, out.u.ring_state.ring_s);

    setUp();
    in.type = LINK_AUDIO;
    in.u.audio.cmd = LINK_AUDIO_SLEEP_TIMER;
    in.u.audio.arg1 = 45;
    in.u.audio.arg2 = 120;
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(LINK_AUDIO_SLEEP_TIMER, out.u.audio.cmd);
    TEST_ASSERT_EQUAL_UINT8(45, out.u.audio.arg1);
    TEST_ASSERT_EQUAL_UINT16(120, out.u.audio.arg2);
}

static void test_game_messages(void)
{
    in.type = LINK_GAME_CFG;
    in.u.game_cfg.sights = 30;
    in.u.game_cfg.zone = 2;
    in.u.game_cfg.ammo = 6;
    in.u.game_cfg.gun_id = 11;
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(30, out.u.game_cfg.sights);
    TEST_ASSERT_EQUAL_UINT8(2, out.u.game_cfg.zone);
    TEST_ASSERT_EQUAL_UINT8(6, out.u.game_cfg.ammo);
    TEST_ASSERT_EQUAL_UINT8(11, out.u.game_cfg.gun_id);

    setUp();
    in.type = LINK_TARGET_CMD;
    in.flags = LINK_FLAG_ACK_REQ;
    in.seq = 9;
    in.u.target_cmd.cmd = LINK_TARGET_LIGHT;
    in.u.target_cmd.round = 4;
    in.u.target_cmd.arg_ms = 8000;
    const uint8_t expected[] = {0xBA, 0x01, 0x09, 0x01, 0x09, 0x00, 0x01, 0x04, 0x40, 0x1F};
    TEST_ASSERT_EQUAL_size_t(sizeof(expected), encode());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, frame, sizeof(expected));
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(LINK_TARGET_LIGHT, out.u.target_cmd.cmd);
    TEST_ASSERT_EQUAL_UINT8(4, out.u.target_cmd.round);
    TEST_ASSERT_EQUAL_UINT16(8000, out.u.target_cmd.arg_ms);

    setUp();
    in.type = LINK_TARGET_EVT;
    in.u.target_evt.evt = LINK_TARGET_HIT;
    in.u.target_evt.round = 4;
    in.u.target_evt.gun_id = 11;
    in.u.target_evt.value = 870;
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(LINK_TARGET_HIT, out.u.target_evt.evt);
    TEST_ASSERT_EQUAL_UINT8(4, out.u.target_evt.round);
    TEST_ASSERT_EQUAL_UINT8(11, out.u.target_evt.gun_id);
    TEST_ASSERT_EQUAL_UINT16(870, out.u.target_evt.value);

    setUp();
    in.type = LINK_GUN_EVT;
    in.u.gun_evt.evt = LINK_GUN_DRY;
    in.u.gun_evt.shot = 15;
    in.u.gun_evt.ammo_left = 255;
    round_trip();
    TEST_ASSERT_EQUAL_UINT8(LINK_GUN_DRY, out.u.gun_evt.evt);
    TEST_ASSERT_EQUAL_UINT8(15, out.u.gun_evt.shot);
    TEST_ASSERT_EQUAL_UINT8(255, out.u.gun_evt.ammo_left);
}

static void test_encode_errors(void)
{
    in.type = LINK_ACK;
    TEST_ASSERT_EQUAL_size_t(8, encode());
    TEST_ASSERT_EQUAL_size_t(0, link_msg_encode(&in, frame, 7));
    TEST_ASSERT_EQUAL_size_t(8, link_msg_encode(&in, frame, 8));
    TEST_ASSERT_EQUAL_size_t(0, link_msg_encode(NULL, frame, sizeof(frame)));
    TEST_ASSERT_EQUAL_size_t(0, link_msg_encode(&in, NULL, sizeof(frame)));
    in.type = (link_type_t)0;
    TEST_ASSERT_EQUAL_size_t(0, link_msg_encode(&in, frame, sizeof(frame)));
    in.type = (link_type_t)12;
    TEST_ASSERT_EQUAL_size_t(0, link_msg_encode(&in, frame, sizeof(frame)));
}

static void test_unknown_flag_bits_are_not_sent_or_kept(void)
{
    in.type = LINK_ACK;
    in.flags = 0xFF;
    encode();
    TEST_ASSERT_EQUAL_HEX8(0x01, frame[3]);
    frame[3] = 0xFE; /* a newer sender with other flags, no ack request */
    TEST_ASSERT_TRUE(link_msg_decode(frame, 8, &out));
    TEST_ASSERT_EQUAL_HEX8(0x00, out.flags);
    frame[3] = 0x81;
    TEST_ASSERT_TRUE(link_msg_decode(frame, 8, &out));
    TEST_ASSERT_EQUAL_HEX8(LINK_FLAG_ACK_REQ, out.flags);
}

static void test_decode_rejects_bad_frames(void)
{
    in.type = LINK_RING;
    in.u.ring.action = LINK_RING_STOP;
    const size_t n = encode();
    const link_msg_t untouched = out;
    TEST_ASSERT_TRUE_MESSAGE(link_msg_decode(frame, n, &out), "the good frame decodes");
    out = untouched;

    for (size_t len = 0; len < n; len++) {
        TEST_ASSERT_FALSE_MESSAGE(link_msg_decode(frame, len, &out), "truncated frame");
    }
    uint8_t bad[32];
    memcpy(bad, frame, n);
    bad[0] = 0xBB;
    TEST_ASSERT_FALSE_MESSAGE(link_msg_decode(bad, n, &out), "wrong magic");
    memcpy(bad, frame, n);
    bad[1] = 2;
    TEST_ASSERT_FALSE_MESSAGE(link_msg_decode(bad, n, &out), "other protocol version");
    bad[1] = 0;
    TEST_ASSERT_FALSE(link_msg_decode(bad, n, &out));
    memcpy(bad, frame, n);
    bad[2] = 0;
    TEST_ASSERT_FALSE_MESSAGE(link_msg_decode(bad, n, &out), "type 0");
    bad[2] = 12;
    TEST_ASSERT_FALSE_MESSAGE(link_msg_decode(bad, n, &out), "unknown type");
    bad[2] = LINK_HELLO; /* needs 18 bytes of payload, the frame has 7 */
    TEST_ASSERT_FALSE_MESSAGE(link_msg_decode(bad, n, &out), "payload too short for the type");
    TEST_ASSERT_FALSE(link_msg_decode(NULL, n, &out));
    TEST_ASSERT_FALSE(link_msg_decode(frame, n, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &out, sizeof(out));
}

static void test_extra_bytes_are_ignored(void)
{
    in.type = LINK_GUN_EVT;
    in.seq = 42;
    in.u.gun_evt.evt = LINK_GUN_SHOT;
    in.u.gun_evt.shot = 3;
    in.u.gun_evt.ammo_left = 5;
    const size_t n = encode();
    frame[n] = 0x99; /* a field a later version appended */
    frame[n + 1] = 0x98;
    TEST_ASSERT_TRUE(link_msg_decode(frame, n + 2, &out));
    TEST_ASSERT_EQUAL_UINT16(42, out.seq);
    TEST_ASSERT_EQUAL_UINT8(LINK_GUN_SHOT, out.u.gun_evt.evt);
    TEST_ASSERT_EQUAL_UINT8(3, out.u.gun_evt.shot);
    TEST_ASSERT_EQUAL_UINT8(5, out.u.gun_evt.ammo_left);
}

static void test_bool_and_name_from_foreign_bytes(void)
{
    in.type = LINK_TIME_ALARM;
    size_t n = encode();
    frame[LINK_HEADER_SIZE + 17] = 0x02; /* any non-zero byte is true */
    TEST_ASSERT_TRUE(link_msg_decode(frame, n, &out));
    TEST_ASSERT_TRUE(out.u.time_alarm.game == true);
    frame[LINK_HEADER_SIZE + 17] = 0x00;
    TEST_ASSERT_TRUE(link_msg_decode(frame, n, &out));
    TEST_ASSERT_FALSE(out.u.time_alarm.game);

    setUp();
    in.type = LINK_HELLO;
    n = encode();
    memset(frame + LINK_HEADER_SIZE + 6, 'x', LINK_NAME_LEN); /* no NUL on the wire */
    TEST_ASSERT_TRUE(link_msg_decode(frame, n, &out));
    TEST_ASSERT_EQUAL_size_t(LINK_NAME_LEN, strlen(out.u.hello.name));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_payload_sizes);
    RUN_TEST(test_hello_bytes);
    RUN_TEST(test_hello_name_of_full_length);
    RUN_TEST(test_time_alarm_bytes);
    RUN_TEST(test_ack_and_heartbeat);
    RUN_TEST(test_ring_messages);
    RUN_TEST(test_game_messages);
    RUN_TEST(test_encode_errors);
    RUN_TEST(test_unknown_flag_bits_are_not_sent_or_kept);
    RUN_TEST(test_decode_rejects_bad_frames);
    RUN_TEST(test_extra_bytes_are_ignored);
    RUN_TEST(test_bool_and_name_from_foreign_bytes);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
