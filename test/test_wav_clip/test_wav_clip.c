/*
 * Tests for lib/wav_clip – requirements COLD-09, COLD-12.
 * The helper builds WAV headers byte by byte.
 */
#include <stdint.h>
#include <string.h>
#include <unity.h>

#include "wav_clip.h"

static uint8_t buf[600];
static size_t len;
static wav_clip_info_t info;

void setUp(void)
{
    memset(buf, 0, sizeof(buf));
    len = 0;
    memset(&info, 0xAA, sizeof(info));
}

void tearDown(void) {}

static void put(const void *p, size_t n)
{
    memcpy(buf + len, p, n);
    len += n;
}

static void put32(uint32_t v)
{
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, 4);
}

static void put16(uint16_t v)
{
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(b, 2);
}

static void riff(void)
{
    put("RIFF", 4);
    put32(0x12345678u); /* ignored */
    put("WAVE", 4);
}

static void fmt(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits, uint32_t chunk_size)
{
    put("fmt ", 4);
    put32(chunk_size);
    put16(tag);
    put16(channels);
    put32(rate);
    put32(rate * channels * bits / 8u);
    put16((uint16_t)(channels * bits / 8u));
    put16(bits);
    for (uint32_t i = 16; i < chunk_size; i++) {
        put("\0", 1);
    }
}

static void data(uint32_t size)
{
    put("data", 4);
    put32(size);
}

static wav_clip_result_t parse(uint32_t file_size)
{
    return wav_clip_parse(buf, len, file_size, &info);
}

static void test_plain_48k_clip(void)
{
    riff();
    fmt(1, 1, 48000, 16, 16);
    data(96000u * 2u); /* 2.0 s */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(44u + 192000u));
    TEST_ASSERT_EQUAL_UINT32(48000, info.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT32(44, info.data_offset);
    TEST_ASSERT_EQUAL_UINT32(96000, info.frames);
    TEST_ASSERT_EQUAL_UINT32(2000, info.duration_ms);
}

static void test_duration_rounds_up(void)
{
    riff();
    fmt(1, 1, 22050, 16, 16);
    data(2u * 22051u); /* one sample more than a second */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(44u + 2u * 22051u));
    TEST_ASSERT_EQUAL_UINT32(22051, info.frames);
    TEST_ASSERT_EQUAL_UINT32(1001, info.duration_ms);
}

static void test_other_chunks_are_skipped_with_padding(void)
{
    riff();
    put("LIST", 4);
    put32(5); /* odd: one pad byte follows */
    put("abcde\0", 6);
    fmt(1, 1, 16000, 16, 18); /* fmt with extension bytes */
    put("fact", 4);
    put32(4);
    put32(0);
    data(32000);
    const uint32_t offset = (uint32_t)len;
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(offset + 32000u));
    TEST_ASSERT_EQUAL_UINT32(16000, info.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT32(offset, info.data_offset);
    TEST_ASSERT_EQUAL_UINT32(16000, info.frames);
    TEST_ASSERT_EQUAL_UINT32(1000, info.duration_ms);
}

static void test_data_size_is_clamped_to_the_file(void)
{
    riff();
    fmt(1, 1, 48000, 16, 16);
    data(0xFFFFFFFFu); /* streamed output */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(44u + 9601u)); /* odd byte at the end is dropped */
    TEST_ASSERT_EQUAL_UINT32(4800, info.frames);
    TEST_ASSERT_EQUAL_UINT32(100, info.duration_ms);

    setUp();
    riff();
    fmt(1, 1, 48000, 16, 16);
    data(0); /* 0 = to the end of the file */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(44u + 9600u));
    TEST_ASSERT_EQUAL_UINT32(4800, info.frames);

    setUp();
    riff();
    fmt(1, 1, 48000, 16, 16);
    data(4800); /* smaller than the file: trailing chunks are not samples */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(44u + 9600u));
    TEST_ASSERT_EQUAL_UINT32(2400, info.frames);
}

static void test_truncated_header_asks_for_more(void)
{
    riff();
    fmt(1, 1, 48000, 16, 16);
    data(1000);
    const size_t full = len;
    const wav_clip_info_t untouched = info;
    for (len = 0; len < full; len++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(WAV_CLIP_ERR_TRUNCATED, parse(2000), "every shorter prefix is TRUNCATED");
        TEST_ASSERT_EQUAL_MEMORY(&untouched, &info, sizeof(info));
    }
    len = full;
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(2000));
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_TRUNCATED, wav_clip_parse(NULL, 44, 2000, &info));
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_TRUNCATED, wav_clip_parse(buf, len, 2000, NULL));
}

static void test_not_a_wav_file(void)
{
    put("ID3\x04\0\0\0\0\0\0\0\0\0\0\0\0", 16); /* an MP3 */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_NOT_WAV, parse(5000));
    setUp();
    put("RIFF", 4);
    put32(100);
    put("AVI ", 4);
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_NOT_WAV, parse(5000));
    /* and a real one is accepted, so the stub cannot pass by always saying NOT_WAV */
    setUp();
    riff();
    fmt(1, 1, 48000, 16, 16);
    data(960);
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(44u + 960u));
}

static void expect_format_error(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits, const char *msg)
{
    setUp();
    riff();
    fmt(tag, channels, rate, bits, 16);
    data(1000);
    const wav_clip_info_t untouched = info;
    TEST_ASSERT_EQUAL_INT_MESSAGE(WAV_CLIP_ERR_FORMAT, parse(44u + 1000u), msg);
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &info, sizeof(info));
}

static void test_only_16_bit_mono_pcm(void)
{
    expect_format_error(3, 1, 48000, 16, "float format tag");
    expect_format_error(1, 2, 48000, 16, "stereo");
    expect_format_error(1, 1, 48000, 24, "24 bit");
    expect_format_error(1, 1, 48000, 8, "8 bit");
    expect_format_error(1, 1, 7999, 16, "rate too low");
    expect_format_error(1, 1, 96000, 16, "rate too high");

    setUp();
    riff();
    data(1000); /* data before fmt */
    fmt(1, 1, 48000, 16, 16);
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_FORMAT, parse(3000));

    setUp();
    riff();
    put("fmt ", 4);
    put32(14); /* too short */
    put("\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 14);
    data(1000);
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_FORMAT, parse(3000));

    setUp();
    riff();
    fmt(1, 1, 8000, 16, 16); /* the limits themselves are fine */
    data(1600);
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(44u + 1600u));
    TEST_ASSERT_EQUAL_UINT32(100, info.duration_ms);
}

static void test_empty_clip(void)
{
    riff();
    fmt(1, 1, 48000, 16, 16);
    data(0);
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_EMPTY, parse(44)); /* nothing after the header */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_EMPTY, parse(45)); /* a single byte is no sample */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_ERR_EMPTY, parse(40)); /* file shorter than its header */
    TEST_ASSERT_EQUAL_INT(WAV_CLIP_OK, parse(46));
    TEST_ASSERT_EQUAL_UINT32(1, info.frames);
    TEST_ASSERT_EQUAL_UINT32(1, info.duration_ms);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_48k_clip);
    RUN_TEST(test_duration_rounds_up);
    RUN_TEST(test_other_chunks_are_skipped_with_padding);
    RUN_TEST(test_data_size_is_clamped_to_the_file);
    RUN_TEST(test_truncated_header_asks_for_more);
    RUN_TEST(test_not_a_wav_file);
    RUN_TEST(test_only_16_bit_mono_pcm);
    RUN_TEST(test_empty_clip);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
