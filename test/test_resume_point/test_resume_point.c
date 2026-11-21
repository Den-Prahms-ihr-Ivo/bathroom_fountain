/*
 * Tests for lib/resume_point – requirement BED-46.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "resume_point.h"

static const uint8_t pinned[] = {0x52, 0x50, 0x01, 0x05, 0x73, 0x6C, 0x65, 0x65, 0x70, 0x11, 0x65,
                                 0x70, 0x30, 0x37, 0x20, 0x74, 0x68, 0x65, 0x20, 0x63, 0x61, 0x73,
                                 0x65, 0x2E, 0x6D, 0x70, 0x33, 0xEF, 0xBE, 0xAD, 0xDE, 0x03, 0x00,
                                 0x00, 0x00, 0x40, 0xE2, 0x01, 0x00, 0xC5, 0xC1, 0xA2, 0x81};

static resume_point_t rp;
static uint8_t buf[RESUME_BLOB_MAX + 16];

void setUp(void)
{
    memset(&rp, 0, sizeof(rp));
    strcpy(rp.folder, "sleep");
    strcpy(rp.track, "ep07 the case.mp3");
    rp.seed = 0xDEADBEEFu;
    rp.index = 3;
    rp.offset = 123456;
    memset(buf, 0xCC, sizeof(buf));
}

void tearDown(void) {}

static void test_blob_bytes(void)
{
    TEST_ASSERT_EQUAL_size_t(sizeof(pinned), resume_encode(&rp, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pinned, buf, sizeof(pinned));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xCC, buf[sizeof(pinned)], "nothing written past the blob");
}

static void test_decode_pinned(void)
{
    resume_point_t out;
    memset(&out, 0xEE, sizeof(out));
    TEST_ASSERT_TRUE(resume_decode(pinned, sizeof(pinned), &out));
    TEST_ASSERT_EQUAL_STRING("sleep", out.folder);
    TEST_ASSERT_EQUAL_STRING("ep07 the case.mp3", out.track);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFu, out.seed);
    TEST_ASSERT_EQUAL_UINT32(3, out.index);
    TEST_ASSERT_EQUAL_UINT32(123456, out.offset);
}

static void test_round_trip_at_maximum_lengths(void)
{
    memset(rp.folder, 'f', RESUME_FOLDER_MAX);
    rp.folder[RESUME_FOLDER_MAX] = '\0';
    memset(rp.track, 't', RESUME_TRACK_MAX);
    rp.track[RESUME_TRACK_MAX] = '\0';
    rp.seed = 0;
    rp.index = 0xFFFFFFFFu;
    rp.offset = 0xFFFFFFFFu;
    const size_t n = resume_encode(&rp, buf, RESUME_BLOB_MAX);
    TEST_ASSERT_EQUAL_size_t(RESUME_BLOB_MAX, n);
    resume_point_t out;
    TEST_ASSERT_TRUE(resume_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_MEMORY(&rp, &out, sizeof(rp));
}

static void test_encode_rejects_invalid(void)
{
    TEST_ASSERT_EQUAL_size_t(0, resume_encode(NULL, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, resume_encode(&rp, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&rp, buf, sizeof(pinned) - 1), "buffer one byte short");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xCC, buf[0], "out untouched on failure");
    TEST_ASSERT_EQUAL_size_t(sizeof(pinned), resume_encode(&rp, buf, sizeof(pinned)));

    resume_point_t bad;
    bad = rp; bad.folder[0] = '\0';
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "empty folder");
    bad = rp; bad.track[0] = '\0';
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "empty track");
    bad = rp; bad.index = 0;
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "index 0");
    bad = rp; strcpy(bad.folder, "a/b");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "slash in folder");
    bad = rp; strcpy(bad.track, "x/y.mp3");
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "slash in track");
    bad = rp; bad.track[2] = '\n';
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "control character");
    bad = rp; bad.folder[1] = (char)0xC3;
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "non-ASCII byte");
    bad = rp; memset(bad.folder, 'f', sizeof(bad.folder)); /* no NUL inside the field */
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, resume_encode(&bad, buf, sizeof(buf)), "folder without NUL");
}

static void test_decode_rejects_damage(void)
{
    resume_point_t out;
    memset(&out, 0x5A, sizeof(out));
    const resume_point_t untouched = out;
    uint8_t b[sizeof(pinned) + 1];

    TEST_ASSERT_FALSE(resume_decode(NULL, sizeof(pinned), &out));
    TEST_ASSERT_FALSE(resume_decode(pinned, sizeof(pinned), NULL));
    for (size_t len = 0; len < sizeof(pinned); len++) {
        TEST_ASSERT_FALSE_MESSAGE(resume_decode(pinned, len, &out), "truncated blob");
    }
    memcpy(b, pinned, sizeof(pinned));
    b[sizeof(pinned)] = 0;
    TEST_ASSERT_FALSE_MESSAGE(resume_decode(b, sizeof(pinned) + 1, &out), "extra byte");

    /* every single-bit flip is caught (by the CRC or by a check before it) */
    for (size_t i = 0; i < sizeof(pinned); i++) {
        for (int bit = 0; bit < 8; bit++) {
            memcpy(b, pinned, sizeof(pinned));
            b[i] ^= (uint8_t)(1u << bit);
            TEST_ASSERT_FALSE_MESSAGE(resume_decode(b, sizeof(pinned), &out), "bit flip accepted");
        }
    }
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &out, sizeof(out));
}

static void test_decode_checks_content_behind_good_crc(void)
{
    /* blobs with a correct CRC but invalid content (written by a buggy older
     * firmware) are refused too */
    resume_point_t out;
    static const uint8_t index_zero[] = {0x52, 0x50, 0x01, 0x01, 0x61, 0x01, 0x62, 0x01, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x23, 0xC2, 0x08, 0xE9};
    TEST_ASSERT_FALSE_MESSAGE(resume_decode(index_zero, sizeof(index_zero), &out), "index 0");
    static const uint8_t empty_folder[] = {0x52, 0x50, 0x01, 0x00, 0x01, 0x62, 0x01, 0x00, 0x00, 0x00, 0x01,
                                           0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x0E, 0xD7, 0xEF, 0x81};
    TEST_ASSERT_FALSE_MESSAGE(resume_decode(empty_folder, sizeof(empty_folder), &out), "empty folder");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blob_bytes);
    RUN_TEST(test_decode_pinned);
    RUN_TEST(test_round_trip_at_maximum_lengths);
    RUN_TEST(test_encode_rejects_invalid);
    RUN_TEST(test_decode_rejects_damage);
    RUN_TEST(test_decode_checks_content_behind_good_crc);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
