/* test_update.c — 自动更新里不碰网络、服务和安装目录的部分（updater/upd_core.c，借鉴设计 A5）：
 * 清单解析与拒收、版本比较与防降级、Ed25519 验签（含 tools/sign_update.py 签出的样例）、安装包大小与 SHA-256 核对，
 * 以及编进 ashdesk_update 的公钥头文件和 packaging/update_pubkey.pem 是否一致 */
#include "upd_core.h"
#include "update_pubkey.h"
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* 仅供本测试的密钥对（tools/sign_update.py genkey 生成），不是发布密钥 */
static const uint8_t TEST_ONLY_SEED[32] = {0xd7, 0x32, 0x4a, 0x65, 0x6b, 0x29, 0x41, 0xbe, 0x5d, 0x28, 0xa3, 0xc7, 0x23, 0x8d, 0x7e, 0x51,
                                           0xa3, 0x30, 0x04, 0xd5, 0x11, 0x43, 0xec, 0xf7, 0x42, 0x75, 0xf0, 0x01, 0x76, 0xc2, 0x9b, 0x21};
static const uint8_t TEST_ONLY_PUB[32] = {0xd7, 0xd3, 0x74, 0xc6, 0x3a, 0xca, 0xe0, 0xfa, 0x66, 0x61, 0x1a, 0xc6, 0xe4, 0xe9, 0x50, 0x97,
                                          0x34, 0x1a, 0x31, 0xff, 0x63, 0xed, 0x40, 0x52, 0x9c, 0x6f, 0x68, 0x6c, 0x12, 0x96, 0x81, 0x9c};

#define V "version=1.2.3\n"
#define U "url=https://example.com/AshDesk-1.2.3-win64.zip\n"
#define S "size=3\n"
#define H "sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"   /* "abc" */
static const char SAMPLE[] = V U S H;
/* tools/sign_update.py sign 用上面的测试私钥对 SAMPLE 签出的签名：Python 签、OpenSSL 验，两边要对得上 */
static const uint8_t SAMPLE_SIG[64] = {0x4f, 0x35, 0xcd, 0x51, 0x03, 0xdb, 0x03, 0x68, 0x4f, 0x81, 0xfb, 0xaa, 0x3f, 0xe8, 0x4e, 0xc4,
                                       0xf9, 0x77, 0xeb, 0xa4, 0x93, 0x17, 0xe9, 0x93, 0xde, 0x54, 0xb8, 0x52, 0x45, 0x07, 0xca, 0x7c,
                                       0x87, 0xb2, 0xe3, 0xd0, 0x27, 0xa9, 0x31, 0x98, 0x45, 0x50, 0xbf, 0xa6, 0x47, 0x68, 0x13, 0x48,
                                       0x39, 0xa3, 0xb8, 0x5e, 0x73, 0x3b, 0x00, 0xa4, 0x64, 0x96, 0xba, 0x3a, 0xaa, 0x8a, 0xd9, 0x02};

static int parses(const char *text) {
    upd_manifest m;
    return upd_manifest_parse(text, strlen(text), &m) == NULL;
}

static upd_version ver(const char *s) {
    upd_version v = {{0, 0, 0}};
    CHECK(upd_version_parse(s, strlen(s), &v) == NULL);
    return v;
}

/* url 值恰好 n 字节的清单能不能过 */
static int url_of_len(size_t n) {
    static const char head[] = V S H "url=https://e/";
    static char text[sizeof head + UPD_URL_MAX + 2];
    const size_t k = n - strlen("https://e/");
    memcpy(text, head, sizeof head - 1);
    memset(text + sizeof head - 1, 'a', k);
    text[sizeof head - 1 + k] = '\n';
    text[sizeof head + k] = 0;
    return parses(text);
}

static void test_manifest(void) {
    upd_manifest m;
    CHECK(upd_manifest_parse(SAMPLE, strlen(SAMPLE), &m) == NULL);
    CHECK(m.version.part[0] == 1 && m.version.part[1] == 2 && m.version.part[2] == 3);
    CHECK(!strcmp(m.url, "https://example.com/AshDesk-1.2.3-win64.zip"));
    CHECK(m.size == 3 && m.sha256[0] == 0xba && m.sha256[31] == 0xad && !m.has_min);
    CHECK(upd_manifest_parse(H S "min_version=1.0.0\n" U V, strlen(H S "min_version=1.0.0\n" U V), &m) == NULL);
    CHECK(m.has_min && m.min_version.part[0] == 1 && m.min_version.part[2] == 0);
    CHECK(parses(V "url=http://127.0.0.1:44395/a.zip?x=1&y=2\n" S H));
    CHECK(parses(V U "size=1073741824\n" H));

    CHECK(!parses(""));
    CHECK(!parses(V U S));                                  /* 缺 sha256 */
    CHECK(!parses(V V U S H));                              /* 重复 */
    CHECK(!parses(V U S H "min_version=1.0.0\nmin_version=1.0.0\n"));
    CHECK(!parses(V U S H "channel=beta\n"));               /* 不认识的键 */
    CHECK(!parses(V U S H "\n"));                           /* 空行 */
    CHECK(!parses(V U S "sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));   /* 末行没换行 */
    CHECK(!parses("version=1.2.3\r\n" U S H));              /* CR */
    CHECK(!parses("version = 1.2.3\n" U S H));              /* 空白 */
    CHECK(!parses("Version=1.2.3\n" U S H));
    CHECK(!parses("version=\n" U S H));
    CHECK(!parses("version=1.2\n" U S H));
    CHECK(!parses(V U S "sha256=BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD\n"));
    CHECK(!parses(V U S "sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a\n"));
    CHECK(!parses(V U S "sha256=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015adf\n"));
    CHECK(!parses(V U S "sha256=ga7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"));
    CHECK(!parses(V U "size=0\n" H));
    CHECK(!parses(V U "size=03\n" H));
    CHECK(!parses(V U "size=1073741825\n" H));              /* 超过 1 GiB */
    CHECK(!parses(V U "size=-3\n" H));
    CHECK(!parses(V "url=ftp://example.com/a.zip\n" S H));
    CHECK(!parses(V "url=https://\n" S H));
    CHECK(!parses(V "url=example.com/a.zip\n" S H));
    CHECK(upd_manifest_parse("version=1.2.3\n\0" U S H, sizeof("version=1.2.3\n\0" U S H) - 1, &m) != NULL);   /* NUL */
    CHECK(upd_manifest_parse("version=1.2.\xe4\n" U S H, strlen("version=1.2.\xe4\n" U S H), &m) != NULL);       /* 非 ASCII */
    CHECK(url_of_len(UPD_URL_MAX - 1));
    CHECK(!url_of_len(UPD_URL_MAX));
}

static void test_versions(void) {
    upd_version v, a, b;
    upd_manifest m;
    char s[UPD_VERSION_STR];
    const char *bad[] = {"", "1", "1.2", "1.2.", "1.2.3.", "1.2.3.4", "01.2.3", "1.02.3", "1.2.03", "65536.0.0", "1.2.3a",
                         "a.2.3", "1..3", "-1.2.3", " 1.2.3", "123456.0.0"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) CHECK(upd_version_parse(bad[i], strlen(bad[i]), &v) != NULL);
    v = ver("65535.0.10");
    upd_version_str(&v, s);
    CHECK(!strcmp(s, "65535.0.10"));
    v = ver("65535.65535.65535");
    upd_version_str(&v, s);
    CHECK(!strcmp(s, "65535.65535.65535"));

    a = ver("0.9.0");
    b = ver("0.10.0");
    CHECK(upd_version_cmp(&a, &b) < 0 && upd_version_cmp(&b, &a) > 0 && upd_version_cmp(&a, &a) == 0);   /* 按数值，不按字符串 */
    a = ver("1.0.0");
    b = ver("0.65535.65535");
    CHECK(upd_version_cmp(&a, &b) > 0);
    a = ver("0.1.1");
    b = ver("0.1.0");
    CHECK(upd_version_cmp(&a, &b) > 0);

    /* 只升不降，同版本也不装；已装版本低于 min_version 不直接升 */
    CHECK(upd_manifest_parse(V U S H "min_version=1.1.0\n", strlen(V U S H "min_version=1.1.0\n"), &m) == NULL);
    a = ver("1.1.0");
    CHECK(upd_manifest_allows(&m, &a) == NULL);
    a = ver("1.2.2");
    CHECK(upd_manifest_allows(&m, &a) == NULL);
    a = ver("1.2.3");
    CHECK(upd_manifest_allows(&m, &a) != NULL);
    a = ver("1.10.0");
    CHECK(upd_manifest_allows(&m, &a) != NULL);
    a = ver("2.0.0");
    CHECK(upd_manifest_allows(&m, &a) != NULL);
    a = ver("1.0.9");
    CHECK(upd_manifest_allows(&m, &a) != NULL);
    m.has_min = 0;
    CHECK(upd_manifest_allows(&m, &a) == NULL);
}

static int sign(const void *msg, size_t len, uint8_t sig[64]) {
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, TEST_ONLY_SEED, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    size_t n = 64;
    const int ok = key && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, key) == 1 && EVP_DigestSign(ctx, sig, &n, msg, len) == 1 && n == 64;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}

static void test_signature(void) {
    static const char other[] = V "url=http://localhost:44395/AshDesk-1.2.3-win64.zip\n" S H;
    char text[sizeof SAMPLE];
    uint8_t sig[64], bad[64];
    const size_t len = strlen(SAMPLE);
    CHECK(upd_signature_ok(TEST_ONLY_PUB, SAMPLE, len, SAMPLE_SIG));
    CHECK(sign(SAMPLE, len, sig) && !memcmp(sig, SAMPLE_SIG, 64));   /* Ed25519 是确定性签名 */
    CHECK(sign(other, strlen(other), sig) && upd_signature_ok(TEST_ONLY_PUB, other, strlen(other), sig));

    CHECK(!upd_signature_ok(TEST_ONLY_PUB, other, strlen(other), SAMPLE_SIG));   /* 签名搬到别的清单上 */
    CHECK(!upd_signature_ok(TEST_ONLY_PUB, SAMPLE, len - 1, SAMPLE_SIG));        /* 截短 */
    memcpy(text, SAMPLE, sizeof text);
    text[8] = '3';                                                              /* version=1.2.3 → 3.2.3 */
    CHECK(!upd_signature_ok(TEST_ONLY_PUB, text, len, SAMPLE_SIG));
    memcpy(bad, SAMPLE_SIG, 64);
    bad[63] ^= 1;
    CHECK(!upd_signature_ok(TEST_ONLY_PUB, SAMPLE, len, bad));
    CHECK(!upd_signature_ok(UPD_PUBKEY, SAMPLE, len, SAMPLE_SIG));              /* 别的密钥 */
}

static void write_file(const wchar_t *path, const char *data) {
    FILE *f = _wfopen(path, L"wb");
    CHECK(f != NULL);
    if (!f) return;
    fwrite(data, 1, strlen(data), f);
    fclose(f);
}

static void test_package(void) {
    static const wchar_t path[] = L"test_update.bin";   /* 相对路径：落在 ctest 的工作目录（构建目录） */
    upd_manifest m;
    CHECK(upd_manifest_parse(SAMPLE, strlen(SAMPLE), &m) == NULL);
    write_file(path, "abc");
    CHECK(upd_package_ok(path, &m) == NULL);
    write_file(path, "abd");
    CHECK(upd_package_ok(path, &m) != NULL);   /* SHA-256 不符 */
    write_file(path, "abcd");
    CHECK(upd_package_ok(path, &m) != NULL);   /* 大小不符 */
    m.size = 4;
    CHECK(upd_package_ok(path, &m) != NULL);   /* 大小对上了，哈希还是不符 */
    DeleteFileW(path);
    CHECK(upd_package_ok(path, &m) != NULL);
}

/* update_pubkey.h 由 tools/sign_update.py 从 packaging/update_pubkey.pem 生成：换了密钥忘了重新生成就在这里发现 */
static void test_pubkey_header(void) {
    uint8_t raw[32];
    size_t n = sizeof raw;
    BIO *bio = BIO_new_file(UPD_PUBKEY_PEM, "r");
    EVP_PKEY *key = bio ? PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL) : NULL;
    CHECK(key && EVP_PKEY_get_raw_public_key(key, raw, &n) == 1 && n == 32 && !memcmp(raw, UPD_PUBKEY, 32));
    EVP_PKEY_free(key);
    BIO_free(bio);
}

int main(void) {
    test_manifest();
    test_versions();
    test_signature();
    test_package();
    test_pubkey_header();
    printf("test_update: checks=%d failures=%d\n", g_checks, g_fail);
    return g_fail != 0;
}
