/*
 * Tests for lib/sync_manifest – cmd/music/sync file list and the write /
 * delete rules (docs/PROTOCOL.md, music/sync).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "sync_manifest.h"

#define SHA_A "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF"
#define SHA_B "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"

static sync_manifest_t *m;

void setUp(void)
{
    m = NULL;
}

void tearDown(void)
{
    sync_manifest_destroy(m);
}

static sync_manifest_t *parse(const char *json, sync_mf_err_t *err)
{
    return sync_manifest_parse(json, strlen(json), err);
}

/* ------------------------------------------------------------- paths --- */

static void test_valid_paths(void)
{
    TEST_ASSERT_TRUE(sync_path_valid("track.mp3"));
    TEST_ASSERT_TRUE(sync_path_valid("B/track01.mp3"));
    TEST_ASSERT_TRUE(sync_path_valid("V/go.wav"));
    TEST_ASSERT_TRUE(sync_path_valid("sleep_rain/Rain (Night) 01.MP3"));
    TEST_ASSERT_TRUE(sync_path_valid("a-b_c/x.Wav"));
    TEST_ASSERT_TRUE_MESSAGE(sync_path_valid("abcdefghijklmnopqrstuvw/a.mp3"), "23-character folder");
    char name[SYNC_NAME_MAX + 1];
    memset(name, 'n', sizeof(name));
    memcpy(name + SYNC_NAME_MAX - 4, ".mp3", 5);
    TEST_ASSERT_EQUAL_size_t(SYNC_NAME_MAX, strlen(name));
    TEST_ASSERT_TRUE_MESSAGE(sync_path_valid(name), "95-character name");
}

static void test_invalid_paths(void)
{
    static const char *const bad[] = {
        "",
        "/track.mp3",
        "B/",
        "B//x.mp3",
        "A/B/x.mp3",          /* two levels */
        "../x.mp3",
        "B/../x.mp3",
        "B/.hidden.mp3",
        "B/ lead.mp3",
        "B/trail.mp3 ",
        "B/dot.mp3.",
        "B/.mp3",             /* nothing before the extension */
        "B/x.txt",
        "B/x.mp3.part",
        "B/x.flac",
        "sleep rain/x.mp3",   /* space in the folder */
        "abcdefghijklmnopqrstuvwx/a.mp3", /* 24-character folder */
        "B/a:b.mp3",
        "B/a*b.mp3",
        "B/a?b.mp3",
        "B/a\"b.mp3",
        "B/a<b.mp3",
        "B/a|b.mp3",
        "B/a\\b.mp3",
        "B/tab\t.mp3",
        "B/\xc3\xa4.mp3",     /* UTF-8: outside 0x20 ... 0x7E */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "path %u", (unsigned)i);
        TEST_ASSERT_FALSE_MESSAGE(sync_path_valid(bad[i]), msg);
    }
    char name[SYNC_NAME_MAX + 2];
    memset(name, 'n', sizeof(name));
    memcpy(name + SYNC_NAME_MAX + 1 - 4, ".mp3", 5);
    TEST_ASSERT_EQUAL_size_t(SYNC_NAME_MAX + 1, strlen(name));
    TEST_ASSERT_FALSE_MESSAGE(sync_path_valid(name), "96-character name");
    TEST_ASSERT_FALSE(sync_path_valid(NULL));
}

static void test_managed_names(void)
{
    TEST_ASSERT_TRUE(sync_name_managed("a.mp3"));
    TEST_ASSERT_TRUE(sync_name_managed("A.MP3"));
    TEST_ASSERT_TRUE(sync_name_managed("go.wav"));
    TEST_ASSERT_TRUE(sync_name_managed("x.mp3.part"));
    TEST_ASSERT_TRUE(sync_name_managed("x.PART"));
    TEST_ASSERT_FALSE(sync_name_managed("settings.txt"));
    TEST_ASSERT_FALSE(sync_name_managed("cover.jpg"));
    TEST_ASSERT_FALSE(sync_name_managed(".mp3"));
    TEST_ASSERT_FALSE(sync_name_managed("mp3"));
    TEST_ASSERT_FALSE_MESSAGE(sync_name_managed("B/a.mp3"), "names only");
    TEST_ASSERT_FALSE(sync_name_managed(NULL));
}

static void test_hex(void)
{
    uint8_t out[32];
    memset(out, 0x5A, sizeof(out));
    TEST_ASSERT_TRUE(sync_hex_sha256(SHA_A, out));
    TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[26]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[31]);
    uint8_t keep[32];
    memcpy(keep, out, sizeof(keep));
    TEST_ASSERT_FALSE(sync_hex_sha256("00", out));
    TEST_ASSERT_FALSE(sync_hex_sha256(SHA_A "0", out));
    TEST_ASSERT_FALSE(sync_hex_sha256("g0112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", out));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(keep, out, 32, "out untouched on failure");
    TEST_ASSERT_FALSE(sync_hex_sha256(NULL, out));
}

/* ---------------------------------------------------------- documents --- */

static void test_parse_ok(void)
{
    sync_mf_err_t err = SYNC_MF_ERR_MEMORY;
    m = parse("{\"base\":\"https://h.example/files/\",\"files\":["
              "{\"path\":\"B/track01.mp3\",\"size\":4812345,\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"V/go.wav\",\"size\":0,\"sha256\":\"" SHA_B "\"}]}",
              &err);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(SYNC_MF_OK, err);
    TEST_ASSERT_EQUAL_STRING("https://h.example/files/", sync_manifest_base(m));
    TEST_ASSERT_EQUAL_size_t(2, sync_manifest_count(m));
    TEST_ASSERT_EQUAL_size_t(0, sync_manifest_skipped(m));
    const sync_file_t *f = sync_manifest_file(m, 0);
    TEST_ASSERT_EQUAL_STRING("B/track01.mp3", f->path);
    TEST_ASSERT_EQUAL_UINT32(4812345u, f->size);
    TEST_ASSERT_EQUAL_HEX8(0x00, f->sha256[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, f->sha256[31]);
    TEST_ASSERT_EQUAL_STRING("V/go.wav", sync_manifest_file(m, 1)->path);
    TEST_ASSERT_EQUAL_UINT32(0, sync_manifest_file(m, 1)->size);
    TEST_ASSERT_NULL(sync_manifest_file(m, 2));
    TEST_ASSERT_TRUE(sync_manifest_has(m, "V/go.wav"));
    TEST_ASSERT_FALSE_MESSAGE(sync_manifest_has(m, "v/go.wav"), "case-sensitive");
    TEST_ASSERT_FALSE(sync_manifest_has(m, "go.wav"));
}

static void test_empty_list(void)
{
    sync_mf_err_t err;
    m = parse("{\"base\":\"https://h/f/\",\"files\":[]}", &err);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL(SYNC_MF_OK, err);
    TEST_ASSERT_EQUAL_size_t(0, sync_manifest_count(m));
}

static void test_format_errors(void)
{
    static const char *const bad[] = {
        "",
        "not json",
        "[]",
        "{\"files\":[]}",
        "{\"base\":\"https://h/f/\"}",
        "{\"base\":\"https://h/f/\",\"files\":{}}",
        "{\"base\":\"http://h/f/\",\"files\":[]}",  /* not https */
        "{\"base\":\"https://h/f\",\"files\":[]}",  /* no trailing slash */
        "{\"base\":\"https://\",\"files\":[]}",
        "{\"base\":7,\"files\":[]}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        sync_mf_err_t err = SYNC_MF_OK;
        sync_manifest_t *x = parse(bad[i], &err);
        char msg[48];
        snprintf(msg, sizeof(msg), "document %u", (unsigned)i);
        TEST_ASSERT_NULL_MESSAGE(x, msg);
        TEST_ASSERT_EQUAL_MESSAGE(SYNC_MF_ERR_FORMAT, err, msg);
    }
    TEST_ASSERT_NULL(sync_manifest_parse(NULL, 0, NULL));
}

static void test_base_too_long(void)
{
    char json[400];
    char base[SYNC_BASE_MAX + 1];
    memset(base, 'a', sizeof(base));
    memcpy(base, "https://", 8);
    base[SYNC_BASE_MAX - 2] = '/';
    base[SYNC_BASE_MAX - 1] = '\0'; /* 159 characters: fits */
    snprintf(json, sizeof(json), "{\"base\":\"%s\",\"files\":[]}", base);
    m = parse(json, NULL);
    TEST_ASSERT_NOT_NULL(m);
    base[SYNC_BASE_MAX - 1] = '/';
    base[SYNC_BASE_MAX] = '\0'; /* 160 characters: too long */
    snprintf(json, sizeof(json), "{\"base\":\"%s\",\"files\":[]}", base);
    TEST_ASSERT_NULL(parse(json, NULL));
}

static void test_bad_entries_skipped(void)
{
    m = parse("{\"base\":\"https://h/f/\",\"files\":["
              "7,"
              "{\"path\":\"../x.mp3\",\"size\":1,\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"A/a.mp3\",\"size\":-1,\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"A/b.mp3\",\"size\":1.5,\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"A/c.mp3\",\"size\":4294967296,\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"A/d.mp3\",\"size\":\"1\",\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"A/e.mp3\",\"size\":1,\"sha256\":\"abc\"},"
              "{\"path\":\"A/f.mp3\",\"size\":1},"
              "{\"size\":1,\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"A/ok.mp3\",\"size\":4294967295,\"sha256\":\"" SHA_A "\"},"
              "{\"path\":\"A/ok.mp3\",\"size\":2,\"sha256\":\"" SHA_B "\"}"
              "]}",
              NULL);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_size_t(1, sync_manifest_count(m));
    TEST_ASSERT_EQUAL_size_t(10, sync_manifest_skipped(m));
    TEST_ASSERT_EQUAL_STRING("A/ok.mp3", sync_manifest_file(m, 0)->path);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(4294967295u, sync_manifest_file(m, 0)->size, "the first of a duplicate wins");
}

static void test_more_than_max_files(void)
{
    const size_t n = SYNC_MAX_FILES + 3;
    char *json = malloc(n * 120 + 100);
    TEST_ASSERT_NOT_NULL(json);
    size_t len = (size_t)sprintf(json, "{\"base\":\"https://h/f/\",\"files\":[");
    for (size_t i = 0; i < n; i++) {
        len += (size_t)sprintf(json + len, "%s{\"path\":\"A/t%03u.mp3\",\"size\":1,\"sha256\":\"%s\"}",
                               i ? "," : "", (unsigned)i, SHA_A);
    }
    strcpy(json + len, "]}");
    m = parse(json, NULL);
    free(json);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_size_t(SYNC_MAX_FILES, sync_manifest_count(m));
    TEST_ASSERT_EQUAL_size_t(3, sync_manifest_skipped(m));
    TEST_ASSERT_EQUAL_STRING("A/t255.mp3", sync_manifest_file(m, SYNC_MAX_FILES - 1)->path);
}

static void test_not_nul_terminated(void)
{
    const char text[] = "{\"base\":\"https://h/f/\",\"files\":[]}XXXX";
    m = sync_manifest_parse(text, strlen(text) - 4, NULL);
    TEST_ASSERT_NOT_NULL(m);
}

/* --------------------------------------------------------------- URLs --- */

static void test_url(void)
{
    m = parse("{\"base\":\"https://h.example/files/\",\"files\":[]}", NULL);
    char url[128];
    TEST_ASSERT_TRUE(sync_manifest_url(m, "B/track01.mp3", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://h.example/files/B/track01.mp3", url);
    TEST_ASSERT_TRUE(sync_manifest_url(m, "sleep_rain/Rain (Night) #1~.mp3", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://h.example/files/sleep_rain/Rain%20%28Night%29%20%231~.mp3", url);
    TEST_ASSERT_TRUE(sync_manifest_url(m, "a&b+c%.mp3", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://h.example/files/a%26b%2Bc%25.mp3", url);
}

static void test_url_too_small(void)
{
    m = parse("{\"base\":\"https://h/f/\",\"files\":[]}", NULL);
    char url[20];
    /* "https://h/f/" is 12 characters: 7 more fit with the NUL */
    TEST_ASSERT_TRUE(sync_manifest_url(m, "a.mp3", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("https://h/f/a.mp3", url);
    TEST_ASSERT_FALSE(sync_manifest_url(m, "abcdefgh.mp3", url, sizeof(url)));
    TEST_ASSERT_EQUAL_STRING("", url);
    TEST_ASSERT_FALSE_MESSAGE(sync_manifest_url(m, "a b.mp3", url, 19), "%20 does not fit");
    TEST_ASSERT_FALSE(sync_manifest_url(NULL, "a.mp3", url, sizeof(url)));
    TEST_ASSERT_FALSE(sync_manifest_url(m, NULL, url, sizeof(url)));
}

static void test_null_reads(void)
{
    TEST_ASSERT_NULL(sync_manifest_base(NULL));
    TEST_ASSERT_EQUAL_size_t(0, sync_manifest_count(NULL));
    TEST_ASSERT_EQUAL_size_t(0, sync_manifest_skipped(NULL));
    TEST_ASSERT_NULL(sync_manifest_file(NULL, 0));
    TEST_ASSERT_FALSE(sync_manifest_has(NULL, "a.mp3"));
    sync_manifest_destroy(NULL);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_valid_paths);
    RUN_TEST(test_invalid_paths);
    RUN_TEST(test_managed_names);
    RUN_TEST(test_hex);
    RUN_TEST(test_parse_ok);
    RUN_TEST(test_empty_list);
    RUN_TEST(test_format_errors);
    RUN_TEST(test_base_too_long);
    RUN_TEST(test_bad_entries_skipped);
    RUN_TEST(test_more_than_max_files);
    RUN_TEST(test_not_nul_terminated);
    RUN_TEST(test_url);
    RUN_TEST(test_url_too_small);
    RUN_TEST(test_null_reads);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
