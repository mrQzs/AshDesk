#pragma once

#include "core_process.h"
#include "desktop_options.h"
#include <QDateTime>
#include <QHash>
#include <QIcon>
#include <QMainWindow>
#include <QTimer>
#include <functional>
#include <optional>

class QAbstractButton;
class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QSystemTrayIcon;
class QTableWidget;
class QVBoxLayout;

namespace ashdesk {

class GlassPanel;
class GlassSurface;
class IconView;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    // dataDirectory：核心的数据目录（设备表、二次验证密钥；界面在它的 logs 子目录里放核心日志），默认和核心的 adp_store_path 一致
    // （Windows %LOCALAPPDATA%\AshDesk、macOS ~/Library/Application Support/AshDesk）；coreDirectory 默认见 main_window.cpp 的 defaultCoreDirectory
    explicit MainWindow(QSettings &settings, const QString &coreDirectory = {}, const QString &dataDirectory = {},
                        QWidget *parent = nullptr);
    void message(const QString &text);
    // 程序一打开就调：号码连接开着时让本机上线，连接页马上有号码和密码（RustDesk、ToDesk 都是打开就有）。
    // 主机归系统服务管时接上服务的那一个，不另起；关了“允许用本机号码连接”就什么都不做
    void goOnline();
    // goOnline 之后调：从安装包或绿色包第一次打开、系统服务还没装时问一次要不要装（关掉远程桌面以后、锁屏、登录界面、
    // 重启后没人登录都能连，靠的是它）。只问一次，以后在设置 → 安全 → 系统服务里装卸
    void offerService();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QWidget *buildHome();
    QWidget *buildSession();
    QWidget *buildHost();
    QWidget *buildDevices();
    QWidget *buildSettings();
    void buildTray();
    void restoreFromTray();
    void bindSettings();
    void saveOptions();
    void applyTheme();
    void showPage(int index);
    void connectToAddress(const QString &address, const QString &totp = {});
    // 按号码连（号码连接设计 N3 / N4）。target 里没有密码：先不带密码试一次，对方信任过这台设备就直接连上；
    // 核心说要密码（error=30）或对方不再信任这台设备（29），clientFinished 里问到密码再带着它来一次
    void connectToNumber(const NumberTarget &target);
    void connectDirect(const Endpoint &endpoint, const QString &password);
    // 问对方的连接密码；点了取消返回空。again：密码框里带上核心给的原因（密码不对、对方换了密码、对方不再信任这台设备）
    QString askPassword(const QString &title, bool again);
    void launchClient(const QStringList &arguments);
    // 把这次连的地址记进“最近”
    void rememberPending();
    void retryWithTotp();
    // “最近”里的一条拿去问在线状态时的写法：号码要带上信令服务器
    QString probeAddress(const QString &address) const;
    // 连接页“本机”那一行：主机报了号码就显示号码和连接密码，否则显示机器名和地址
    void showLocal();
    void startHost(Pairing pairing);
    bool authenticationReady();
    bool totpReady();
    QString coreProgram(bool host) const;
    QString coreLog(const QString &name) const;
    void updateProcesses();
    void showStatus();
    void clientMessage(const CoreMessage &message);
    void hostMessage(const CoreMessage &message);
    void clientFinished(int code);
    void hostFinished();
    void clearHostSession();
    void offerDriver(const QString &driver);
    void installDriver(const QString &driver);
    void driverInstalled(const QString &driver, int code);
    void installService();
    void uninstallService();
    void switchService(const QString &commandLine);
    void serviceChanged(bool installing, int code);
    void showService();
    void listSessionPermissions();
    // “信任这台设备”（号码连接设计 §4.5）：确认以后让主机把正连着的、用密码连进来的设备转成长期设备。
    // 发的是弹确认框那一刻那台的设备号（trust 设备号）：确认框开着时会话换了人，主机不会信任新来的那台
    void trustPeer();
    void showTrusted(const CoreMessage &trusted);
    // 设置 → 安全 → 固定密码那一行：设没设、几个控件能不能用
    void showFixed();
    void fixedReported(const CoreMessage &fixed);
    // 设置 → 安全 → 临时密码那两个开关：开不开、用过以后换不换，跟着主机报的 @ui temp 走
    void showTemp();
    void tempReported(const CoreMessage &temp);
    // 临时密码关着、也没设固定密码时的一句提醒（别人只能用信任过的设备连进来）；不是这样时为空
    QString passwordsOffNotice() const;
    void showPairing(const CoreMessage &pairing);
    void showPairingTime();
    // 跑一次查完 / 改完就退出的主机核心命令：它报来的每行状态交给 report（可空），退出码交给 done（没跑起来是 -1）
    void runHostTool(const QStringList &arguments, const std::function<void(const CoreMessage &)> &report,
                     const std::function<void(int)> &done);
    // input 非空时核心一起来就把它当一行写进 stdin（口令这类不进命令行的）
    void runCoreTool(const QString &program, const QStringList &arguments, const std::function<void(const CoreMessage &)> &report,
                     const std::function<void(int)> &done, const QString &input = {});
    // 身份的加密导出、导入（docs/CrossDesk借鉴设计.md CD13）：选文件、问口令，交给客户端核心做
    void transferIdentity(bool out);
    QString askPassphrase(bool twice);
    // “最近”列表每个地址的在线状态，存在条目的 kRecentStatusRole 里；换主题重画图标时照它来
    enum class RecentStatus { Unknown, Online, Busy, Offline };
    void probeRecent();
    void showRecentStatus(const CoreMessage &probe);
    const QIcon &recentIcon(RecentStatus status) const;
    // 已配对设备：主机在跑（自己起的或接着系统服务的）时设备表在它手里，列表、吊销、改权限都经控制管道问它
    // （devices / revoke / device-perm）；没在跑时另起一个查完 / 改完就退出的主机核心
    void refreshPaired();
    void showPaired(const QList<CoreMessage> &devices);
    QWidget *pairedRow(const CoreMessage &device);
    // command：{"revoke", 设备号} 或 {"device-perm", 设备号, 权限}
    void changePaired(const QStringList &command, const QString &succeeded, const QString &failed);
    // 在跑的主机对 revoke / device-perm 的答复
    void pairedChanged(const CoreMessage &result);
    void runNetdiag();
    void showDiagItem(const QString &item, bool ok, const QString &value);
    void refreshDevices();
    void addDevice();
    void removeDevice();
    QPushButton *iconButton(const QString &text, const QString &name, const char *role, const QString &symbol);
    void refreshRecent();
    void editRecent(bool regroup);
    void exportDiagnostics();
    void setupTotp();
    void showTotp(const QString &uri);

    QSettings &settings_;
    DesktopOptions options_;
    QString coreDirectory_;
    QString dataDirectory_;
    QString pendingAddress_;
    // 这次是按号码连的：号码（9 位数字）；按地址连时为空
    QString pendingNumber_;
    // 这次是按地址、按号码认连的（没存过共享密钥，CD10）：地址；别的连法时 host 为空
    Endpoint pendingDirect_;
    // 这次按号码连要给客户端核心的密码：核心起来以后写进它的 stdin（launched），随即清空
    QString pendingPassword_;
    // 按号码连、没带密码的那次试连：核心报了阶段（对方认得这台设备）才算真的在连，那时才切到会话页、记进“最近”
    bool numberTrial_ = false;
    // 主机核心报来的本机号码、当前的连接密码；主机停了就清空
    QString hostNumber_, hostPassword_;
    // 主机报号码还没在信令服务器上登记上：别人暂时找不到这台电脑
    bool hostOffline_ = false;
    // 主机报的固定密码设没设（@ui fixed）；没报过（主机没在跑、没开连接密码）时为空
    std::optional<bool> hostFixed_;
    // 界面刚发了设置 / 清除固定密码，下一条 @ui fixed 是它的结果
    bool fixedAsked_ = false;
    // 主机报的临时密码开不开（@ui temp on=），没报过时为空；用过以后换不换（keep=1 不换）
    std::optional<bool> hostTempOn_;
    bool hostTempKeep_ = false;
    // 界面刚发了改临时密码设置的命令，下一条 @ui temp 是它的结果
    bool tempAsked_ = false;
    // 正连着的、可以点“信任这台设备”的临时设备的设备号（phase 行 trust=1 时的 dev=）；空 = 现在没有可以信任的
    QString hostTrustDevice_;
    // 在跑的主机正在答复 devices：攒到 @ui devices 那一行再一起画
    QList<CoreMessage> pairedListing_;
    // 发给在跑的主机的 revoke / device-perm 成了、没成各说什么
    QString pairedSucceeded_;
    QString pairedFailed_;
    CoreProcess client_, host_;
    // 核心最近一次报来的状态（@ui 行）；对应的核心退出或重新启动时清空
    QString clientPhase_, clientPath_, clientError_;
    int clientErrorCode_ = 0;
    QString hostPhase_, hostPeer_, hostPeerName_;
    // 会话开始时设备有的权限，和此刻还有效的（主机这边会话中收回的不在其中）
    QStringList hostGranted_, hostPerms_;
    // 主机核心正在停止、停下后要带着这种配对窗口重新启动
    std::optional<Pairing> hostRestart_;
    // 最近一次启动主机核心带的配对窗口：补装虚拟屏驱动后照原样再启动
    Pairing hostPairing_ = Pairing::None;
    // 这次主机运行里已经问过要不要装的驱动；这次界面运行里装成功过的驱动（用法见 offerDriver）
    QStringList offeredDrivers_, installedDrivers_;
    QDateTime pairingDeadline_;
    QTimer pairingTimer_;
    // 装了系统服务时主机归服务管（A2）：host_ 接到服务的管道上。没接上时每 2 s 试一次；
    // serviceAgent_ 是服务报的代理进程号，"0" = 服务正在（重新）拉起它
    QString serviceName_, serviceAgent_;
    QTimer serviceTimer_;
    // 装卸系统服务的提权脚本正在跑；装之前先停自己起的主机（占着端口和号码），停下后接着装；
    // 卸掉以后要等服务那个主机断开再让本机上线
    bool serviceBusy_ = false, serviceAfterStop_ = false, onlineAfterService_ = false;
    QPushButton *service_ = nullptr;
    bool netdiagRunning_ = false;
    bool closing_ = false;
    // 主机核心在跑时关窗口是收进托盘；托盘菜单的“退出”才走确认退出
    bool inTray_ = false;
    bool exiting_ = false;
    bool trayControlled_ = false;
    QSystemTrayIcon *tray_ = nullptr;
    QAction *trayDisconnect_ = nullptr;
    QAction *trayTrust_ = nullptr;
    QMenu *trayPermissions_ = nullptr;
    int appearance_ = -1;
    GlassSurface *surface_ = nullptr;
    GlassPanel *status_ = nullptr;
    IconView *logo_ = nullptr;
    IconView *searchIcon_ = nullptr;
    QIcon recentIcon_, recentOnlineIcon_, recentBusyIcon_;
    bool probing_ = false;
    QList<QPair<QAbstractButton *, QString>> icons_;
    QList<QPushButton *> navigation_;
    QStackedWidget *pages_ = nullptr;
    QLineEdit *address_ = nullptr;
    QLineEdit *keyFile_ = nullptr;
    QLineEdit *rendezvous_ = nullptr;
    QLineEdit *forwardAllow_ = nullptr;
    QLineEdit *forward_ = nullptr;
    QLineEdit *fixedPassword_ = nullptr;
    QComboBox *theme_ = nullptr;
    QComboBox *material_ = nullptr;
    QComboBox *language_ = nullptr;
    QComboBox *authentication_ = nullptr;
    QComboBox *totp_ = nullptr;
    QComboBox *confirm_ = nullptr;
    QSpinBox *port_ = nullptr;
    QSpinBox *fps_ = nullptr;
    QSpinBox *bitrate_ = nullptr;
    QSpinBox *socks5_ = nullptr;
    QCheckBox *audio_ = nullptr;
    QCheckBox *microphone_ = nullptr;
    QCheckBox *clipboard_ = nullptr;
    QCheckBox *files_ = nullptr;
    QCheckBox *viewOnly_ = nullptr;
    QCheckBox *metered_ = nullptr;
    QCheckBox *hostControl_ = nullptr;
    QCheckBox *numberHost_ = nullptr;
    QCheckBox *virtualDisplay_ = nullptr;
    QCheckBox *tempPassword_ = nullptr;
    QCheckBox *tempKeep_ = nullptr;
    QLabel *message_ = nullptr;
    QLabel *localName_ = nullptr;
    QLabel *localAddress_ = nullptr;
    QLabel *localPassword_ = nullptr;
    QWidget *localNumber_ = nullptr;
    QPushButton *copyNumber_ = nullptr;
    QPushButton *showPassword_ = nullptr;
    QPushButton *newPassword_ = nullptr;
    QLabel *clientStatus_ = nullptr;
    QLabel *clientStats_ = nullptr;
    QLabel *hostStatus_ = nullptr;
    QLabel *hostDetail_ = nullptr;
    QLabel *hostStats_ = nullptr;
    QLabel *fixedState_ = nullptr;
    QLabel *sessionAddress_ = nullptr;
    QLabel *hostBanner_ = nullptr;
    QLabel *pairingTitle_ = nullptr;
    QLabel *pairingCode_ = nullptr;
    QLabel *pairingTime_ = nullptr;
    QLabel *netdiagVerdict_ = nullptr;
    QHash<QString, QLabel *> netdiagValues_;
    QWidget *pairing_ = nullptr;
    QWidget *pairedSection_ = nullptr;
    QWidget *netdiagResults_ = nullptr;
    QVBoxLayout *paired_ = nullptr;
    QPushButton *connect_ = nullptr;
    QPushButton *startHost_ = nullptr;
    QPushButton *stopClient_ = nullptr;
    QPushButton *stopHost_ = nullptr;
    QPushButton *pairDevice_ = nullptr;
    QPushButton *pairTemporary_ = nullptr;
    QPushButton *trustDevice_ = nullptr;
    QPushButton *showFixed_ = nullptr;
    QPushButton *setFixed_ = nullptr;
    QPushButton *clearFixed_ = nullptr;
    QPushButton *connectDevice_ = nullptr;
    QPushButton *removeDevice_ = nullptr;
    QPushButton *totpSetup_ = nullptr;
    QPushButton *netdiag_ = nullptr;
    QListWidget *recent_ = nullptr;
    QTableWidget *devices_ = nullptr;
    QPlainTextEdit *clientLog_ = nullptr;
    QPlainTextEdit *hostLog_ = nullptr;
};

} // namespace ashdesk
