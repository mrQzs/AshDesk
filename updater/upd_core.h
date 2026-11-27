/* upd_core.h — ashdesk_update 里不碰网络、服务和安装目录的部分（tests/test_update.c 直接测）：
 * 清单解析、版本比较与防降级、Ed25519 签名校验、安装包大小与 SHA-256 核对。
 *
 * 清单（docs/安装与打包.md「自动更新」）：纯 ASCII 的 key=value，每行以 LF 结尾，不许 CR、空行、空白和注释，
 * 每个键恰好出现一次，不认识的键整份拒收：
 *   version=X.Y.Z        新版版本号（三段，每段 0–65535，不带前导零；和 exe 版本资源的前三段对应）
 *   url=https://…        安装包（CPack 出的 ZIP）的绝对网址
 *   size=N               安装包字节数（1 B – 1 GiB）
 *   sha256=…             安装包的 SHA-256，64 位小写十六进制
 *   min_version=X.Y.Z    可选：已装版本低于它就不能直接升级
 * 签名是对清单原样字节的 Ed25519 签名（64 字节），放在清单网址后加 .sig 的地方 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define UPD_MANIFEST_MAX 4096u
#define UPD_URL_MAX 1024u
#define UPD_PACKAGE_MAX (1ull << 30)
#define UPD_SIG_LEN 64u
#define UPD_VERSION_STR 18u                 /* "65535.65535.65535" 加结尾的 0 */

typedef struct {
    uint16_t part[3];
} upd_version;

typedef struct {
    upd_version version, min_version;
    int has_min;
    char url[UPD_URL_MAX];
    uint64_t size;
    uint8_t sha256[32];
} upd_manifest;

/* 返回 NULL 表示通过，否则是一句中文原因（静态字符串，UTF-8） */
const char *upd_version_parse(const char *s, size_t n, upd_version *v);
const char *upd_manifest_parse(const char *text, size_t len, upd_manifest *m);
/* 清单版本必须比已装的新（拒绝降级和重装同一版本，防止把旧的有漏洞版本推回来），且已装版本不低于 min_version */
const char *upd_manifest_allows(const upd_manifest *m, const upd_version *installed);
/* path 处的文件大小与清单一致且 SHA-256 相同 */
const char *upd_package_ok(const wchar_t *path, const upd_manifest *m);

int upd_version_cmp(const upd_version *a, const upd_version *b);
void upd_version_str(const upd_version *v, char out[UPD_VERSION_STR]);
/* 签名对得上返回 1 */
int upd_signature_ok(const uint8_t pub[32], const void *msg, size_t len, const uint8_t sig[UPD_SIG_LEN]);
