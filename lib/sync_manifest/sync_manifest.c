/*
 * sync_manifest – see sync_manifest.h.
 */
#include "sync_manifest.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

struct sync_manifest {
    char base[SYNC_BASE_MAX];
    size_t count;
    size_t skipped;
    sync_file_t *files;
};

/* ------------------------------------------------------------- rules --- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ends_with_ci(const char *s, size_t n, const char *ext)
{
    const size_t m = strlen(ext);
    if (n <= m) {
        return false; /* at least one character before the extension */
    }
    for (size_t i = 0; i < m; i++) {
        if (lower(s[n - m + i]) != ext[i]) {
            return false;
        }
    }
    return true;
}

static bool folder_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool folder_valid(const char *s, size_t n)
{
    if (n < 1 || n > SYNC_FOLDER_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!folder_char(s[i])) {
            return false;
        }
    }
    return true;
}

static bool name_valid(const char *s, size_t n)
{
    if (n < 1 || n > SYNC_NAME_MAX || s[0] == '.' || s[0] == ' ' || s[n - 1] == '.' || s[n - 1] == ' ') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E || strchr("/\\:*?\"<>|", c) != NULL) {
            return false;
        }
    }
    return ends_with_ci(s, n, ".mp3") || ends_with_ci(s, n, ".wav");
}

bool sync_path_valid(const char *path)
{
    if (path == NULL) {
        return false;
    }
    const char *slash = strchr(path, '/');
    if (slash == NULL) {
        return name_valid(path, strlen(path));
    }
    return folder_valid(path, (size_t)(slash - path)) && name_valid(slash + 1, strlen(slash + 1));
}

bool sync_name_managed(const char *name)
{
    if (name == NULL || strchr(name, '/') != NULL) {
        return false;
    }
    const size_t n = strlen(name);
    return ends_with_ci(name, n, ".mp3") || ends_with_ci(name, n, ".wav") || ends_with_ci(name, n, ".part");
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = lower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

bool sync_hex_sha256(const char *hex, uint8_t out[32])
{
    if (hex == NULL || out == NULL || strlen(hex) != 64) {
        return false;
    }
    uint8_t tmp[32];
    for (size_t i = 0; i < 32; i++) {
        const int hi = hex_val(hex[2 * i]);
        const int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        tmp[i] = (uint8_t)(hi << 4 | lo);
    }
    memcpy(out, tmp, sizeof(tmp));
    return true;
}

/* ----------------------------------------------------------- parsing --- */

static bool base_valid(const char *b)
{
    const size_t n = strlen(b);
    return n < SYNC_BASE_MAX && n > 8 && strncmp(b, "https://", 8) == 0 && b[n - 1] == '/';
}

/* JSON number -> uint32 if it is an integer in range. */
static bool get_size(const cJSON *v, uint32_t *out)
{
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    const double d = v->valuedouble;
    if (!(d >= 0.0) || d > 4294967295.0 || d != (double)(uint32_t)d) {
        return false;
    }
    *out = (uint32_t)d;
    return true;
}

static bool listed(const sync_manifest_t *m, const char *path)
{
    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->files[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

sync_manifest_t *sync_manifest_parse(const char *json, size_t len, sync_mf_err_t *err)
{
    sync_mf_err_t dummy;
    if (err == NULL) {
        err = &dummy;
    }
    *err = SYNC_MF_ERR_FORMAT;
    if (json == NULL) {
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return NULL; /* not JSON (or no memory: cJSON does not say) */
    }
    sync_manifest_t *m = NULL;
    const cJSON *base = cJSON_GetObjectItemCaseSensitive(root, "base");
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    if (!cJSON_IsObject(root) || !cJSON_IsString(base) || !base_valid(base->valuestring) || !cJSON_IsArray(files)) {
        goto done;
    }
    m = calloc(1, sizeof(*m));
    const int n = cJSON_GetArraySize(files);
    const size_t cap = (n < SYNC_MAX_FILES) ? (size_t)n : SYNC_MAX_FILES;
    if (m == NULL || (cap > 0 && (m->files = calloc(cap, sizeof(sync_file_t))) == NULL)) {
        free(m);
        m = NULL;
        *err = SYNC_MF_ERR_MEMORY;
        goto done;
    }
    strcpy(m->base, base->valuestring);
    const cJSON *e;
    cJSON_ArrayForEach(e, files)
    {
        const cJSON *path = cJSON_GetObjectItemCaseSensitive(e, "path");
        const cJSON *size = cJSON_GetObjectItemCaseSensitive(e, "size");
        const cJSON *sha = cJSON_GetObjectItemCaseSensitive(e, "sha256");
        sync_file_t f;
        if (m->count >= cap || !cJSON_IsObject(e) || !cJSON_IsString(path) || !sync_path_valid(path->valuestring) ||
            !get_size(size, &f.size) || !cJSON_IsString(sha) || !sync_hex_sha256(sha->valuestring, f.sha256) ||
            listed(m, path->valuestring)) {
            m->skipped++;
            continue;
        }
        strcpy(f.path, path->valuestring); /* length checked by sync_path_valid */
        m->files[m->count++] = f;
    }
    *err = SYNC_MF_OK;
done:
    cJSON_Delete(root);
    return m;
}

void sync_manifest_destroy(sync_manifest_t *m)
{
    if (m != NULL) {
        free(m->files);
        free(m);
    }
}

const char *sync_manifest_base(const sync_manifest_t *m)
{
    return m != NULL ? m->base : NULL;
}

size_t sync_manifest_count(const sync_manifest_t *m)
{
    return m != NULL ? m->count : 0;
}

size_t sync_manifest_skipped(const sync_manifest_t *m)
{
    return m != NULL ? m->skipped : 0;
}

const sync_file_t *sync_manifest_file(const sync_manifest_t *m, size_t index)
{
    return (m != NULL && index < m->count) ? &m->files[index] : NULL;
}

bool sync_manifest_has(const sync_manifest_t *m, const char *path)
{
    return m != NULL && path != NULL && listed(m, path);
}

bool sync_manifest_url(const sync_manifest_t *m, const char *path, char *out, size_t out_size)
{
    if (out != NULL && out_size > 0) {
        out[0] = '\0';
    }
    if (m == NULL || path == NULL || out == NULL || out_size == 0) {
        return false;
    }
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = strlen(m->base);
    if (n + 1 > out_size) {
        return false;
    }
    memcpy(out, m->base, n);
    for (const char *p = path; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '.' || c == '_' || c == '~' || c == '/';
        const size_t need = plain ? 1 : 3;
        if (n + need + 1 > out_size) {
            out[0] = '\0';
            return false;
        }
        if (plain) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = HEX[c >> 4];
            out[n++] = HEX[c & 0x0F];
        }
    }
    out[n] = '\0';
    return true;
}
