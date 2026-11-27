#pragma once

#include <QSettings>
#include <QStringList>
#include <optional>

namespace ashdesk {

struct Endpoint {
    QString host;
    int port = 44100;
    QString address() const;
};

struct DesktopOptions {
    int theme = 0;
    int material = 2;
    int authentication = 0;
    QString keyFile;
    int port = 44100;
    int fps = 60;
    int bitrate = 30;
    bool audio = true;
    bool microphone = false;
    bool clipboard = true;
    bool files = true;   /* 文件复制粘贴默认开（用户实测默认关导致主机→客户端粘贴无反应） */
    bool viewOnly = false;
    bool metered = false;
    bool hostControl = false;
    // 主机自己插一块虚拟屏来采集（--capture idd），要装虚拟屏驱动
    bool virtualDisplay = false;
    int language = 0;
    // 主机二次验证：0 关、1 配对时、2 配对和每次会话
    int totp = 0;
    // 信令服务器（主机:端口）：号码连接和网络诊断都用它，空 = 内置的那一台（signalServer）
    QString rendezvous;
    // 号码连接（号码连接设计 N3）：启动主机时向信令服务器登记本机号码，并开连接密码
    bool numberHost = true;
    // 有人来连时：0 凭据（连接密码、固定密码、已配对的设备、共享密钥）对了就直接连，不在本机问（默认，用户 11-26 定的）；
    // 1 电脑前有人才要在本机点同意、没人直接连；2 每次都要点同意
    int confirm = 0;
    // 端口转发，空 / 0 = 不开：主机肯去连的目标白名单（主机:端口[,…]）；
    // 客户端的转发规则（本地端口=主机:端口[,…]）和 SOCKS5 入口端口。写法由核心校验
    QString forwardAllow;
    QString forward;
    int socks5 = 0;

    static DesktopOptions load(const QSettings &settings);
    void save(QSettings &settings) const;
};

// 主机带着哪种配对窗口启动：Device 配对新设备，Temporary 临时口令（配上的设备只管一次会话）
enum class Pairing { None, Device, Temporary };

bool parseEndpoint(const QString &text, int defaultPort, Endpoint &endpoint, QString &error);
// totp：主机要二次验证时用户输入的验证码，空 = 不带。options.language 定下的界面语言不是中文时带 --lang en（判定同 useLanguage）
QStringList clientArguments(const Endpoint &endpoint, const DesktopOptions &options, const QString &totp = {});

// 主机启动参数还要知道的两件事：数据目录里有没有存过共享密钥、号码存在哪个文件
struct HostStore {
    bool storedKey = true;
    QString numberFile;
};
// 号码连接开着时带 --rendezvous、--number、--password（配对窗口和连接密码只能开一个，带配对窗口时不带 --password）；
// 这时没存过共享密钥也能启动（不带 --psk-stored），因为对方靠密码或已配对的设备密钥进来
QStringList hostArguments(const DesktopOptions &options, Pairing pairing = Pairing::None, const HostStore &store = {});
// 装成系统服务时服务里那个主机的参数：同 hostArguments，号码文件和日志放在 directory（SYSTEM 用的 ProgramData\AshDesk），
// --ui-control 由服务自己加。服务里的主机是 SYSTEM，读不到这个账户加密存的共享密钥和二次验证密钥：
// 设置要用它们（不开号码连接又用存着的共享密钥、开了二次验证）时没法装，返回 nullopt
std::optional<QStringList> serviceHostArguments(const DesktopOptions &options, const QString &directory);

// ---------------- 号码连接（号码连接设计 N3） ----------------
// 设置里填的信令服务器；没填就是内置的那一台
QString signalServer(const DesktopOptions &options);
// 认用户输入的号码：数字之间可以有空格和连字符，全角数字当半角。返回 1 = 是号码（number 收到 9 位半角数字）；
// 0 = 含别的字符，按地址处理；-1 = 只有数字和分隔符，但不是 9 位或以 0 开头
int parseNumber(const QString &text, QString &number);
// 123456789 → “123 456 789”；12345678 → “1234 5678”（连接密码）
QString groupDigits(const QString &digits);
// 固定密码合不合规则（同核心的 adp_password_kind，proto/common/adp_dev.h）：8–64 个可打印 ASCII 字符（0x20–0x7E），
// 不以空格开头或结尾，至少有一个不是数字、空格、连字符
bool validFixedPassword(const QString &text);
// 认对方的连接密码（号码连接设计 §4.3），返回规整后的密码，两种都不是返回空：
//   临时密码：8 位数字，中间可以有空格、连字符，全角数字当半角 → 8 位半角数字；
//   固定密码：只去掉首尾空白，其余原样
QString parsePassword(const QString &text);
// 按号码连的对象：号码，和（已经知道的话）对方的连接密码。密码是 parsePassword 规整过的
struct NumberTarget {
    QString number;
    QString password;
};
// 号码和连接密码写在一起（对方点一下本机号码复制出来的就是这样）：一共 17 位数字，前 9 位是号码、后 8 位是密码。
// 数字之间可以夹着空格、标点和文字，全角数字当半角；带着地址里才有的 . : @ / 就不是。是的话返回 true
bool parseNumberTarget(const QString &text, NumberTarget &target);
// 按号码连：--connect 号码@信令服务器，不带地址和共享密钥；别的选项同 clientArguments。
// 有密码时再带 --password -：密码本身不进命令行，核心起来以后从 stdin 读一行（界面写给它）。
// 没密码时不带 --password：对方信任过这台设备才连得上，否则核心报 error=30 马上退出
QStringList numberArguments(const NumberTarget &target, const DesktopOptions &options);
// 按地址连、按号码认（docs/CrossDesk借鉴设计.md CD10）：没存过共享密钥、也没选密钥文件时按地址连走这条。
// --host 地址 --port 端口 --by-number：核心从对方的局域网发现应答里拿号码，对方信任过这台设备就直接连，没有就报要密码（error=30）；
// 有密码时再带 --password -（同 numberArguments）
QStringList directArguments(const Endpoint &endpoint, bool password, const DesktopOptions &options);
// 身份的加密导出、导入（docs/CrossDesk借鉴设计.md CD13），交给客户端核心：--identity-export / --identity-import 文件
// --identity-dir 数据目录 --ui-control；口令不进命令行，核心起来以后从 stdin 读一行
QStringList identityArguments(bool out, const QString &file, const QString &dataDirectory);

// 界面配置的格式版本：存储结构变化时加一，并在 prepareSettings 里迁移旧版本
// 版本 2：最近连接从一串地址改为带别名、分组的数组
constexpr int kSettingsVersion = 2;

// 打开界面配置之前调用：读不懂（格式错误、版本比本程序新或不是数字）的文件改名为 <文件>.bad-<时间> 留档，
// 随后从默认值开始；能读的旧版本迁移后写上当前版本号。返回需要告诉用户的话，正常时为空
QString prepareSettings(const QString &fileName);

struct RecentConnection {
    QString address;
    QString alias;
    QString group;
};

// 最近连接兼作地址簿：未命名、未分组的记录最多留 kRecentLimit 条，起过别名或分过组的不会被新连接挤掉
constexpr qsizetype kRecentLimit = 12;
QList<RecentConnection> loadRecent(QSettings &settings);
void saveRecent(QSettings &settings, const QList<RecentConnection> &recent);
void rememberRecent(QList<RecentConnection> &recent, const QString &address);

// 按 DesktopOptions::language（0 跟随系统、1 中文、2 English）装界面和 Qt 自带对话框的翻译，可重复调用
void useLanguage(int language);

} // namespace ashdesk
