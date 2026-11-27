/* update_main.c — AshDesk 自动更新（借鉴设计 A5，docs/安装与打包.md「自动更新」）。单独的进程，不在主机和界面里跑：
 *   ashdesk_update check --manifest URL
 *       标准输出一行：available <新版本> <已装版本>，或 latest <已装版本>
 *   ashdesk_update apply --manifest URL [--install-dir DIR] [--dry-run]
 *       验清单签名 → 安装包下载到 DIR 旁边、核对大小和 SHA-256 → 解压到 DIR.new → 没有会话时停服务
 *       → DIR 改名 DIR.old、DIR.new 改名 DIR → 新版自检、起服务；哪步失败就换回旧目录。--dry-run 解压完就停
 *   ashdesk_update rollback [--install-dir DIR]
 *       把 DIR.old（上次更新前的版本）换回 DIR
 * 选项：--service NAME 要停、起的服务（默认 AshDeskSvc，没装就跳过）；--log FILE（默认 %ProgramData%\AshDesk\update.log）。
 * 不给 --install-dir 时取本程序所在 bin 目录的上一级。退出码：0 成功，1 失败或不更新，2 用法不对 */
#include "upd_core.h"
#include "update_pubkey.h"
#include "ashdesk_version.h"
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <winver.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* 主机在有客户端会话期间持有的全局互斥量（主机崩溃时系统自动释放）。名字是和引擎的约定，见 docs/核心接口.md */
#define SESSION_MARK L"Global\\AshDeskSessionActive"
#define DIR_MAX (MAX_PATH - 40)            /* 安装目录路径上限：后面还要接 .new\bin\adrd_host.exe */

typedef enum { CMD_CHECK, CMD_APPLY, CMD_ROLLBACK } command;

typedef struct {
    command cmd;
    const wchar_t *manifest, *dir, *service, *log;
    int dry_run;
} options;

/* 安装目录 DIR 和它旁边的 DIR.new（新版解压处）、DIR.old（上一版）、DIR.new.zip（下载的安装包）：同一个卷，改名即换 */
typedef struct {
    wchar_t dir[MAX_PATH], next[MAX_PATH], old[MAX_PATH], zip[MAX_PATH];
} paths;

typedef enum { SVC_ABSENT, SVC_IDLE, SVC_STOPPED } svc_was;   /* 服务没装 / 装了没在跑 / 本程序停掉的 */

static wchar_t g_log[MAX_PATH], g_sys[MAX_PATH];

static int cmd_check(const options *o, const paths *p);
static int cmd_apply(const options *o, const paths *p);
static int cmd_rollback(const options *o, const paths *p);
static int parse_args(int argc, wchar_t **argv, options *o);
static void usage(void);
static void open_log(const wchar_t *given);
static int install_paths(const wchar_t *given, paths *p);
static int fetch_manifest(const wchar_t *url, upd_manifest *m);
static int download(const upd_manifest *m, const wchar_t *zip);
static int http_get(const wchar_t *url, HANDLE out, uint8_t *buf, uint64_t cap, uint64_t *got);
static int extract(const paths *p);
static int carry_over(const wchar_t *from, const wchar_t *to);
static int swap_in(const paths *p, const wchar_t *svc);
static void swap_back(const paths *p, const wchar_t *svc, svc_was was);
static int swap_dirs(const wchar_t *dir, const wchar_t *aside, const wchar_t *from);
static int quiesce(const wchar_t *dir, const wchar_t *svc, svc_was *was);
static int session_active(void);
static int running_from(const wchar_t *dir);
static int service_stop(const wchar_t *name, svc_was *was);
static int service_resume(const wchar_t *name, svc_was was);
static int healthy(const wchar_t *dir);
static int exe_version(const wchar_t *dir, upd_version *v);
static int remove_leftovers(const paths *p);
static int remove_tree(const wchar_t *dir);
static int rename_dir(const wchar_t *from, const wchar_t *to);
static void logmsg(const char *fmt, ...);
static int fail(const char *what, const wchar_t *path);
static const char *u8(const wchar_t *w);

int main(void) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    options o;
    paths p;
    UINT n;
    SetConsoleOutputCP(CP_UTF8);
    /* 子进程继承这个模式：坏掉的新版自检时缺 DLL / 映像损坏只返回错误码，不在桌面弹系统错误框卡住等人点 */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    if (!argv || !parse_args(argc, argv, &o)) {
        usage();
        return 2;
    }
    open_log(o.log);
    if (!install_paths(o.dir, &p)) return 1;
    /* 进程的当前目录在安装目录里时整个目录改不了名：挪到系统目录（tar 也从这里找） */
    n = GetSystemDirectoryW(g_sys, MAX_PATH - 16);
    if (!n || n >= MAX_PATH - 16 || !SetCurrentDirectoryW(g_sys)) {
        fail("进入", L"系统目录");
        return 1;
    }
    switch (o.cmd) {
    case CMD_CHECK: return cmd_check(&o, &p);
    case CMD_APPLY: return cmd_apply(&o, &p);
    default: return cmd_rollback(&o, &p);
    }
}

static int cmd_check(const options *o, const paths *p) {
    upd_version have;
    upd_manifest m;
    char now[UPD_VERSION_STR], next[UPD_VERSION_STR];
    const char *err;
    if (!exe_version(p->dir, &have) || !fetch_manifest(o->manifest, &m)) return 1;
    upd_version_str(&have, now);
    upd_version_str(&m.version, next);
    if (upd_version_cmp(&m.version, &have) <= 0) {
        printf("latest %s\n", now);
    } else if ((err = upd_manifest_allows(&m, &have)) != NULL) {
        logmsg("有新版 %s，但%s", next, err);
        return 1;
    } else {
        printf("available %s %s\n", next, now);
    }
    return 0;
}

static int cmd_apply(const options *o, const paths *p) {
    upd_version have, got;
    upd_manifest m;
    char now[UPD_VERSION_STR], next[UPD_VERSION_STR];
    const char *err;
    if (!exe_version(p->dir, &have) || !fetch_manifest(o->manifest, &m)) return 1;
    upd_version_str(&have, now);
    upd_version_str(&m.version, next);
    if ((err = upd_manifest_allows(&m, &have)) != NULL) {
        logmsg("不更新（已装 %s，清单 %s）：%s", now, next, err);
        return 1;
    }
    logmsg("%s：%s → %s", u8(p->dir), now, next);
    if (!remove_leftovers(p) || !download(&m, p->zip) || !extract(p) || !exe_version(p->next, &got)) return 1;
    if (upd_version_cmp(&got, &m.version)) {
        logmsg("安装包里的 adrd_host.exe 不是清单写的 %s", next);
        return 1;
    }
    if (!carry_over(p->dir, p->next)) return 1;
    DeleteFileW(p->zip);
    if (o->dry_run) {
        logmsg("演练：新版已解压到 %s，安装目录没动", u8(p->next));
        return 0;
    }
    if (!swap_in(p, o->service)) return 1;
    logmsg("已更新到 %s；上一版留在 %s（rollback 可换回），下次更新时删除", next, u8(p->old));
    return 0;
}

static int cmd_rollback(const options *o, const paths *p) {
    upd_version v;
    char ver[UPD_VERSION_STR];
    svc_was was;
    int ok;
    if (GetFileAttributesW(p->old) == INVALID_FILE_ATTRIBUTES) {
        logmsg("没有可换回的上一版：%s 不存在", u8(p->old));
        return 1;
    }
    if (!exe_version(p->old, &v)) return 1;
    upd_version_str(&v, ver);
    if (!remove_tree(p->next)) {
        fail("删除", p->next);
        return 1;
    }
    if (!quiesce(p->dir, o->service, &was)) return 1;
    /* 上次换回时第二次改名失败会留下没有 DIR 的状态：这时直接把 DIR.old 改回来 */
    ok = GetFileAttributesW(p->dir) == INVALID_FILE_ATTRIBUTES ? rename_dir(p->old, p->dir) : swap_dirs(p->dir, p->next, p->old);
    ok = service_resume(o->service, was) && ok;
    if (ok) logmsg("已换回 %s；换下来的版本在 %s", ver, u8(p->next));
    return !ok;
}

static int parse_args(int argc, wchar_t **argv, options *o) {
    memset(o, 0, sizeof *o);
    o->service = L"AshDeskSvc";
    if (argc < 2) return 0;
    if (!wcscmp(argv[1], L"check")) o->cmd = CMD_CHECK;
    else if (!wcscmp(argv[1], L"apply")) o->cmd = CMD_APPLY;
    else if (!wcscmp(argv[1], L"rollback")) o->cmd = CMD_ROLLBACK;
    else return 0;
    for (int i = 2; i < argc; ++i) {
        const wchar_t *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!wcscmp(a, L"--dry-run")) {
            o->dry_run = 1;
            continue;
        }
        if (!v) return 0;
        if (!wcscmp(a, L"--manifest")) o->manifest = v;
        else if (!wcscmp(a, L"--install-dir")) o->dir = v;
        else if (!wcscmp(a, L"--service")) o->service = v;
        else if (!wcscmp(a, L"--log")) o->log = v;
        else return 0;
        ++i;
    }
    if (o->cmd == CMD_ROLLBACK) return !o->manifest && !o->dry_run;
    return o->manifest && (o->cmd == CMD_APPLY || !o->dry_run);
}

static void usage(void) {
    fputs("用法：ashdesk_update check --manifest URL\n"
          "      ashdesk_update apply --manifest URL [--install-dir DIR] [--dry-run]\n"
          "      ashdesk_update rollback [--install-dir DIR]\n"
          "选项：--service NAME  要停、起的服务（默认 AshDeskSvc，没装就跳过）\n"
          "      --log FILE      日志（默认 %ProgramData%\\AshDesk\\update.log）\n",
          stderr);
}

/* 日志文件：--log 给的路径，或 %ProgramData%\AshDesk\update.log；都放不了就只打到标准错误 */
static void open_log(const wchar_t *given) {
    DWORD n;
    if (given) {
        n = GetFullPathNameW(given, MAX_PATH, g_log, NULL);
        if (!n || n >= MAX_PATH) g_log[0] = 0;
        return;
    }
    n = GetEnvironmentVariableW(L"ProgramData", g_log, MAX_PATH);
    if (!n || n > MAX_PATH - 24) {
        g_log[0] = 0;
        return;
    }
    wcscat(g_log, L"\\AshDesk");
    CreateDirectoryW(g_log, NULL);
    wcscat(g_log, L"\\update.log");
}

/* 安装目录整理成不带结尾反斜杠的绝对路径；不给时本程序须在 DIR\bin 下 */
static int install_paths(const wchar_t *given, paths *p) {
    wchar_t *d = p->dir;
    DWORD n = given ? GetFullPathNameW(given, MAX_PATH, d, NULL) : GetModuleFileNameW(NULL, d, MAX_PATH);
    if (!given && n && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(d, L'\\');
        if (slash) *slash = 0;
        slash = wcsrchr(d, L'\\');
        if (!slash || _wcsicmp(slash + 1, L"bin")) {
            logmsg("本程序不在安装目录的 bin 下：用 --install-dir 指定安装目录");
            return 0;
        }
        *slash = 0;
        n = (DWORD)wcslen(d);
    }
    while (n && n < MAX_PATH && d[n - 1] == L'\\') d[--n] = 0;
    if (!n || n > DIR_MAX || !wcschr(d, L'\\')) {
        logmsg("安装目录不对（不能是盘符根目录，路径不超过 %d 个字符）", DIR_MAX);
        return 0;
    }
    swprintf(p->next, MAX_PATH, L"%ls.new", d);
    swprintf(p->old, MAX_PATH, L"%ls.old", d);
    swprintf(p->zip, MAX_PATH, L"%ls.new.zip", d);
    return 1;
}

/* 下载清单和它的签名（清单网址后加 .sig），先验签名再解析 */
static int fetch_manifest(const wchar_t *url, upd_manifest *m) {
    static uint8_t text[UPD_MANIFEST_MAX], sig[UPD_SIG_LEN];
    wchar_t sig_url[UPD_URL_MAX + 8];
    uint64_t n = 0, ns = 0;
    const char *err;
    if (wcslen(url) >= UPD_URL_MAX) {
        logmsg("清单网址超过 %u 个字符", UPD_URL_MAX);
        return 0;
    }
    swprintf(sig_url, UPD_URL_MAX + 8, L"%ls.sig", url);
    if (!http_get(url, NULL, text, sizeof text, &n) || !http_get(sig_url, NULL, sig, sizeof sig, &ns)) return 0;
    if (ns != UPD_SIG_LEN || !upd_signature_ok(UPD_PUBKEY, text, (size_t)n, sig)) {
        logmsg("清单的签名不对：不是发布密钥签的，或清单被改过");
        return 0;
    }
    if ((err = upd_manifest_parse((const char *)text, (size_t)n, m)) != NULL) {
        logmsg("清单格式不对：%s", err);
        return 0;
    }
    return 1;
}

static int download(const upd_manifest *m, const wchar_t *zip) {
    wchar_t url[UPD_URL_MAX];
    uint64_t got = 0;
    const char *err;
    int ok;
    HANDLE f = CreateFileW(zip, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return fail("创建", zip);
    MultiByteToWideChar(CP_UTF8, 0, m->url, -1, url, (int)UPD_URL_MAX);
    ok = http_get(url, f, NULL, m->size, &got);
    CloseHandle(f);
    if (ok && (err = upd_package_ok(zip, m)) != NULL) {
        logmsg("下载的安装包不对：%s", err);
        ok = 0;
    }
    if (ok) logmsg("已下载并核对安装包（%llu 字节）", (unsigned long long)got);
    else DeleteFileW(zip);
    return ok;
}

/* 拆网址并核对协议：https，或本机（127.0.0.1 / localhost）的 http（测试用） */
static int crack_url(const wchar_t *url, URL_COMPONENTS *u, wchar_t host[256]) {
    memset(u, 0, sizeof *u);
    u->dwStructSize = sizeof *u;
    u->dwSchemeLength = u->dwHostNameLength = u->dwUrlPathLength = u->dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, u) || !u->dwHostNameLength || u->dwHostNameLength >= 256) {
        logmsg("网址不对：%s", u8(url));
        return 0;
    }
    wmemcpy(host, u->lpszHostName, u->dwHostNameLength);
    host[u->dwHostNameLength] = 0;
    if (u->nScheme == INTERNET_SCHEME_HTTPS ||
        (u->nScheme == INTERNET_SCHEME_HTTP && (!_wcsicmp(host, L"127.0.0.1") || !_wcsicmp(host, L"localhost"))))
        return 1;
    logmsg("只接受 https 网址（http 只许 127.0.0.1 和 localhost）：%s", u8(url));
    return 0;
}

/* GET，要 200；内容写进 buf，buf 为 NULL 时写进文件 out；超过 cap 字节就放弃 */
static int http_get(const wchar_t *url, HANDLE out, uint8_t *buf, uint64_t cap, uint64_t *got) {
    static uint8_t chunk[1 << 16];
    URL_COMPONENTS u;
    wchar_t host[256];
    HINTERNET s = NULL, c = NULL, r = NULL;
    DWORD status = 0, len = sizeof status, n = 0, written = 0;
    int ok;
    *got = 0;
    if (!crack_url(url, &u, host)) return 0;
    ok = (s = WinHttpOpen(L"AshDeskUpdate/" ASHDESK_VERSION_STRING, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                          WINHTTP_NO_PROXY_BYPASS, 0)) != NULL &&
         WinHttpSetTimeouts(s, 10000, 10000, 30000, 30000) && (c = WinHttpConnect(s, host, u.nPort, 0)) != NULL &&
         (r = WinHttpOpenRequest(c, L"GET", u.lpszUrlPath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)) != NULL &&
         WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, NULL) &&
         WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                             WINHTTP_NO_HEADER_INDEX);
    if (!ok) {
        fail("下载", url);
    } else if (status != 200) {
        logmsg("下载 %s 失败：HTTP %lu", u8(url), status);
        ok = 0;
    }
    while (ok) {
        if (!WinHttpReadData(r, chunk, sizeof chunk, &n)) {
            ok = fail("接收", url);
        } else if (!n) {
            break;
        } else if (n > cap - *got) {
            logmsg("%s 超过了预期的 %llu 字节", u8(url), (unsigned long long)cap);
            ok = 0;
        } else if (buf) {
            memcpy(buf + (size_t)*got, chunk, n);
            *got += n;
        } else if (WriteFile(out, chunk, n, &written, NULL) && written == n) {
            *got += n;
        } else {
            ok = fail("保存", url);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

/* 不开窗口跑一个子进程（输入输出都接空设备），等它退出，超时就结束它 */
static int run(const wchar_t *app, wchar_t *cmd, DWORD timeout_ms, DWORD *code) {
    SECURITY_ATTRIBUTES sa = {(DWORD)sizeof sa, NULL, TRUE};
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    int ok;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = nul;
    ok = nul != INVALID_HANDLE_VALUE && CreateProcessW(app, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) fail("启动", app);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) return 0;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        ok = GetExitCodeProcess(pi.hProcess, code);
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
        logmsg("%s 超过 %lu 秒没结束", u8(app), timeout_ms / 1000);
        ok = 0;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

/* 用系统自带的 tar（bsdtar，Windows 10 1803 起）解 ZIP；包里的文件都在 AshDesk-<版本>-win64\ 下，剥掉这一层。
 * bsdtar 默认拒绝绝对路径和带 .. 的条目，解不到 DIR.new 外面 */
static int extract(const paths *p) {
    wchar_t tar[MAX_PATH], cmd[3 * MAX_PATH];
    DWORD code = 0;
    swprintf(tar, MAX_PATH, L"%ls\\tar.exe", g_sys);
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" -xf \"%ls\" --strip-components 1 -C \"%ls\"", tar, p->zip, p->next);
    if (!CreateDirectoryW(p->next, NULL)) return fail("创建", p->next);
    if (!run(tar, cmd, 600000, &code)) return 0;
    if (code) {
        logmsg("解压 %s 失败：tar 退出码 %lu", u8(p->zip), code);
        return 0;
    }
    return 1;
}

/* 旧目录里有、新包里没有的文件照搬进 DIR.new：install.ps1 下载的 OpenH264 DLL、主机写在 bin 里的 resume.bin、
 * 用户自己放的文件。不跟链接。新版删掉的文件也会被当成用户文件留下 */
static int carry_over(const wchar_t *from, const wchar_t *to) {
    wchar_t pattern[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    if (swprintf(pattern, MAX_PATH, L"%ls\\*", from) < 0 || (h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return fail("列出", from);
    do {
        const DWORD a = fd.dwFileAttributes;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || (a & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (swprintf(src, MAX_PATH, L"%ls\\%ls", from, fd.cFileName) < 0 || swprintf(dst, MAX_PATH, L"%ls\\%ls", to, fd.cFileName) < 0)
            ok = fail("拼接路径", fd.cFileName);
        else if (a & FILE_ATTRIBUTE_DIRECTORY)
            ok = (CreateDirectoryW(dst, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) ? carry_over(src, dst) : fail("创建", dst);
        else if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES)
            continue;
        else if (CopyFileW(src, dst, TRUE))
            logmsg("保留 %s", u8(src));
        else
            ok = fail("复制", src);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* 停下 → 两次改名换上新版 → 自检、起服务；自检或服务没过就换回旧版 */
static int swap_in(const paths *p, const wchar_t *svc) {
    svc_was was;
    if (!quiesce(p->dir, svc, &was)) return 0;
    if (!swap_dirs(p->dir, p->old, p->next)) {
        service_resume(svc, was);
        return 0;
    }
    if (healthy(p->dir) && service_resume(svc, was)) return 1;
    logmsg("新版没通过检查，换回旧版");
    swap_back(p, svc, was);
    return 0;
}

/* 停掉新版可能半起来的服务，DIR 挪回 DIR.new、DIR.old 换回 DIR，旧版服务照原样起 */
static void swap_back(const paths *p, const wchar_t *svc, svc_was was) {
    svc_was ignored;
    if (service_stop(svc, &ignored) && swap_dirs(p->dir, p->next, p->old)) {
        if (service_resume(svc, was)) logmsg("已换回旧版");
        return;
    }
    logmsg("回滚没完成：关掉占着安装目录的程序后运行 ashdesk_update rollback --install-dir \"%s\"", u8(p->dir));
}

/* 两次改名：dir 挪到 aside，from 改名为 dir；第二次失败就把第一次改回去，安装目录不会空着 */
static int swap_dirs(const wchar_t *dir, const wchar_t *aside, const wchar_t *from) {
    if (!rename_dir(dir, aside)) return 0;
    if (rename_dir(from, dir)) return 1;
    rename_dir(aside, dir);
    return 0;
}

/* 安装目录里的程序都停下来才能改名：有会话不动；服务在跑就停掉；还有程序从安装目录里跑着也不动 */
static int quiesce(const wchar_t *dir, const wchar_t *svc, svc_was *was) {
    if (session_active()) {
        logmsg("有远程会话进行中，先不动安装目录");
        return 0;
    }
    if (!service_stop(svc, was)) return 0;
    if (!running_from(dir)) return 1;
    service_resume(svc, *was);
    return 0;
}

/* 会话标记在：别的用户建的互斥量可能打不开（拒绝访问），但存在就算 */
static int session_active(void) {
    HANDLE mark = OpenMutexW(SYNCHRONIZE, FALSE, SESSION_MARK);
    const DWORD e = GetLastError();
    if (mark) CloseHandle(mark);
    return mark || e != ERROR_FILE_NOT_FOUND;
}

/* 还有程序从安装目录里跑着（本程序自己除外）：界面、手动起的主机或客户端；列不出进程时按有算 */
static int running_from(const wchar_t *dir) {
    PROCESSENTRY32W pe;
    const size_t n = wcslen(dir);
    int found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        (void)fail("列出", L"进程");
        return 1;
    }
    pe.dwSize = sizeof pe;
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
        wchar_t image[MAX_PATH];
        DWORD len = MAX_PATH;
        HANDLE h = pe.th32ProcessID == GetCurrentProcessId() ? NULL : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!h) continue;
        if (QueryFullProcessImageNameW(h, 0, image, &len) && len > n && image[n] == L'\\' && !_wcsnicmp(image, dir, n)) {
            logmsg("%s（进程 %lu）还在运行，先关掉它", u8(image), pe.th32ProcessID);
            found = 1;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

/* 等服务到 want 状态，最多 30 秒；等 RUNNING 时它停了就不再等 */
static int wait_state(SC_HANDLE s, DWORD want) {
    SERVICE_STATUS st;
    for (int i = 0; i < 300; ++i) {
        if (!QueryServiceStatus(s, &st)) return 0;
        if (st.dwCurrentState == want) return 1;
        if (want == SERVICE_RUNNING && st.dwCurrentState == SERVICE_STOPPED) return 0;
        Sleep(100);
    }
    return 0;
}

/* 服务在跑就停掉并等它停稳；没装这个服务就跳过 */
static int service_stop(const wchar_t *name, svc_was *was) {
    SERVICE_STATUS st;
    SC_HANDLE m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), s = NULL;
    int ok = 0;
    *was = SVC_ABSENT;
    if (m) s = OpenServiceW(m, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (m && !s && GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) {
        ok = 1;
    } else if (s && QueryServiceStatus(s, &st)) {
        *was = st.dwCurrentState == SERVICE_STOPPED ? SVC_IDLE : SVC_STOPPED;
        ok = *was == SVC_IDLE || ((ControlService(s, SERVICE_CONTROL_STOP, &st) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE) &&
                                  wait_state(s, SERVICE_STOPPED));
    }
    if (!ok) fail("停服务", name);
    else if (*was == SVC_ABSENT) logmsg("没装服务 %s，不用停", u8(name));
    else if (*was == SVC_STOPPED) logmsg("已停服务 %s", u8(name));
    if (s) CloseServiceHandle(s);
    if (m) CloseServiceHandle(m);
    return ok;
}

/* 服务原来在跑才重新起；起来 3 秒后还在跑才算数（新版一起来就崩的情况） */
static int service_resume(const wchar_t *name, svc_was was) {
    SERVICE_STATUS st;
    SC_HANDLE m, s;
    int ok;
    if (was != SVC_STOPPED) return 1;
    m = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    s = m ? OpenServiceW(m, name, SERVICE_START | SERVICE_QUERY_STATUS) : NULL;
    ok = s && (StartServiceW(s, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) && wait_state(s, SERVICE_RUNNING);
    if (ok) {
        Sleep(3000);
        ok = QueryServiceStatus(s, &st) && st.dwCurrentState == SERVICE_RUNNING;
    }
    if (ok) logmsg("已起服务 %s", u8(name));
    else fail("起服务（或起来后又停了）", name);
    if (s) CloseServiceHandle(s);
    if (m) CloseServiceHandle(m);
    return ok;
}

/* 新版自检：adrd_host --list 能启动（导入的 DLL 齐全、能加载）并正常退出 */
static int healthy(const wchar_t *dir) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 16];
    DWORD code = 0;
    swprintf(exe, MAX_PATH, L"%ls\\bin\\adrd_host.exe", dir);
    swprintf(cmd, MAX_PATH + 16, L"\"%ls\" --list", exe);
    if (!run(exe, cmd, 30000, &code)) return 0;
    if (code) {
        logmsg("新版自检没过：adrd_host --list 退出码 0x%lX", code);
        return 0;
    }
    logmsg("新版自检通过");
    return 1;
}

/* DIR\bin\adrd_host.exe 版本资源的前三段（来自 CMake 的 project(VERSION)） */
static int exe_version(const wchar_t *dir, upd_version *v) {
    wchar_t exe[MAX_PATH];
    DWORD ignored = 0, size = 0;
    void *block = NULL, *fixed = NULL;
    UINT n = 0;
    int ok;
    swprintf(exe, MAX_PATH, L"%ls\\bin\\adrd_host.exe", dir);
    ok = (size = GetFileVersionInfoSizeW(exe, &ignored)) != 0 && (block = malloc(size)) != NULL && GetFileVersionInfoW(exe, 0, size, block) &&
         VerQueryValueW(block, L"\\", &fixed, &n) && n >= sizeof(VS_FIXEDFILEINFO);
    if (ok) {
        const VS_FIXEDFILEINFO *fi = fixed;
        v->part[0] = HIWORD(fi->dwFileVersionMS);
        v->part[1] = LOWORD(fi->dwFileVersionMS);
        v->part[2] = HIWORD(fi->dwFileVersionLS);
    } else {
        logmsg("读不到 %s 的版本号（不是 AshDesk 的安装目录？）", u8(exe));
    }
    free(block);
    return ok;
}

/* 上次留下的 DIR.old、DIR.new、DIR.new.zip */
static int remove_leftovers(const paths *p) {
    if (!DeleteFileW(p->zip) && GetLastError() != ERROR_FILE_NOT_FOUND) return fail("删除", p->zip);
    if (!remove_tree(p->old)) return fail("删除", p->old);
    if (!remove_tree(p->next)) return fail("删除", p->next);
    return 1;
}

/* 删掉整棵目录树，不存在也算成功；目录联接、符号链接只删链接本身，不跟进去 */
static int remove_tree(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int ok = 1;
    if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (swprintf(pattern, MAX_PATH, L"%ls\\*", dir) < 0 || (h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        const DWORD a = fd.dwFileAttributes;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (swprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) < 0) ok = 0;
        else if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) ok = remove_tree(sub);
        else if (a & FILE_ATTRIBUTE_DIRECTORY) ok = RemoveDirectoryW(sub);
        else ok = SetFileAttributesW(sub, FILE_ATTRIBUTE_NORMAL) && DeleteFileW(sub);
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok && RemoveDirectoryW(dir);
}

static int rename_dir(const wchar_t *from, const wchar_t *to) {
    DWORD e;
    if (MoveFileExW(from, to, 0)) {
        logmsg("%s → %s", u8(from), u8(to));
        return 1;
    }
    e = GetLastError();
    logmsg("%s 改名为 %s 失败：错误 %lu%s", u8(from), u8(to), e,
           e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? "（目录里有打开的文件、是某个程序的当前目录，或资源管理器开着它）" : "");
    return 0;
}

/* 一行日志：带时间和进程号追加进日志文件（每次打开、写完就关，不占着文件），同时打到标准错误 */
static void logmsg(const char *fmt, ...) {
    char line[8192];
    SYSTEMTIME t;
    va_list ap;
    size_t head, len;
    HANDLE f;
    DWORD written;
    GetLocalTime(&t);
    head = (size_t)snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d [%lu] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                            t.wSecond, GetCurrentProcessId());
    va_start(ap, fmt);
    vsnprintf(line + head, sizeof line - head - 1, fmt, ap);
    va_end(ap);
    len = strlen(line);
    line[len++] = '\n';
    fwrite(line + head, 1, len - head, stderr);
    if (!g_log[0]) return;
    f = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    WriteFile(f, line, (DWORD)len, &written, NULL);
    CloseHandle(f);
}

/* 记“<做什么> <路径> 失败：错误 N”，返回 0 */
static int fail(const char *what, const wchar_t *path) {
    const DWORD e = GetLastError();
    logmsg("%s %s 失败：错误 %lu", what, u8(path), e);
    return 0;
}

/* 宽字符串转 UTF-8 给日志用；轮换几块静态缓冲，一条日志里能用好几次 */
static const char *u8(const wchar_t *w) {
    static char buf[4][4 * UPD_URL_MAX];
    static unsigned next;
    char *b = buf[next++ % 4u];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, b, (int)sizeof buf[0], NULL, NULL)) b[0] = 0;
    return b;
}
