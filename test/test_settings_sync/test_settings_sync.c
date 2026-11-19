/*
 * Tests for lib/settings_sync – requirements SRV-05 ... SRV-09.
 * The store is a fake with five keys; the reported document is checked by
 * parsing it with cJSON.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "cJSON.h"
#include "settings_sync.h"

/* ---------------------------------------------------------------- fake store */

#define N_KEYS 5
static const char *const KEYS[N_KEYS] = {"name", "level", "pass", "low", "high"};
static char values[N_KEYS][32];
static int set_calls;
static char set_log[256];

static int key_index(const char *key)
{
    for (int i = 0; i < N_KEYS; i++) {
        if (strcmp(KEYS[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

static size_t store_count(void *ctx)
{
    (void)ctx;
    return N_KEYS;
}

static const char *store_name(void *ctx, size_t i)
{
    (void)ctx;
    return i < N_KEYS ? KEYS[i] : NULL;
}

static bool store_is_secret(void *ctx, const char *key)
{
    (void)ctx;
    return strcmp(key, "pass") == 0;
}

/* level: 1-32; low < high (conflict); name: no '!' (invalid), at most 10 chars (range) */
static sync_set_t store_set(void *ctx, const char *key, const char *value)
{
    (void)ctx;
    set_calls++;
    strncat(set_log, key, sizeof(set_log) - strlen(set_log) - 2);
    strcat(set_log, " ");
    const int i = key_index(key);
    if (i < 0) {
        return SYNC_SET_UNKNOWN;
    }
    if (i == 0) {
        if (strchr(value, '!') != NULL) {
            return SYNC_SET_INVALID;
        }
        if (strlen(value) > 10) {
            return SYNC_SET_RANGE;
        }
    } else if (i != 2) {
        char *end;
        const long v = strtol(value, &end, 10);
        if (*value == '\0' || *end != '\0') {
            return SYNC_SET_INVALID;
        }
        if (i == 1 && (v < 1 || v > 32)) {
            return SYNC_SET_RANGE;
        }
        if (i == 3 && v >= atol(values[4])) {
            return SYNC_SET_CONFLICT;
        }
        if (i == 4 && v <= atol(values[3])) {
            return SYNC_SET_CONFLICT;
        }
    }
    snprintf(values[i], sizeof(values[i]), "%s", value);
    return SYNC_SET_OK;
}

static bool store_format(void *ctx, const char *key, bool reveal, char *out, size_t n)
{
    (void)ctx;
    const int i = key_index(key);
    if (i < 0 || n == 0) {
        return false;
    }
    const char *v = values[i];
    if (i == 2 && !reveal && v[0] != '\0') {
        v = "********";
    }
    if (strlen(v) + 1 > n) {
        out[0] = '\0';
        return false;
    }
    strcpy(out, v);
    return true;
}

static const sync_store_t store = {NULL, store_count, store_name, store_is_secret, store_set, store_format};

/* ---------------------------------------------------------------- fixture */

static settings_sync_t *sync;
static char report[1024];

void setUp(void)
{
    strcpy(values[0], "bath");
    strcpy(values[1], "16");
    strcpy(values[2], "hunter22");
    strcpy(values[3], "150");
    strcpy(values[4], "600");
    set_calls = 0;
    set_log[0] = '\0';
    sync = NULL;
    memset(report, 'Z', sizeof(report));
}

void tearDown(void)
{
    settings_sync_destroy(sync);
    sync = NULL;
}

static void create(void)
{
    sync = settings_sync_create(&store);
    TEST_ASSERT_NOT_NULL_MESSAGE(sync, "settings_sync_create returned NULL for a complete store");
}

static sync_result_t apply(const char *json)
{
    return settings_sync_apply(sync, json, strlen(json));
}

static void expect(sync_result_t r, sync_status_t status, uint32_t version, unsigned changed, unsigned rejected)
{
    TEST_ASSERT_EQUAL_INT(status, r.status);
    TEST_ASSERT_EQUAL_UINT32(version, r.version);
    TEST_ASSERT_EQUAL_UINT16(changed, r.changed);
    TEST_ASSERT_EQUAL_UINT16(rejected, r.rejected);
}

static cJSON *parsed_report(void)
{
    memset(report, 'Z', sizeof(report));
    const int len = settings_sync_report(sync, report, sizeof(report));
    TEST_ASSERT_TRUE_MESSAGE(len > 0, "report failed");
    TEST_ASSERT_EQUAL_size_t((size_t)len, strlen(report));
    cJSON *doc = cJSON_Parse(report);
    TEST_ASSERT_NOT_NULL_MESSAGE(doc, "the report is not valid JSON");
    return doc;
}

static void expect_member(const cJSON *obj, const char *key, const char *value)
{
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(obj, key);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(m), key);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(value, m->valuestring, key);
}

/* ---------------------------------------------------------------- tests */

static void test_create_needs_a_complete_store(void)
{
    create();
    TEST_ASSERT_EQUAL_UINT32(0, settings_sync_version(sync));
    TEST_ASSERT_NULL(settings_sync_create(NULL));
    sync_store_t broken = store;
    broken.set = NULL;
    TEST_ASSERT_NULL(settings_sync_create(&broken));
    broken = store;
    broken.format = NULL;
    TEST_ASSERT_NULL(settings_sync_create(&broken));
    broken = store;
    broken.is_secret = NULL;
    TEST_ASSERT_NULL(settings_sync_create(&broken));
    broken = store;
    broken.count = NULL;
    TEST_ASSERT_NULL(settings_sync_create(&broken));
    broken = store;
    broken.name = NULL;
    TEST_ASSERT_NULL(settings_sync_create(&broken));
    settings_sync_destroy(NULL);
}

static void test_apply_sets_changed_values_only(void)
{
    create();
    expect(apply("{\"version\":3,\"values\":{\"name\":\"bath\",\"level\":\"20\",\"high\":\"700\"}}"), SYNC_APPLIED, 3, 2, 0);
    TEST_ASSERT_EQUAL_STRING("20", values[1]);
    TEST_ASSERT_EQUAL_STRING("700", values[4]);
    TEST_ASSERT_EQUAL_STRING("bath", values[0]);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("level high ", set_log, "unchanged values are not written again");
    TEST_ASSERT_EQUAL_UINT32(3, settings_sync_version(sync));
    TEST_ASSERT_EQUAL_STRING("150", values[3]); /* keys not in the document keep their value */
}

static void test_json_does_not_need_a_terminator(void)
{
    create();
    const char *raw = "{\"version\":1,\"values\":{\"level\":\"9\"}}trailing bytes of the MQTT buffer";
    expect(settings_sync_apply(sync, raw, 37), SYNC_APPLIED, 1, 1, 0);
    TEST_ASSERT_EQUAL_STRING("9", values[1]);
}

static void test_old_or_repeated_versions_are_ignored(void)
{
    create();
    expect(apply("{\"version\":5,\"values\":{\"level\":\"20\"}}"), SYNC_APPLIED, 5, 1, 0);
    expect(apply("{\"version\":5,\"values\":{\"level\":\"21\"}}"), SYNC_STALE, 5, 0, 0);
    expect(apply("{\"version\":4,\"values\":{\"level\":\"22\"}}"), SYNC_STALE, 4, 0, 0);
    TEST_ASSERT_EQUAL_STRING("20", values[1]);
    TEST_ASSERT_EQUAL_INT(1, set_calls);
    TEST_ASSERT_EQUAL_UINT32(5, settings_sync_version(sync));
    expect(apply("{\"version\":6,\"values\":{\"level\":\"23\"}}"), SYNC_APPLIED, 6, 1, 0);
    TEST_ASSERT_EQUAL_STRING("23", values[1]);
}

static void test_version_survives_a_restart(void)
{
    create();
    settings_sync_set_version(sync, 40); /* from NVS */
    TEST_ASSERT_EQUAL_UINT32(40, settings_sync_version(sync));
    expect(apply("{\"version\":40,\"values\":{\"level\":\"2\"}}"), SYNC_STALE, 40, 0, 0);
    expect(apply("{\"version\":41,\"values\":{\"level\":\"2\"}}"), SYNC_APPLIED, 41, 1, 0);
}

static void test_malformed_documents_change_nothing(void)
{
    const char *bad[] = {
        "",
        "not json",
        "[1,2]",
        "\"text\"",
        "{\"values\":{\"level\":\"20\"}}",
        "{\"version\":\"7\",\"values\":{\"level\":\"20\"}}",
        "{\"version\":0,\"values\":{\"level\":\"20\"}}",
        "{\"version\":-3,\"values\":{\"level\":\"20\"}}",
        "{\"version\":7.5,\"values\":{\"level\":\"20\"}}",
        "{\"version\":2147483648,\"values\":{\"level\":\"20\"}}",
        "{\"version\":7}",
        "{\"version\":7,\"values\":[\"level\",\"20\"]}",
        "{\"version\":7,\"values\":\"level\"}",
        "{\"version\":7,\"values\":{\"level\":\"20\"}",
    };
    create();
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const sync_result_t r = apply(bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(SYNC_ERR_FORMAT, r.status, bad[i]);
        TEST_ASSERT_EQUAL_UINT32(0, r.version);
        TEST_ASSERT_EQUAL_UINT16(0, r.changed);
    }
    TEST_ASSERT_EQUAL_INT(0, set_calls);
    TEST_ASSERT_EQUAL_UINT32(0, settings_sync_version(sync));
    TEST_ASSERT_EQUAL_INT(SYNC_ERR_FORMAT, settings_sync_apply(sync, NULL, 5).status);
    expect(apply("{\"version\":2147483647,\"values\":{}}"), SYNC_APPLIED, 2147483647u, 0, 0);
}

static void test_rejected_values_do_not_stop_the_rest(void)
{
    create();
    const sync_result_t r = apply("{\"version\":2,\"values\":{"
                                  "\"level\":\"99\",\"nokey\":\"1\",\"name\":\"a!\",\"low\":\"800\","
                                  "\"high\":\"900\",\"level2\":7,\"name2\":null}}");
    expect(r, SYNC_APPLIED, 2, 1, 6);
    TEST_ASSERT_EQUAL_STRING("16", values[1]);
    TEST_ASSERT_EQUAL_STRING("bath", values[0]);
    TEST_ASSERT_EQUAL_STRING("150", values[3]);
    TEST_ASSERT_EQUAL_STRING("900", values[4]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, settings_sync_version(sync), "the version counts as applied");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("level nokey name low high ", set_log, "document order; non-strings never reach the store");

    cJSON *doc = parsed_report();
    const cJSON *rej = cJSON_GetObjectItemCaseSensitive(doc, "rejected");
    TEST_ASSERT_TRUE(cJSON_IsObject(rej));
    TEST_ASSERT_EQUAL_INT(6, cJSON_GetArraySize(rej));
    expect_member(rej, "level", "range");
    expect_member(rej, "nokey", "unknown");
    expect_member(rej, "name", "invalid");
    expect_member(rej, "low", "conflict");
    expect_member(rej, "level2", "invalid");
    expect_member(rej, "name2", "invalid");
    cJSON_Delete(doc);

    /* the next apply replaces the list */
    expect(apply("{\"version\":3,\"values\":{\"low\":\"800\"}}"), SYNC_APPLIED, 3, 1, 0);
    doc = parsed_report();
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(doc, "rejected")));
    cJSON_Delete(doc);
}

static void test_order_in_the_document_matters_for_dependent_keys(void)
{
    create();
    /* low 700 is only legal once high is 900 */
    expect(apply("{\"version\":1,\"values\":{\"high\":\"900\",\"low\":\"700\"}}"), SYNC_APPLIED, 1, 2, 0);
    TEST_ASSERT_EQUAL_STRING("700", values[3]);
    expect(apply("{\"version\":2,\"values\":{\"low\":\"950\",\"high\":\"990\"}}"), SYNC_APPLIED, 2, 1, 1);
    TEST_ASSERT_EQUAL_STRING("700", values[3]);
    TEST_ASSERT_EQUAL_STRING("990", values[4]);
}

static void test_secrets(void)
{
    create();
    /* the server echoes the mask: not a new password */
    expect(apply("{\"version\":1,\"values\":{\"pass\":\"********\"}}"), SYNC_APPLIED, 1, 0, 0);
    TEST_ASSERT_EQUAL_STRING("hunter22", values[2]);
    TEST_ASSERT_EQUAL_INT(0, set_calls);
    /* the same secret again: unchanged */
    expect(apply("{\"version\":2,\"values\":{\"pass\":\"hunter22\"}}"), SYNC_APPLIED, 2, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, set_calls);
    /* a new one is stored */
    expect(apply("{\"version\":3,\"values\":{\"pass\":\"correct horse\"}}"), SYNC_APPLIED, 3, 1, 0);
    TEST_ASSERT_EQUAL_STRING("correct horse", values[2]);
    /* for a normal key the mask text is just a value */
    expect(apply("{\"version\":4,\"values\":{\"name\":\"********\"}}"), SYNC_APPLIED, 4, 1, 0);
    TEST_ASSERT_EQUAL_STRING("********", values[0]);

    cJSON *doc = parsed_report();
    expect_member(cJSON_GetObjectItemCaseSensitive(doc, "values"), "pass", "********");
    TEST_ASSERT_NULL_MESSAGE(strstr(report, "correct horse"), "a secret must never appear in the report");
    cJSON_Delete(doc);

    /* an empty secret is reported as empty */
    expect(apply("{\"version\":5,\"values\":{\"pass\":\"\"}}"), SYNC_APPLIED, 5, 1, 0);
    doc = parsed_report();
    expect_member(cJSON_GetObjectItemCaseSensitive(doc, "values"), "pass", "");
    cJSON_Delete(doc);
}

static void test_report_lists_every_key_in_store_order(void)
{
    create();
    cJSON *doc = parsed_report();
    const cJSON *ver = cJSON_GetObjectItemCaseSensitive(doc, "version");
    TEST_ASSERT_TRUE(cJSON_IsNumber(ver));
    TEST_ASSERT_EQUAL_INT(0, ver->valueint);
    const cJSON *vals = cJSON_GetObjectItemCaseSensitive(doc, "values");
    TEST_ASSERT_TRUE(cJSON_IsObject(vals));
    TEST_ASSERT_EQUAL_INT(N_KEYS, cJSON_GetArraySize(vals));
    int i = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, vals)
    {
        TEST_ASSERT_EQUAL_STRING(KEYS[i], m->string);
        i++;
    }
    expect_member(vals, "name", "bath");
    expect_member(vals, "level", "16");
    expect_member(vals, "low", "150");
    expect_member(vals, "high", "600");
    TEST_ASSERT_TRUE(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(doc, "rejected")));
    TEST_ASSERT_NULL_MESSAGE(strchr(report, '\n'), "compact JSON, one line");
    cJSON_Delete(doc);

    apply("{\"version\":8,\"values\":{\"name\":\"a \\\"q\\\" b\"}}");
    strcpy(values[1], "30"); /* a local change (console) shows up as well */
    doc = parsed_report();
    TEST_ASSERT_EQUAL_INT(8, cJSON_GetObjectItemCaseSensitive(doc, "version")->valueint);
    expect_member(cJSON_GetObjectItemCaseSensitive(doc, "values"), "name", "a \"q\" b");
    expect_member(cJSON_GetObjectItemCaseSensitive(doc, "values"), "level", "30");
    cJSON_Delete(doc);
}

static void test_report_into_a_small_buffer(void)
{
    create();
    const int len = settings_sync_report(sync, report, sizeof(report));
    TEST_ASSERT_TRUE(len > 0);
    char small[40];
    memset(small, 'Z', sizeof(small));
    TEST_ASSERT_EQUAL_INT(-1, settings_sync_report(sync, small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);
    TEST_ASSERT_EQUAL_INT(-1, settings_sync_report(sync, NULL, 100));
    TEST_ASSERT_EQUAL_INT(-1, settings_sync_report(sync, small, 0));
    TEST_ASSERT_EQUAL_INT(-1, settings_sync_report(NULL, report, sizeof(report)));
}

static void test_more_rejects_than_the_list_holds(void)
{
    create();
    char json[1024] = "{\"version\":1,\"values\":{";
    for (int i = 0; i < 20; i++) {
        char member[32];
        snprintf(member, sizeof(member), "%s\"bad%02d\":\"x\"", i ? "," : "", i);
        strcat(json, member);
    }
    strcat(json, "}}");
    expect(apply(json), SYNC_APPLIED, 1, 0, 20);
    cJSON *doc = parsed_report();
    const cJSON *rej = cJSON_GetObjectItemCaseSensitive(doc, "rejected");
    TEST_ASSERT_EQUAL_INT(SYNC_MAX_REJECTED, cJSON_GetArraySize(rej));
    expect_member(rej, "bad00", "unknown");
    expect_member(rej, "bad15", "unknown");
    cJSON_Delete(doc);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_needs_a_complete_store);
    RUN_TEST(test_apply_sets_changed_values_only);
    RUN_TEST(test_json_does_not_need_a_terminator);
    RUN_TEST(test_old_or_repeated_versions_are_ignored);
    RUN_TEST(test_version_survives_a_restart);
    RUN_TEST(test_malformed_documents_change_nothing);
    RUN_TEST(test_rejected_values_do_not_stop_the_rest);
    RUN_TEST(test_order_in_the_document_matters_for_dependent_keys);
    RUN_TEST(test_secrets);
    RUN_TEST(test_report_lists_every_key_in_store_order);
    RUN_TEST(test_report_into_a_small_buffer);
    RUN_TEST(test_more_rejects_than_the_list_holds);
    return UNITY_END();
}

#ifdef ESP_PLATFORM
void app_main(void)
{
    main();
}
#endif
