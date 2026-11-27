/* upd_core.c — 见 upd_core.h */
#include "upd_core.h"
#include <windows.h>
#include <bcrypt.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

enum { K_VERSION, K_URL, K_SIZE, K_SHA256, K_MIN, K_COUNT };
static const char *const KEYS[K_COUNT] = {"version", "url", "size", "sha256", "min_version"};
#define REQUIRED ((1u << K_VERSION) | (1u << K_URL) | (1u << K_SIZE) | (1u << K_SHA256))

static int key_of(const char *s, size_t n);
static const char *set_value(upd_manifest *m, int key, const char *v, size_t n);
static const char *parse_size(const char *v, size_t n, uint64_t *out);
static const char *parse_hex(const char *v, size_t n, uint8_t out[32]);
static int sha256_file(HANDLE f, uint8_t out[32]);

const char *upd_manifest_parse(const char *text, size_t len, upd_manifest *m) {
    unsigned seen = 0;
    memset(m, 0, sizeof *m);
    if (!len || len > UPD_MANIFEST_MAX || text[len - 1] != '\n') return "清单为空、超过 4096 字节或最后一行没有换行";
    for (size_t at = 0; at < len;) {
        const char *line = text + at;
        const size_t n = (size_t)((const char *)memchr(line, '\n', len - at) - line);
        const char *eq = memchr(line, '=', n);
        const int key = eq ? key_of(line, (size_t)(eq - line)) : -1;
        const char *err;
        if (key < 0) return "清单里有不是 key=value 的行，或不认识的键";
        if (seen & (1u << key)) return "清单里有重复的键";
        seen |= 1u << key;
        if ((err = set_value(m, key, eq + 1, n - (size_t)(eq - line) - 1))) return err;
        at += n + 1;
    }
    if ((seen & REQUIRED) != REQUIRED) return "清单缺少 version、url、size 或 sha256";
    m->has_min = (int)((seen >> K_MIN) & 1u);
    return NULL;
}

const char *upd_manifest_allows(const upd_manifest *m, const upd_version *installed) {
    if (upd_version_cmp(&m->version, installed) <= 0) return "清单的版本不比已装的新（不降级，也不重装同一版本）";
    if (m->has_min && upd_version_cmp(installed, &m->min_version) < 0) return "已装版本低于清单的 min_version，不能直接升级，请手动安装新版";
    return NULL;
}

const char *upd_package_ok(const wchar_t *path, const upd_manifest *m) {
    LARGE_INTEGER size;
    uint8_t got[32];
    const char *err = NULL;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return "打不开安装包";
    if (!GetFileSizeEx(f, &size) || (uint64_t)size.QuadPart != m->size) err = "安装包的大小和清单不符";
    else if (!sha256_file(f, got)) err = "算不出安装包的 SHA-256";
    else if (memcmp(got, m->sha256, sizeof got)) err = "安装包的 SHA-256 和清单不符";
    CloseHandle(f);
    return err;
}

int upd_signature_ok(const uint8_t pub[32], const void *msg, size_t len, const uint8_t sig[UPD_SIG_LEN]) {
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pub, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const int ok = key && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, key) == 1 &&
                   EVP_DigestVerify(ctx, sig, UPD_SIG_LEN, msg, len) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

const char *upd_version_parse(const char *s, size_t n, upd_version *v) {
    size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        unsigned value = 0;
        const size_t start = at;
        while (at < n && s[at] >= '0' && s[at] <= '9' && at - start < 5) value = value * 10u + (unsigned)(s[at++] - '0');
        if (at == start || value > 0xFFFFu || (s[start] == '0' && at - start > 1)) return "版本号要写成 X.Y.Z，每段 0–65535，不带前导零";
        v->part[i] = (uint16_t)value;
        if (i < 2) {
            if (at >= n || s[at] != '.') return "版本号要写成 X.Y.Z，每段 0–65535，不带前导零";
            ++at;
        }
    }
    return at == n ? NULL : "版本号要写成 X.Y.Z，每段 0–65535，不带前导零";
}

int upd_version_cmp(const upd_version *a, const upd_version *b) {
    for (int i = 0; i < 3; ++i)
        if (a->part[i] != b->part[i]) return a->part[i] < b->part[i] ? -1 : 1;
    return 0;
}

void upd_version_str(const upd_version *v, char out[UPD_VERSION_STR]) {
    snprintf(out, UPD_VERSION_STR, "%u.%u.%u", (unsigned)v->part[0], (unsigned)v->part[1], (unsigned)v->part[2]);
}

static int key_of(const char *s, size_t n) {
    for (int k = 0; k < K_COUNT; ++k)
        if (strlen(KEYS[k]) == n && !memcmp(KEYS[k], s, n)) return k;
    return -1;
}

static const char *set_value(upd_manifest *m, int key, const char *v, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (v[i] < 0x21 || v[i] > 0x7E) return "清单的值里有空白、控制字符或非 ASCII 字符";
    if (!n) return "清单里有空值";
    switch (key) {
    case K_VERSION: return upd_version_parse(v, n, &m->version);
    case K_MIN: return upd_version_parse(v, n, &m->min_version);
    case K_SIZE: return parse_size(v, n, &m->size);
    case K_SHA256: return parse_hex(v, n, m->sha256);
    default:
        if (n >= UPD_URL_MAX || !((n > 8 && !memcmp(v, "https://", 8)) || (n > 7 && !memcmp(v, "http://", 7))))
            return "url 要是 1024 字节以内的 http(s) 绝对网址";
        memcpy(m->url, v, n);
        m->url[n] = 0;
        return NULL;
    }
}

static const char *parse_size(const char *v, size_t n, uint64_t *out) {
    uint64_t value = 0;
    if (n > 10 || v[0] == '0') return "size 要是 1 到 1 GiB 之间、不带前导零的十进制数";
    for (size_t i = 0; i < n; ++i) {
        if (v[i] < '0' || v[i] > '9') return "size 要是 1 到 1 GiB 之间、不带前导零的十进制数";
        value = value * 10u + (uint64_t)(v[i] - '0');
    }
    if (value > UPD_PACKAGE_MAX) return "size 要是 1 到 1 GiB 之间、不带前导零的十进制数";
    *out = value;
    return NULL;
}

static const char *parse_hex(const char *v, size_t n, uint8_t out[32]) {
    if (n != 64) return "sha256 要是 64 位小写十六进制";
    for (size_t i = 0; i < 64; ++i) {
        unsigned d;
        if (v[i] >= '0' && v[i] <= '9') d = (unsigned)(v[i] - '0');
        else if (v[i] >= 'a' && v[i] <= 'f') d = (unsigned)(v[i] - 'a') + 10u;
        else return "sha256 要是 64 位小写十六进制";
        out[i / 2] = (uint8_t)(((unsigned)out[i / 2] << 4) | d);
    }
    return NULL;
}

static int sha256_file(HANDLE f, uint8_t out[32]) {
    static uint8_t buf[1 << 16];
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    DWORD n = 0;
    int ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) &&
             BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0));
    while (ok && (ok = ReadFile(f, buf, sizeof buf, &n, NULL)) != 0 && n) ok = BCRYPT_SUCCESS(BCryptHashData(h, buf, n, 0));
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(h, out, 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
