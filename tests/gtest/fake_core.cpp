// 界面测试用的假核心：测试把它复制成 adrd_host / adrd_client 放进临时目录，由真实的 MainWindow / CoreProcess 拉起。
// 只照 docs/核心接口.md 和界面用到的命令行约定打印状态行，不含业务逻辑：
//   每次调用把参数追加进工作目录的 calls.txt；
//   --devices / --netdiag / --probe：原样打印工作目录的 devices.txt / netdiag.txt / probe.txt 后退出；--device-perm / --revoke：直接退出；
//   客户端（带 --host）：有 totp.txt 而 --totp 的值和它不同时报 error=5、退出码 3，否则报 stream 后等命令；
//   客户端（带 --connect 按号码连，或 --host … --by-number 按地址连、按号码认），顺序照真客户端（proto/client/client_session.c 的 known_or_password）：
//         --password -：先从 stdin 读一行当密码，记进工作目录的 stdin.txt；
//         known.txt 说对方记不记得这台设备：没有这个文件 = 不记得，trusted = 记得，revoked = 记得过、已经吊销，offline = 记得、可是对方不在线，
//         broken = 核心自己出了错，什么都不报、以 1 退出；
//         没给密码、对方也不记得：真客户端这时先问信令服务（password_precheck），不报阶段，以退出码 3 结束。
//         信令服务怎么答由 presence.txt 安排（照真客户端的原话写那一行 @ui error=…），没安排就是“收密码”：error=30；
//         之后报 lookup；offline 报 error=26；revoked 而没给密码报 error=29；给了密码、对方不记得、
//         有 password.txt 而密码和它不同报 error=20（有 wrong.txt 就改报它里面的那一行），都是退出码 3；否则报 stream 后等命令；
//   主机：带 --capture idd 时报 need driver=vdisplay 后以 1 退出（真主机打不开虚拟屏驱动时就是这样，见 proto/host/host_vd.c）；
//         工作目录里有 noscreen.txt 时报 need screen=1 后以 1 退出（真主机在采不到屏幕的会话里就是这样，见 proto/host/host_capture.c）；
//         否则报 listening，带 --pair / --pair-temp 时再报配对码，带 --number / --password 时报号码 / 连接密码
//         （带 --password 时先报固定密码设没设：工作目录里有 fixed.txt 就是设着；再报临时密码的设置：temp.txt 里有 off 就是关着、
//         有 keep 就是用过以后不换，关着时报的密码是空的），然后等命令。
// 等命令时把收到的每条命令回显成一行日志，收到 stop 或管道关闭就退出。会答复的命令（答复的格式照 proto/host/host_main.c）：
//   password new：另报一个新密码（临时密码关着时是空的）；password fixed 密码 / password fixed-clear：报 fixed set=1 / set=0；
//   password temp on|off|keep|rotate：照真主机的 temp_set 报 temp on= keep=，开关变了另报一个密码。没带 --password 起的报 error=off、
//   工作目录里有 temp-store.txt 时报 error=store，这两种时设置不变；
//   devices：把 devices.txt 里的设备一台一行报出来，再报 devices n=台数；
//   trust [设备号] / revoke 设备号 / device-perm 设备号 权限：工作目录里有 trusted.txt / revoked.txt / device-perm.txt 就原样打印它
//   （测试用它安排答复，比如会话换了人时的 trusted ok=0 error=changed），没有就报 trusted ok=0 error=none（假主机没有会话）/
//   revoked ok=1 id=设备号前 8 位 / device-perm ok=1。
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>

namespace {

bool has(int argc, char **argv, const char *option) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], option) == 0) return true;
    return false;
}

std::string valueOf(int argc, char **argv, const char *option) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], option) == 0) return argv[i + 1];
    return {};
}

void line(const std::string &text) { std::cout << text << '\n' << std::flush; }

int print(const char *file) {
    std::ifstream input(file, std::ios::binary);
    std::cout << input.rdbuf() << std::flush;
    return 0;
}

// 把 file 里以 prefix 开头的行逐行打印出来，返回打印了几行
int printLines(const char *file, const std::string &prefix) {
    std::ifstream input(file);
    std::string text;
    int count = 0;
    while (std::getline(input, text)) {
        if (text.compare(0, prefix.size(), prefix) == 0) {
            line(text);
            ++count;
        }
    }
    return count;
}

// 测试安排了答复（工作目录里的 file）就照它说，没有就说 fallback
void reply(const char *file, const std::string &fallback) {
    if (printLines(file, "@ui ") == 0) {
        line(fallback);
    }
}

bool startsWith(const std::string &text, const char *prefix) { return text.rfind(prefix, 0) == 0; }

// 假主机的连接密码：带没带 --password 起的，临时密码开不开、用过以后换不换（真主机的 PR.pw、PR.temp_off、PR.temp_keep）
struct Passwords {
    bool on;
    bool temp;
    bool keep;
};

std::string tempLine(const Passwords &passwords) {
    return std::string("@ui temp on=") + (passwords.temp ? "1" : "0") + " keep=" + (passwords.keep ? "1" : "0");
}

// 真主机的 pw_new_code：临时密码关着时报的 code 是空的
std::string codeLine(const Passwords &passwords, const char *code) {
    return std::string("@ui password code=") + (passwords.temp ? code : "");
}

// password temp on|off|keep|rotate，照真主机的 temp_set：先认词，再看能不能改，改不了报原来的值和原因
void setTemp(Passwords &passwords, const std::string &what) {
    const bool onOff = what == "on" || what == "off";
    if (!onOff && what != "keep" && what != "rotate") {
        return;
    }
    if (!passwords.on || std::ifstream("temp-store.txt").good()) {
        line(tempLine(passwords) + (passwords.on ? " error=store" : " error=off"));
        return;
    }
    const bool was = passwords.temp;
    if (onOff) {
        passwords.temp = what == "on";
    } else {
        passwords.keep = what == "keep";
    }
    line(tempLine(passwords));
    if (passwords.temp != was) {
        line(codeLine(passwords, "13572468"));
    }
}

// 客户端也在这里等命令：它不会收到密码的命令，passwords 给个“没开”就行
int serve(Passwords passwords = {false, true, false}) {
    constexpr std::size_t kShortId = 8;          // 真主机在 revoked 里只报设备号的前 8 位
    const std::string revoke = "revoke ";
    const std::string temp = "password temp ";
    std::string command;
    while (std::getline(std::cin, command) && command != "stop") {
        line("command " + command);
        if (command == "password new" && passwords.on) {
            line(codeLine(passwords, "87654321"));
        } else if (startsWith(command, temp.c_str())) {
            setTemp(passwords, command.substr(temp.size()));
        } else if (startsWith(command, "password fixed ")) {
            line("@ui fixed set=1");
        } else if (command == "password fixed-clear") {
            line("@ui fixed set=0");
        } else if (command == "devices") {
            line("@ui devices n=" + std::to_string(printLines("devices.txt", "@ui device ")));
        } else if (command == "trust" || startsWith(command, "trust ")) {
            reply("trusted.txt", "@ui trusted ok=0 error=none");
        } else if (startsWith(command, revoke.c_str())) {
            reply("revoked.txt", "@ui revoked ok=1 id=" + command.substr(revoke.size(), kShortId));
        } else if (startsWith(command, "device-perm ")) {
            reply("device-perm.txt", "@ui device-perm ok=1");
        }
    }
    return 0;
}

// 按号码连的客户端，见文件头
int connectByNumber(int argc, char **argv) {
    const bool typed = valueOf(argc, argv, "--password") == "-";
    std::string password;
    if (typed) {
        std::getline(std::cin, password);
        std::ofstream("stdin.txt", std::ios::app) << password << '\n';
    }
    std::string known;
    std::ifstream("known.txt") >> known;
    if (known == "broken") {                     // 核心自己出了错（真客户端取不到数据目录时就是这样）：什么都不报，以 1 退出
        return 1;
    }
    if (!typed && known.empty()) {
        reply("presence.txt", "@ui error=30 text=Enter\\sthe\\sother\\scomputer's\\sconnection\\spassword");
        return 3;
    }
    line("@ui phase=lookup path=direct secure=1 reconnects=0");
    if (known == "offline") {
        line("@ui error=26 text=The\\sother\\scomputer\\sis\\soffline,\\sor\\sthe\\snumber\\sis\\swrong");
        return 3;
    }
    if (known == "revoked" && !typed) {
        line("@ui error=29 text=The\\sother\\scomputer\\sno\\slonger\\strusts\\sthis\\sdevice:\\senter\\sthe\\spassword");
        return 3;
    }
    std::string expected;
    std::getline(std::ifstream("password.txt"), expected);
    if (known != "trusted" && !expected.empty() && password != expected) {
        reply("wrong.txt", "@ui error=20 text=wrong\\spassword");
        return 3;
    }
    line("@ui phase=stream path=relay secure=1 reconnects=0");
    return serve();
}

// 把这次调用的参数作为一行追加进 calls.txt。界面常常同时拉起两个假核心（起客户端的同时去问“最近”的在线状态），
// 而 MSVC 的追加写是“先移到文件尾、再写”，不是一步：两个进程同时写，后写的会盖掉先写的那一行的开头。
// 所以先占一个锁目录（建目录是原子的）再写；占不到（上一个进程没来得及删就没了）等够了照写，顺手把它清掉
void recordCall(int argc, char **argv) {
    constexpr int kLockAttempts = 400;
    constexpr std::chrono::milliseconds kLockWait{5};
    std::string call;
    for (int i = 1; i < argc; ++i) {
        call += i > 1 ? " " : "";
        call += argv[i];
    }
    std::error_code ignored;
    for (int attempt = 0; attempt < kLockAttempts && !std::filesystem::create_directory("calls.lock", ignored); ++attempt) {
        std::this_thread::sleep_for(kLockWait);
    }
    std::ofstream("calls.txt", std::ios::app) << call << '\n';
    std::filesystem::remove("calls.lock", ignored);
}

int run(int argc, char **argv) {
    recordCall(argc, argv);
    if (has(argc, argv, "--devices")) return print("devices.txt");
    if (has(argc, argv, "--netdiag")) return print("netdiag.txt");
    if (has(argc, argv, "--probe")) {
        return print("probe.txt");
    }
    if (has(argc, argv, "--device-perm") || has(argc, argv, "--revoke")) return 0;
    // 按地址连、按号码认（--host … --by-number）：真客户端拿到号码以后同按号码连，这里不分
    if (has(argc, argv, "--host") && has(argc, argv, "--by-number")) {
        return connectByNumber(argc, argv);
    }
    if (has(argc, argv, "--host")) {
        std::string expected;
        std::ifstream("totp.txt") >> expected;
        line("@ui phase=handshake path=direct secure=1 reconnects=0");
        if (!expected.empty() && valueOf(argc, argv, "--totp") != expected) {
            line("@ui error=5 text=totp\\srequired");
            return 3;
        }
        line("@ui phase=stream path=direct secure=1 reconnects=0");
        return serve();
    }
    if (has(argc, argv, "--connect")) {
        return connectByNumber(argc, argv);
    }
    if (valueOf(argc, argv, "--capture") == "idd") {
        line("@ui need driver=vdisplay");
        return 1;
    }
    if (std::ifstream("noscreen.txt").good()) {
        line("@ui need screen=1");
        return 1;
    }
    line("@ui phase=listening");
    if (has(argc, argv, "--number")) {
        line("@ui number id=123456789");
    }
    Passwords passwords{has(argc, argv, "--password"), true, false};
    if (passwords.on) {
        // 真主机启动时的顺序：固定密码设没设、临时密码的设置（存在主机上，重启照旧）、这次的临时密码
        std::string words;
        std::getline(std::ifstream("temp.txt"), words);
        passwords.temp = words.find("off") == std::string::npos;
        passwords.keep = words.find("keep") != std::string::npos;
        line(std::ifstream("fixed.txt").good() ? "@ui fixed set=1" : "@ui fixed set=0");
        line(tempLine(passwords));
        line(codeLine(passwords, "12345678"));
    }
    if (has(argc, argv, "--pair")) line("@ui pair code=135792 ttl=600 temp=0");
    if (has(argc, argv, "--pair-temp")) line("@ui pair code=246801 ttl=600 temp=1");
    return serve(passwords);
}

} // namespace

int main(int argc, char **argv) {
    try {
        return run(argc, argv);
    } catch (...) {
        return 70;
    }
}
