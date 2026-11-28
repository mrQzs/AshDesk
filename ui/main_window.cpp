#include "main_window.h"

#include "ashdesk_version.h"
#include "controls.h"
#include "diagnostics.h"
#include "driver_install.h"
#include "firewall.h"
#include "liquid_glass.h"

#include <QApplication>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif
#include <algorithm>
#include <memory>
#include <utility>

namespace ashdesk {
namespace {

QLabel *label(const QString &text, const QString &name = {}) {
    auto *widget = new QLabel(text);
    widget->setTextFormat(Qt::PlainText);
    widget->setObjectName(name);
    widget->setWordWrap(true);
    return widget;
}

QPushButton *button(const QString &text, const QString &name, const char *role = "") {
    auto *widget = new QPushButton(text);
    widget->setObjectName(name);
    widget->setProperty("role", role);
    // Keyboard-only focus keeps the focus ring for keyboard users, like CSS :focus-visible.
    widget->setFocusPolicy(Qt::TabFocus);
    return widget;
}

// QSS :focus also matches startup and page-switch focus; Qt only flags the window once the keyboard moved focus.
void markFocusVisible(QWidget *widget, bool visible) {
    auto *target = qobject_cast<QPushButton *>(widget);
    if (!target || target->property("focusVisible").toBool() == visible) return;
    target->setProperty("focusVisible", visible);
    target->style()->unpolish(target);
    target->style()->polish(target);
}

// Glass panels reserve their shadow in the contents margins, so the layout inside starts at the glass edge.
template<typename Layout>
Layout *panel(QBoxLayout *parent, int radius, const QMargins &padding, int stretch = 0) {
    auto *glass = new GlassPanel(GlassRole::Action, radius);
    parent->addWidget(glass, stretch);
    auto *layout = new Layout(glass);
    layout->setContentsMargins(padding);
    return layout;
}

QVBoxLayout *pageLayout(QWidget *page, const QString &title) {
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(2, 22, 8, 4);
    layout->setSpacing(0);
    if (!title.isEmpty()) layout->addWidget(label(title, "pageTitle"));
    return layout;
}

// Item views and scroll areas fill their viewport by default, which would hide the glass behind them.
void seeThrough(QAbstractScrollArea *area) {
    area->setFrameShape(QFrame::NoFrame);
    area->viewport()->setAutoFillBackground(false);
}

QPlainTextEdit *diagnostics(const QString &name) {
    auto *widget = new QPlainTextEdit;
    widget->setObjectName(name);
    widget->setAccessibleName(QCoreApplication::translate("ashdesk", "核心诊断"));
    widget->setReadOnly(true);
    widget->setMaximumBlockCount(1000);
    widget->setMinimumHeight(120);
    seeThrough(widget);
    return widget;
}

void appendOutput(QPlainTextEdit *log, const QString &text) {
    const bool atEnd = log->textCursor().atEnd();
    QTextCursor cursor(log->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    if (atEnd) {
        log->setTextCursor(cursor);
        log->ensureCursorVisible();
    }
}

// accept：肯定按钮上的字，空 = “确定”
bool confirm(QWidget *parent, const QString &title, const QString &question, const QString &accept = {}) {
    QMessageBox dialog(QMessageBox::Question, title, question, QMessageBox::Yes | QMessageBox::Cancel, parent);
    dialog.setTextFormat(Qt::PlainText);
    dialog.button(QMessageBox::Yes)->setText(accept.isEmpty() ? QCoreApplication::translate("ashdesk", "确定") : accept);
    dialog.button(QMessageBox::Cancel)->setText(QCoreApplication::translate("ashdesk", "取消"));
    dialog.setDefaultButton(QMessageBox::Cancel);
    return dialog.exec() == QMessageBox::Yes;
}

QString css(const QColor &color) {
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

QColor rgba(int red, int green, int blue, qreal alpha) {
    QColor color(red, green, blue);
    color.setAlphaF(float(alpha));
    return color;
}

struct Permission {
    const char *key;
    const char *label;
};

// 名字和顺序同 proto/common/adp_dev.c 的 perm_names
constexpr Permission kPermissions[] = {
    {"control", QT_TRANSLATE_NOOP("ashdesk", "控制")},     {"files", QT_TRANSLATE_NOOP("ashdesk", "传文件")},
    {"clipboard", QT_TRANSLATE_NOOP("ashdesk", "剪贴板")}, {"audio", QT_TRANSLATE_NOOP("ashdesk", "声音")},
    {"mic", QT_TRANSLATE_NOOP("ashdesk", "麦克风")},       {"curtain", QT_TRANSLATE_NOOP("ashdesk", "隐私幕布")},
    {"forward", QT_TRANSLATE_NOOP("ashdesk", "端口转发")}, {"restart", QT_TRANSLATE_NOOP("ashdesk", "重启")}};

struct DiagItem {
    const char *key;
    const char *label;
};

// 网络诊断逐项显示的结果：前五项是 adrd_host --netdiag 报的 item，firewall 由界面自己查
constexpr DiagItem kDiagItems[] = {
    {"nat", QT_TRANSLATE_NOOP("ashdesk", "NAT 类型")},      {"udp", QT_TRANSLATE_NOOP("ashdesk", "UDP 出站")},
    {"tcp", QT_TRANSLATE_NOOP("ashdesk", "TCP 出站")},      {"relay_rtt", QT_TRANSLATE_NOOP("ashdesk", "中转往返")},
    {"portmap", QT_TRANSLATE_NOOP("ashdesk", "端口映射")}, {"firewall", QT_TRANSLATE_NOOP("ashdesk", "防火墙规则")}};

// 核心报的 "control,files" 拆成名字；"view"（只看画面）是空表
QStringList permissions(const QString &list) {
    return list == QLatin1String("view") ? QStringList() : list.split(',', Qt::SkipEmptyParts);
}

QString permissionSummary(const QStringList &keys) {
    QStringList labels;
    for (const Permission &permission : kPermissions)
        if (keys.contains(QLatin1String(permission.key)))
            labels << QCoreApplication::translate("ashdesk", permission.label);
    return labels.isEmpty() ? QCoreApplication::translate("ashdesk", "只看画面")
                            : labels.join(QCoreApplication::translate("ashdesk", "、"));
}

// 核心还没报阶段，或报了界面不认识的阶段时返回空串
QString clientPhaseText(const QString &phase, const QString &path) {
    if (phase == QLatin1String("lookup")) return QCoreApplication::translate("ashdesk", "正在查找主机");
    if (phase == QLatin1String("handshake")) return QCoreApplication::translate("ashdesk", "正在握手");
    if (phase == QLatin1String("negotiate")) return QCoreApplication::translate("ashdesk", "正在协商");
    if (phase == QLatin1String("configure")) return QCoreApplication::translate("ashdesk", "等待首帧");
    if (phase != QLatin1String("stream")) return {};
    return QCoreApplication::translate("ashdesk", "已连接 · %1")
        .arg(path == QLatin1String("relay") ? QCoreApplication::translate("ashdesk", "中转")
             : path == QLatin1String("tcp") ? QCoreApplication::translate("ashdesk", "TCP 中转")
                                            : QCoreApplication::translate("ashdesk", "直连"));
}

// 每秒一行的统计。encoder= / codec= 是核心给的显示名（引擎的 docs/音视频管线架构.md §7），直接显示；
// 老核心只有 enc= 的编号（AD_ENC_*），照旧查表。cap 的数值是 AD_CAPK_*。未知的不显示
QString statsText(const CoreMessage &stats) {
    const QStringList encoders{{}, "NVENC", "x264", "OpenH264", "AE264"};
    const QStringList captures{{}, "DXGI", "GDI", "IDD", QCoreApplication::translate("ashdesk", "测试图样")};
    QStringList parts{QCoreApplication::translate("ashdesk", "%1 fps · %2 Mb/s · 往返 %3 ms · 丢包 %4%")
                          .arg(stats.value("fps"), stats.value("mbps"), stats.value("rtt_ms"), stats.value("loss"))};
    const QString encoder = stats.value("encoder");
    parts << (encoder.isEmpty() ? encoders.value(stats.value("enc").toInt()) : encoder) << stats.value("codec")
          << captures.value(stats.value("cap").toInt());
    parts.removeAll(QString());
    return parts.join(QStringLiteral(" · "));
}

constexpr int kRecentStatusRole = Qt::UserRole + 1;
constexpr int kProbeLimit = 16;               // adrd_client --probe 一次最多问这么多个地址
constexpr int kLocalNumberSpacing = 6;        // 连接页“本机”一行里号码、密码、按钮之间的间距
constexpr int kLocalPasswordGap = 8;          // 号码和密码之间再多留的一点
constexpr int kPasswordPromptWidth = 320;
constexpr int kPassphraseMin = 8;              // 身份导出的口令至少几个字节，同 proto/common/adp_identity.h 的 ADP_ID_PASS_MIN
constexpr int kButtonIcon = 18;               // 图标按钮里图标的边长
constexpr int kFixedPasswordWidth = 120;      // 设置 → 安全里固定密码输入框的最小宽度
constexpr int kDevicesPage = 3;
constexpr int kExitRejected = 3;              // 客户端核心的退出码：连接被明确拒绝，要人改了再连
// 客户端按号码连没连上的原因（proto/client/client_session.c 的 PWE_*）里，问到密码就能再连的几种
constexpr int kPasswordWrong = 20;            // 密码不对
constexpr int kPasswordReplaced = 22;         // 错太多次，对方已经换了密码
constexpr int kPasswordSpent = 25;            // 连上以后断了，密码只管一次
constexpr int kPasswordForgotten = 29;        // 对方不再信任这台设备：记住的密钥被吊销了
constexpr int kPasswordNeeded = 30;           // 没给密码，对方也没记住这台设备
constexpr int kPasswordFormat = 31;           // 密码的格式不对
constexpr int kPasswordKindOff = 32;          // 对方没开这一种密码（关了临时密码，或没设固定密码），另一种也许能用

// “最近”列表里在线的主机：图标右下角叠一个圆点（绿 = 在线，琥珀 = 正在被控），白边让它在深浅底上都分得清
QIcon dotted(const QIcon &base, const QSize &size, const QColor &color, qreal ratio) {
    constexpr qreal kDotShare = .42;          // 圆点直径占图标边长的比例
    constexpr qreal kRimShare = 1.0 / 16.0;   // 白边的宽度占边长的比例
    constexpr qreal kInset = .5;              // 离右下角留半个像素，白边不被裁掉
    QPixmap image = base.pixmap(size, ratio);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal side = size.width();
    const qreal dot = side * kDotShare;
    painter.setPen(QPen(Qt::white, side * kRimShare));
    painter.setBrush(color);
    painter.drawEllipse(QRectF(side - dot - kInset, size.height() - dot - kInset, dot, dot));
    return QIcon(image);
}

// 托盘图标：应用图标，被控时右下角叠一个琥珀色圆点（设计稿的 ctl 色），白边让它在深浅任务栏上都分得清
QIcon trayIcon(bool controlled) {
    QIcon icon;
    for (const int size : {16, 20, 24, 32}) {
        QPixmap image(QStringLiteral(":/ashdesk/app/ashdesk-%1.png").arg(size));
        if (controlled) {
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            const qreal dot = size * .44;
            painter.setPen(QPen(Qt::white, size / 16.0));
            painter.setBrush(QColor("#F5A524"));
            painter.drawEllipse(QRectF(size - dot - .5, size - dot - .5, dot, dot));
        }
        icon.addPixmap(image);
    }
    return icon;
}

constexpr char kStyleSheet[] = R"qss(
QMainWindow, QDialog, QMessageBox { background: @window; }
QWidget { color: @text; font-size: 14px; }
QScrollArea#pageScroll, QScrollArea#pageScroll > QWidget, QScrollArea#pageScroll > QWidget > QWidget,
QTabWidget > QStackedWidget, QTabWidget > QStackedWidget > QWidget { background: transparent; border: 0; }
QLabel { background: transparent; }
QLabel#muted, QLabel#clientStats, QLabel#hostDetail, QLabel#hostStats, QLabel#pairingTime, QLabel#fixedState { color: @muted; }
QLabel#pageTitle { font-size: 22px; font-weight: 600; padding: 0 0 8px 10px; }
QLabel#sectionTitle { font-size: 15px; font-weight: 600; padding: 0 0 2px 10px; }
QLabel#hostName { font-size: 18px; font-weight: 600; }
QLabel#tag, QLabel#pairingTitle { font-size: 12px; color: @muted; background: @hover; border-radius: 6px; padding: 2px 8px; }
QLabel#sessionAddress, QLabel#hostStatus, QLabel#pairedName, QLabel#netdiagVerdict { font-weight: 600; }
QLabel#pairingCode { font-size: 30px; font-weight: 600; }
QFrame#pairedRow { border: 0; border-bottom: 1px solid @line; }
QLabel#hostBanner { background: @warnFill; color: @warnText; border-radius: 12px; padding: 10px 14px; margin: 0 10px 10px 10px; }
QPushButton { background: @button; border: 1px solid @strong; border-radius: 10px; padding: 0 14px; min-height: 32px; font-weight: 600; }
QPushButton:hover { background: @buttonHover; }
QPushButton:disabled { color: @muted; }
QPushButton[role=primary] { background: @accent; color: #FFFFFF; border-color: @accent; }
QPushButton[role=primary]:hover { background: @accentHover; }
QPushButton[role=danger] { background: @danger; color: #FFFFFF; border-color: @danger; }
QPushButton[role=primary]:disabled, QPushButton[role=danger]:disabled { background: @button; color: @muted; border-color: @strong; }
QPushButton#connect { min-height: 38px; max-height: 38px; border-radius: 20px; padding: 0 22px; }
QPushButton[role=nav] { background: transparent; border: 0; border-radius: 14px; padding: 0; min-height: 44px; }
QPushButton[role=nav]:hover { background: @hover; }
QPushButton[role=nav]:checked { background: @selected; }
QPushButton#copyNumber, QPushButton#showPassword, QPushButton#newPassword, QPushButton#showFixed, QPushButton#showPasswordCode { background: transparent; border: 0; padding: 0 6px; }
QPushButton#copyNumber { font-size: 18px; }
QPushButton#copyNumber[offline=true] { color: @muted; }
QPushButton#copyNumber:hover, QPushButton#showPassword:hover, QPushButton#newPassword:hover, QPushButton#showFixed:hover,
QPushButton#showPasswordCode:hover { background: @hover; }
QPushButton#showPassword:checked, QPushButton#showFixed:checked, QPushButton#showPasswordCode:checked { background: @selected; }
QLabel#localPassword { font-size: 16px; font-weight: 600; }
QPushButton[focusVisible=true], QPushButton#copyNumber[focusVisible=true], QPushButton#showPassword[focusVisible=true],
QPushButton#newPassword[focusVisible=true], QPushButton#showFixed[focusVisible=true],
QPushButton#showPasswordCode[focusVisible=true] { border: 2px solid @ring; }
QLineEdit, QComboBox, QSpinBox { background: @field; border: 1px solid @strong; border-radius: 8px; padding: 0 9px; min-height: 30px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 2px solid @ring; padding: 0 8px; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: @muted; }
QSpinBox::up-button, QSpinBox::down-button { subcontrol-origin: border; width: 26px; border: 0; background: transparent; }
QSpinBox::up-button { subcontrol-position: top right; border-top-right-radius: 8px; }
QSpinBox::down-button { subcontrol-position: bottom right; border-bottom-right-radius: 8px; }
QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: @hover; }
QComboBox::drop-down { subcontrol-origin: border; subcontrol-position: center right; width: 28px; border: 0; }
QSpinBox::up-arrow { image: url(:/ashdesk/chevron-up.svg); width: 10px; height: 10px; }
QSpinBox::down-arrow, QComboBox::down-arrow { image: url(:/ashdesk/chevron-down.svg); width: 10px; height: 10px; }
QLineEdit#address, QLineEdit#address:focus { background: transparent; border: 0; padding: 0 4px; font-size: 17px; min-height: 40px; }
QComboBox QAbstractItemView { background: @field; border: 1px solid @strong; selection-background-color: @selected; selection-color: @text; }
QListWidget#recent, QTableWidget#devices { background: transparent; border: 0; }
QListWidget#recent::item { padding: 8px 6px; border-radius: 10px; }
QListWidget#recent::item:hover { background: @hover; }
QListWidget#recent::item:selected { background: @selected; color: @text; }
QListWidget#recent::item:disabled { background: transparent; }
QTableWidget#devices::item { padding: 0 8px; border-bottom: 1px solid @line; }
QTableWidget#devices::item:selected { background: @selected; color: @text; }
QHeaderView, QHeaderView::section, QTableCornerButton::section { background: transparent; color: @muted; border: 0; }
QHeaderView::section { padding: 6px 8px; font-weight: 600; border-bottom: 1px solid @line; }
QPlainTextEdit { background: transparent; border: 0; padding: 4px 6px; selection-background-color: @accent; selection-color: #FFFFFF; }
QTabWidget::pane { border: 0; background: transparent; }
QTabBar::tab { background: transparent; color: @muted; min-height: 30px; padding: 0 14px; margin: 2px 1px; border-radius: 9px; font-weight: 600; }
QTabBar::tab:hover { color: @text; }
QTabBar::tab:selected { background: @selected; color: @text; }
QToolTip { background: @field; color: @text; border: 1px solid @strong; padding: 4px 6px; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: @strong; border-radius: 3px; min-height: 24px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
)qss";

// 核心程序在哪：Windows 和界面同一个目录；macOS 上客户端是套在 AshDesk.app/Contents/Helpers 里的小 .app（docs/macOS客户端设计.md D1）
QString defaultCoreDirectory() {
#ifdef Q_OS_MACOS
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../Helpers/adrd_client.app/Contents/MacOS"));
#else
    return QCoreApplication::applicationDirPath();
#endif
}

// 要和核心的 adp_store_path 是同一个目录（密钥、设备表、二次验证、日志都在这里）：
// Windows 是 %LOCALAPPDATA%\AshDesk，macOS 是 ~/Library/Application Support/AshDesk，Linux 是 $XDG_DATA_HOME/AshDesk
QString defaultDataDirectory() {
#ifdef Q_OS_WIN
    return QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("AshDesk");
#else
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("AshDesk");
#endif
}

} // namespace

MainWindow::MainWindow(QSettings &settings, const QString &coreDirectory, const QString &dataDirectory, QWidget *parent)
    : QMainWindow(parent), settings_(settings), options_(DesktopOptions::load(settings)),
      coreDirectory_(coreDirectory.isEmpty() ? defaultCoreDirectory() : coreDirectory),
      dataDirectory_(dataDirectory.isEmpty() ? defaultDataDirectory() : dataDirectory),
      client_(this), host_(this) {
    setWindowTitle(tr("AshDesk · 极帧"));
    setObjectName("desktopWindow");
    client_.setObjectName("clientCore");
    host_.setObjectName("hostCore");
    resize(1120, 740);
    setMinimumSize(640, 480);
    surface_ = new GlassSurface;
    auto *body = new QHBoxLayout(surface_);
    body->setContentsMargins(0, 4, 0, 0);
    body->setSpacing(0);
    auto *rail = new GlassPanel(GlassRole::Navigation, 22);
    rail->setObjectName("navigation");
    rail->setFixedWidth(64 + rail->contentsMargins().left() + rail->contentsMargins().right());
    auto *navLayout = new QVBoxLayout(rail);
    navLayout->setContentsMargins(10, 12, 10, 12);
    navLayout->setSpacing(6);
    logo_ = new IconView(30);
    logo_->setFixedSize(44, 44);
    logo_->setAccessibleName(tr("AshDesk 极帧"));
    navLayout->addWidget(logo_);
    navLayout->addSpacing(8);
    auto *group = new QButtonGroup(this);
    const QStringList names{tr("连接"), tr("会话"), tr("被控端"), tr("设备"), tr("设置")};
    const QStringList symbols{"link", "monitor", "server", "devices", "settings"};
    for (int i = 0; i < names.size(); ++i) {
        auto *item = iconButton({}, QString("nav%1").arg(i), "nav", symbols[i]);
        item->setCheckable(true);
        item->setFixedSize(44, 44);
        item->setIconSize(QSize(22, 22));
        item->setToolTip(names[i]);
        item->setAccessibleName(names[i]);
        group->addButton(item, i);
        navLayout->addWidget(item);
        navigation_.append(item);
    }
    navLayout->addStretch();
    body->addWidget(rail);

    auto *column = new QVBoxLayout;
    column->setSpacing(0);
    pages_ = new QStackedWidget;
    pages_->setObjectName("pages");
    const QList<QWidget *> content{buildHome(), buildSession(), buildHost(), buildDevices(), buildSettings()};
    for (QWidget *page : content) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName("pageScroll");
        scroll->setWidgetResizable(true);
        scroll->setWidget(page);
        seeThrough(scroll);
        page->setAutoFillBackground(false);
        pages_->addWidget(scroll);
    }
    column->addWidget(pages_, 1);
    status_ = new GlassPanel(GlassRole::Action, 14);
    status_->setObjectName("status");
    auto *statusLayout = new QHBoxLayout(status_);
    statusLayout->setContentsMargins(14, 6, 14, 6);
    // 不另设读屏名：设了读屏读到的就只有那个名字，状态栏上这句话本身读不出来
    message_ = label({}, "message");
    statusLayout->addWidget(message_);
    status_->hide();
    auto *statusRow = new QHBoxLayout;
    statusRow->setContentsMargins(2, 0, 8, 0);
    statusRow->addWidget(status_);
    column->addLayout(statusRow);
    body->addLayout(column, 1);
    setCentralWidget(surface_);

    connect(group, &QButtonGroup::idClicked, this, &MainWindow::showPage);
    connect(&client_, &CoreProcess::changed, this, &MainWindow::updateProcesses);
    connect(&host_, &CoreProcess::changed, this, &MainWindow::updateProcesses);
    connect(&client_, &CoreProcess::output, this, [this](const QString &text) { appendOutput(clientLog_, text); });
    connect(&host_, &CoreProcess::output, this, [this](const QString &text) { appendOutput(hostLog_, text); });
    connect(&client_, &CoreProcess::message, this, &MainWindow::clientMessage);
    connect(&host_, &CoreProcess::message, this, &MainWindow::hostMessage);
    connect(&client_, &CoreProcess::finished, this, &MainWindow::clientFinished);
    connect(&host_, &CoreProcess::finished, this, &MainWindow::hostFinished);
    pairingTimer_.setInterval(1000);
    connect(&pairingTimer_, &QTimer::timeout, this, &MainWindow::showPairingTime);
    // 没装服务时每次只是一次打开失败
    constexpr int kServiceRetryMs = 2000;
    // 设置里没写服务名时看环境变量 ASHDESK_SERVICE_NAME：界面测试靠它把所有窗口指到一个不存在的服务上。
    // 装着真服务的机器上，测试里的窗口不能接到它——接上了，测试点的吊销、设固定密码就发给真的主机了
    serviceName_ = settings_.value("serviceName", qEnvironmentVariable("ASHDESK_SERVICE_NAME", QStringLiteral("AshDeskSvc"))).toString();
    const auto attachService = [this] {
        if (!host_.busy() && !closing_) host_.attach(serviceName_);
    };
    serviceTimer_.setInterval(kServiceRetryMs);
    connect(&serviceTimer_, &QTimer::timeout, this, attachService);
    serviceTimer_.start();
    QTimer::singleShot(0, this, attachService);
    connect(&client_, &CoreProcess::launched, this, [this] {
        // 密码不进命令行（--password -）：核心起来以后从 stdin 读这一行，最多等 10 s
        if (!pendingPassword_.isEmpty()) {
            client_.send(std::exchange(pendingPassword_, {}));
        }
        if (!numberTrial_) {
            rememberPending();
        }
    });
    connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] { applyTheme(); });
    connect(qApp, &QApplication::focusChanged, this, [](QWidget *previous, QWidget *current) {
        markFocusVisible(previous, false);
        if (current) markFocusVisible(current, current->window()->testAttribute(Qt::WA_KeyboardFocusChange));
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    connect(qApp->styleHints()->accessibility(), &QAccessibilityHints::contrastPreferenceChanged, this,
            [this] { applyTheme(); });
#endif
    bindSettings();
    buildTray();
    refreshDevices();
    applyTheme();
    refreshRecent();
    updateProcesses();
    showPage(0);
    restoreGeometry(settings_.value("window/geometry").toByteArray());
    if (!QFileInfo(coreProgram(false)).isExecutable())
        message(tr("此安装没有远程核心；界面、设备收藏与设置可用。"));
}

QPushButton *MainWindow::iconButton(const QString &text, const QString &name, const char *role, const QString &symbol) {
    auto *widget = button(text, name, role);
    widget->setIconSize(QSize(kButtonIcon, kButtonIcon));
    icons_.append({widget, symbol});
    return widget;
}

QWidget *MainWindow::buildHome() {
    auto *page = new QWidget;
    auto *layout = pageLayout(page, {});
    auto *hero = new QWidget;
    hero->setMaximumWidth(680);
    auto *heroLayout = new QVBoxLayout(hero);
    heroLayout->setContentsMargins(0, 0, 0, 0);
    heroLayout->setSpacing(0);
    auto *centre = new QHBoxLayout;
    centre->addStretch();
    centre->addWidget(hero, 1);
    centre->addStretch();
    layout->addLayout(centre);

    auto *omnibox = new GlassPanel(GlassRole::Action, 29);
    omnibox->setObjectName("omnibox");
    omnibox->enableFocusRing();
    omnibox->setFixedHeight(58 + omnibox->contentsMargins().top() + omnibox->contentsMargins().bottom());
    heroLayout->addWidget(omnibox);
    auto *search = new QHBoxLayout(omnibox);
    search->setContentsMargins(20, 0, 10, 0);
    search->setSpacing(6);
    searchIcon_ = new IconView(20);
    address_ = new QLineEdit;
    address_->setObjectName("address");
    address_->setAccessibleName(tr("对方的号码或地址"));
    address_->setPlaceholderText(tr("对方号码、地址或主机名"));
    address_->setClearButtonEnabled(true);
    connect_ = button(tr("连接"), "connect", "primary");
    search->addWidget(searchIcon_);
    search->addWidget(address_, 1);
    search->addWidget(connect_);
    connect(connect_, &QPushButton::clicked, this, [this] { connectToAddress(address_->text()); });
    connect(address_, &QLineEdit::returnPressed, this, [this] { connectToAddress(address_->text()); });

    auto *local = panel<QHBoxLayout>(heroLayout, 18, {16, 8, 10, 8});
    local->setSpacing(10);
    auto *tag = label(tr("本机"), "tag");
    tag->setWordWrap(false);
    local->addWidget(tag);
    localName_ = label(QHostInfo::localHostName(), "hostName");
    localName_->setWordWrap(false);
    localName_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    local->addWidget(localName_);
    // 对方按地址连时用得上的（CD10）：IPv4 和 IPv6 都列。不列链路本地（要带接口号）和 IPv6 的临时地址（过几个小时就换）
    QStringList addresses;
    for (const QNetworkInterface &face : QNetworkInterface::allInterfaces()) {
        if (!(face.flags() & QNetworkInterface::IsUp)) continue;
        for (const QNetworkAddressEntry &entry : face.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (!ip.isLoopback() && !ip.isLinkLocal() && !entry.isTemporary()) addresses << ip.toString();
        }
    }
    localAddress_ = label(addresses.isEmpty() ? tr("没有可用的网络地址") : addresses.join(" · "), "muted");
    localAddress_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    localAddress_->setAccessibleName(tr("本机网络地址"));
    local->addWidget(localAddress_, 1);
    // 主机在跑并且登记了号码时，这一行换成本机号码和连接密码（showLocal）
    localNumber_ = new QWidget;
    localNumber_->setObjectName("localNumber");
    auto *number = new QHBoxLayout(localNumber_);
    number->setContentsMargins(0, 0, 0, 0);
    number->setSpacing(kLocalNumberSpacing);
    copyNumber_ = iconButton({}, "copyNumber", "", "copy");
    copyNumber_->setAccessibleDescription(tr("本机号码，点一下复制"));
    copyNumber_->setToolTip(tr("复制号码"));
    localPassword_ = label({}, "localPassword");
    localPassword_->setWordWrap(false);
    localPassword_->setAccessibleDescription(tr("连接密码"));
    localPassword_->setToolTip(tr("连接密码"));
    showPassword_ = iconButton({}, "showPassword", "", "eye");
    showPassword_->setCheckable(true);
    showPassword_->setAccessibleName(tr("显示连接密码"));
    showPassword_->setToolTip(showPassword_->accessibleName());
    newPassword_ = iconButton({}, "newPassword", "", "refresh");
    newPassword_->setAccessibleName(tr("换一个连接密码"));
    newPassword_->setToolTip(newPassword_->accessibleName());
    number->addWidget(copyNumber_);
    number->addSpacing(kLocalPasswordGap);
    number->addWidget(localPassword_);
    number->addWidget(showPassword_);
    number->addWidget(newPassword_);
    number->addStretch();
    local->addWidget(localNumber_, 1);
    localNumber_->hide();
    connect(copyNumber_, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(copyNumber_->property("share").toString());
        message(hostPassword_.isEmpty() ? tr("号码已复制") : tr("号码和密码已复制"));
    });
    connect(showPassword_, &QPushButton::toggled, this, [this] { showLocal(); });
    connect(newPassword_, &QPushButton::clicked, this, [this] { host_.send(QStringLiteral("password new")); });
    auto *hostButton = iconButton(tr("被控端"), "manageHost", "", "server");
    hostButton->setAccessibleName(tr("管理被控端"));
    local->addWidget(hostButton);
    connect(hostButton, &QPushButton::clicked, this, [this] { showPage(2); });

    layout->addSpacing(12);
    layout->addWidget(label(tr("最近"), "sectionTitle"));
    auto *recentBox = panel<QVBoxLayout>(layout, 18, {6, 6, 6, 6}, 1);
    recent_ = new QListWidget;
    recent_->setObjectName("recent");
    recent_->setAccessibleName(tr("最近发起的连接"));
    recent_->setMinimumHeight(120);
    recent_->setIconSize(QSize(20, 20));
    seeThrough(recent_);
    recentBox->addWidget(recent_);
    connect(recent_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (!item->data(Qt::UserRole).isValid()) return;
        address_->setText(item->data(Qt::UserRole).toString());
        connectToAddress(address_->text());
    });
    auto *rename = new QAction(tr("重命名"), recent_);
    rename->setObjectName("renameRecent");
    rename->setShortcut(Qt::Key_F2);
    rename->setShortcutContext(Qt::WidgetShortcut);
    auto *regroup = new QAction(tr("分组"), recent_);
    regroup->setObjectName("groupRecent");
    recent_->addActions({rename, regroup});
    recent_->setContextMenuPolicy(Qt::ActionsContextMenu);
    connect(rename, &QAction::triggered, this, [this] { editRecent(false); });
    connect(regroup, &QAction::triggered, this, [this] { editRecent(true); });
    connect(recent_, &QListWidget::currentItemChanged, this, [rename, regroup](QListWidgetItem *item) {
        const bool entry = item && item->data(Qt::UserRole).isValid();
        rename->setEnabled(entry);
        regroup->setEnabled(entry);
    });
    rename->setEnabled(false);
    regroup->setEnabled(false);
    return page;
}

QWidget *MainWindow::buildSession() {
    auto *page = new QWidget;
    auto *layout = pageLayout(page, tr("会话"));
    auto *toolbar = panel<QHBoxLayout>(layout, 18, {16, 8, 10, 8});
    sessionAddress_ = label(tr("尚未选择远程设备"), "sessionAddress");
    toolbar->addWidget(sessionAddress_, 1);
    stopClient_ = iconButton(tr("断开"), "stopClient", "danger", "power");
    toolbar->addWidget(stopClient_);
    connect(stopClient_, &QPushButton::clicked, &client_, &CoreProcess::stop);
    auto *details = panel<QVBoxLayout>(layout, 18, {16, 12, 16, 12});
    details->setSpacing(6);
    clientStatus_ = label({}, "clientStatus");
    details->addWidget(clientStatus_);
    clientStats_ = label({}, "clientStats");
    details->addWidget(clientStats_);
    details->addWidget(label(tr("远程画面在现有客户端窗口中显示。全屏：Ctrl+Alt+Enter。"), "muted"));
    clientLog_ = diagnostics("clientLog");
    panel<QVBoxLayout>(layout, 18, {6, 6, 6, 6}, 1)->addWidget(clientLog_);
    return page;
}

QWidget *MainWindow::buildHost() {
    auto *page = new QWidget;
    auto *layout = pageLayout(page, tr("被控端"));
    auto *form = panel<QVBoxLayout>(layout, 18, {16, 12, 16, 14});
    form->setSpacing(8);
    hostStatus_ = label({}, "hostStatus");
    form->addWidget(hostStatus_);
    hostDetail_ = label({}, "hostDetail");
    form->addWidget(hostDetail_);
    hostStats_ = label({}, "hostStats");
    form->addWidget(hostStats_);
    // 正连着的是用密码连进来的设备时才出现（showStatus）
    trustDevice_ = button(tr("信任这台设备"), "trustDevice");
    form->addWidget(trustDevice_, 0, Qt::AlignLeft);
    trustDevice_->hide();
    connect(trustDevice_, &QPushButton::clicked, this, &MainWindow::trustPeer);
    pairing_ = new QWidget;
    pairing_->setObjectName("pairing");
    auto *pairingLayout = new QHBoxLayout(pairing_);
    pairingLayout->setContentsMargins(0, 2, 0, 2);
    pairingLayout->setSpacing(12);
    pairingTitle_ = label({}, "pairingTitle");
    pairingCode_ = label({}, "pairingCode");
    pairingCode_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pairingTime_ = label({}, "pairingTime");
    pairingTime_->setToolTip(tr("剩余时间"));
    for (QLabel *part : {pairingTitle_, pairingCode_, pairingTime_}) {
        part->setWordWrap(false);
        pairingLayout->addWidget(part);
    }
    pairingLayout->addStretch();
    pairing_->hide();
    form->addWidget(pairing_);
    hostControl_ = new ToggleSwitch(tr("允许键盘与鼠标控制"));
    hostControl_->setObjectName("hostControl");
    hostControl_->setChecked(options_.hostControl);
    form->addWidget(hostControl_);
    numberHost_ = new ToggleSwitch(tr("允许用本机号码连接"));
    numberHost_->setObjectName("numberHost");
    numberHost_->setChecked(options_.numberHost);
    form->addWidget(numberHost_);
    form->addWidget(label(tr("仅接受加密连接。接管确认由现有核心弹窗处理，超时拒绝。"), "muted"));
    auto *actions = new QHBoxLayout;
    startHost_ = button(tr("启动主机"), "startHost", "primary");
    stopHost_ = iconButton(tr("停止"), "stopHost", "danger", "power");
    actions->addWidget(startHost_);
    actions->addWidget(stopHost_);
    actions->addStretch();
    form->addLayout(actions);
    auto *pairingActions = new QHBoxLayout;
    pairDevice_ = button(tr("配对新设备"), "pairDevice");
    pairTemporary_ = button(tr("临时口令"), "pairTemporary");
    pairTemporary_->setToolTip(tr("配上的设备只能连接一次"));
    pairingActions->addWidget(pairDevice_);
    pairingActions->addWidget(pairTemporary_);
    pairingActions->addStretch();
    form->addLayout(pairingActions);
    connect(startHost_, &QPushButton::clicked, this, [this] { startHost(Pairing::None); });
    connect(pairDevice_, &QPushButton::clicked, this, [this] { startHost(Pairing::Device); });
    connect(pairTemporary_, &QPushButton::clicked, this, [this] { startHost(Pairing::Temporary); });
    connect(stopHost_, &QPushButton::clicked, &host_, &CoreProcess::stop);
    hostBanner_ = label(tr("主机运行期间，仅批准你认识的设备。停止会结束本次主机进程。"), "hostBanner");
    layout->addWidget(hostBanner_);
    hostLog_ = diagnostics("hostLog");
    panel<QVBoxLayout>(layout, 18, {6, 6, 6, 6}, 1)->addWidget(hostLog_);
    return page;
}

QWidget *MainWindow::buildDevices() {
    auto *page = new QWidget;
    auto *layout = pageLayout(page, {});
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 10, 0);
    header->addWidget(label(tr("设备"), "pageTitle"), 1);
    auto *add = iconButton(tr("添加设备"), "addDevice", "primary", "plus");
    header->addWidget(add, 0, Qt::AlignTop);
    layout->addLayout(header);
    auto *list = panel<QVBoxLayout>(layout, 18, {10, 8, 10, 12}, 1);
    list->setSpacing(8);
    devices_ = new QTableWidget(0, 2);
    devices_->setObjectName("devices");
    devices_->setAccessibleName(tr("收藏的设备，不代表已配对或在线"));
    devices_->setHorizontalHeaderLabels({tr("设备"), tr("地址")});
    devices_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    devices_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    devices_->verticalHeader()->hide();
    devices_->setSelectionBehavior(QAbstractItemView::SelectRows);
    devices_->setSelectionMode(QAbstractItemView::SingleSelection);
    devices_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    devices_->setShowGrid(false);
    seeThrough(devices_);
    list->addWidget(devices_, 1);
    auto *actions = new QHBoxLayout;
    connectDevice_ = iconButton(tr("连接"), "connectDevice", "", "link");
    removeDevice_ = iconButton(tr("移除收藏"), "removeDevice", "", "trash");
    actions->addWidget(connectDevice_);
    actions->addWidget(removeDevice_);
    actions->addStretch();
    list->addLayout(actions);
    // 能连本机的设备由主机核心的设备表管；没有主机核心的安装里这一栏不出现
    pairedSection_ = new QWidget;
    pairedSection_->setObjectName("pairedSection");
    layout->addWidget(pairedSection_);
    pairedSection_->setVisible(QFileInfo(coreProgram(true)).isExecutable());
    auto *section = new QVBoxLayout(pairedSection_);
    section->setContentsMargins(0, 12, 0, 0);
    section->setSpacing(0);
    section->addWidget(label(tr("已配对设备"), "sectionTitle"));
    paired_ = panel<QVBoxLayout>(section, 18, {10, 4, 10, 8});
    paired_->setSpacing(0);
    paired_->parentWidget()->setObjectName("paired");
    connect(add, &QPushButton::clicked, this, &MainWindow::addDevice);
    connect(removeDevice_, &QPushButton::clicked, this, &MainWindow::removeDevice);
    connect(connectDevice_, &QPushButton::clicked, this, [this] {
        const int row = devices_->currentRow();
        if (row >= 0) connectToAddress(devices_->item(row, 1)->text());
    });
    connect(devices_, &QTableWidget::itemSelectionChanged, this, [this] {
        const bool selected = devices_->currentRow() >= 0;
        connectDevice_->setEnabled(selected && !client_.busy() && QFileInfo(coreProgram(false)).isExecutable());
        removeDevice_->setEnabled(selected);
    });
    return page;
}

QWidget *MainWindow::buildSettings() {
    auto *page = new QWidget;
    auto *layout = pageLayout(page, tr("设置"));
    auto *box = panel<QVBoxLayout>(layout, 18, {12, 10, 12, 12});
    box->setSpacing(8);
    auto *tabs = new QTabWidget;
    tabs->setObjectName("settingsTabs");
    tabs->setDocumentMode(true);
    tabs->tabBar()->setDrawBase(false);
    const auto category = [tabs](const QString &title) {
        auto *section = new QWidget;
        section->setAutoFillBackground(false);
        auto *rows = new QVBoxLayout(section);
        rows->setContentsMargins(4, 12, 4, 4);
        rows->setSpacing(6);
        tabs->addTab(section, title);
        return rows;
    };
    const auto row = [](QVBoxLayout *section, const QString &text, QWidget *control) {
        auto *line = new QHBoxLayout;
        line->setSpacing(16);
        auto *name = label(text);
        name->setBuddy(control);
        if (control->accessibleName().isEmpty()) control->setAccessibleName(text);
        line->addWidget(name, 1);
        line->addWidget(control);
        section->addLayout(line);
    };
    const auto spin = [](const QString &name, int min, int max, int value, const QString &suffix) {
        auto *control = new QSpinBox;
        control->setObjectName(name);
        control->setRange(min, max);
        control->setValue(value);
        control->setSuffix(suffix);
        control->setMinimumWidth(200);
        return control;
    };
    const auto choice = [](const QString &name, const QStringList &items, int index) {
        auto *control = new QComboBox;
        control->setObjectName(name);
        control->addItems(items);
        control->setCurrentIndex(index);
        control->setMinimumWidth(200);
        return control;
    };
    const auto field = [](const QString &name, const QString &text, const QString &placeholder) {
        auto *control = new QLineEdit(text);
        control->setObjectName(name);
        control->setPlaceholderText(placeholder);
        control->setMinimumWidth(200);
        return control;
    };
    const auto toggle = [](const QString &name, const QString &text, bool checked) {
        auto *control = new ToggleSwitch(text);
        control->setObjectName(name);
        control->setChecked(checked);
        return control;
    };
    auto *quality = category(tr("画质"));
    fps_ = spin("fps", 30, 144, options_.fps, " fps");
    bitrate_ = spin("bitrate", 5, 100, options_.bitrate, " Mb/s");
    row(quality, tr("主机帧率"), fps_);
    row(quality, tr("主机码率上限"), bitrate_);
    virtualDisplay_ = toggle("virtualDisplay", tr("主机使用虚拟屏"), options_.virtualDisplay);
    metered_ = toggle("metered", tr("客户端省流"), options_.metered);
    quality->addWidget(virtualDisplay_);
    quality->addWidget(metered_);
    quality->addStretch();
    auto *audio = category(tr("音频"));
    audio_ = toggle("audio", tr("传输系统声音"), options_.audio);
    microphone_ = toggle("microphone", tr("发送本机麦克风"), options_.microphone);
    audio->addWidget(audio_);
    audio->addWidget(microphone_);
    audio->addStretch();
    auto *network = category(tr("网络"));
    rendezvous_ = field("rendezvous", options_.rendezvous, tr("主机:端口，留空用内置的"));
    row(network, tr("信令服务器"), rendezvous_);
    port_ = spin("port", 1, 65535, options_.port, {});
    row(network, tr("默认 UDP 端口"), port_);
    auto *corePath = new QLineEdit(QDir::toNativeSeparators(coreDirectory_));
    corePath->setReadOnly(true);
    corePath->setMinimumWidth(200);
    corePath->setCursorPosition(0);
    row(network, tr("核心目录"), corePath);
    forwardAllow_ = field("forwardAllow", options_.forwardAllow, tr("主机:端口，逗号分隔"));
    forward_ = field("forward", options_.forward, tr("本地端口=主机:端口"));
    socks5_ = spin("socks5", 0, 65535, options_.socks5, {});
    socks5_->setSpecialValueText(tr("关"));
    row(network, tr("主机允许转发到"), forwardAllow_);
    row(network, tr("客户端端口转发"), forward_);
    row(network, tr("客户端 SOCKS5 端口"), socks5_);
    network->addStretch();
    auto *security = category(tr("安全"));
    authentication_ = choice("authentication", {tr("系统已存密钥"), tr("密钥文件")}, options_.authentication);
    row(security, tr("连接凭据"), authentication_);
    auto *keyRow = new QWidget;
    auto *keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyFile_ = new QLineEdit(options_.keyFile);
    keyFile_->setObjectName("keyFile");
    keyFile_->setAccessibleName(tr("十六进制密钥文件路径"));
    keyFile_->setMinimumWidth(160);
    auto *browse = iconButton(tr("选择"), "browseKey", "", "folder");
    keyLayout->addWidget(keyFile_, 1);
    keyLayout->addWidget(browse);
    row(security, tr("密钥文件"), keyRow);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getOpenFileName(this, tr("选择密钥文件"));
        if (!file.isEmpty()) keyFile_->setText(file);
    });
    security->addWidget(label(tr("系统密钥沿用核心的 --psk-import 导入结果；UI 不保存密钥正文。"), "muted"));
    viewOnly_ = toggle("viewOnly", tr("客户端只看画面"), options_.viewOnly);
    clipboard_ = toggle("clipboard", tr("同步文本剪贴板"), options_.clipboard);
    files_ = toggle("files", tr("允许文件复制粘贴"), options_.files);
    security->addWidget(viewOnly_);
    security->addWidget(clipboard_);
    security->addWidget(files_);
    auto *totpRow = new QWidget;
    auto *totpLayout = new QHBoxLayout(totpRow);
    totpLayout->setContentsMargins(0, 0, 0, 0);
    totp_ = choice("totp", {tr("关"), tr("配对时"), tr("配对和每次会话")}, options_.totp);
    totp_->setMinimumWidth(120);
    totp_->setAccessibleName(tr("主机要求二次验证的时机"));
    totpSetup_ = button(tr("生成密钥"), "totpSetup");
    totpLayout->addWidget(totp_, 1);
    totpLayout->addWidget(totpSetup_);
    row(security, tr("二次验证 (TOTP)"), totpRow);
    connect(totpSetup_, &QPushButton::clicked, this, &MainWindow::setupTotp);
    // 身份（CD13）：号码、设备表、密钥、记住的主机用口令加密导出 / 导入，换电脑、重装系统时带走
    auto *exportIdentity = button(tr("导出…"), "identityExport");
    auto *importIdentity = button(tr("导入…"), "identityImport");
    row(security, tr("导出身份"), exportIdentity);
    row(security, tr("导入身份"), importIdentity);
    connect(exportIdentity, &QPushButton::clicked, this, [this] { transferIdentity(true); });
    connect(importIdentity, &QPushButton::clicked, this, [this] { transferIdentity(false); });
    confirm_ = choice("confirm", {tr("密码对了就直接连"), tr("电脑前有人时询问"), tr("每次都询问")}, options_.confirm);
    row(security, tr("有人来连时"), confirm_);
    if (!installDirectory().isEmpty()) {
        service_ = button({}, "service");
        row(security, tr("系统服务"), service_);
        connect(service_, &QPushButton::clicked, this, [this] {
            const QString line = serviceCommandLine(serviceName_);
            if (line.isEmpty()) {
                installService();
            } else if (serviceRunsHere(line)) {
                uninstallService();
            } else {
                switchService(line);
            }
        });
    }
    // 临时密码（号码连接设计 §4.3）：两项都存在主机上，界面只把改动发给在跑的主机，开关的样子照主机报回来的（showTemp）
    tempPassword_ = toggle("tempPassword", tr("临时密码"), false);
    tempKeep_ = toggle("tempKeep", tr("临时密码用过以后不换"), false);
    security->addWidget(tempPassword_);
    security->addWidget(tempKeep_);
    connect(tempPassword_, &QCheckBox::clicked, this, [this](bool on) {
        tempAsked_ = true;
        host_.send(on ? QStringLiteral("password temp on") : QStringLiteral("password temp off"));
    });
    connect(tempKeep_, &QCheckBox::clicked, this, [this](bool keep) {
        tempAsked_ = true;
        host_.send(keep ? QStringLiteral("password temp keep") : QStringLiteral("password temp rotate"));
    });
    // 固定密码（号码连接设计 §4.3）：主机只存派生值，界面这里只管把新密码交给在跑的主机，自己不存
    auto *fixedRow = new QWidget;
    auto *fixedLayout = new QHBoxLayout(fixedRow);
    fixedLayout->setContentsMargins(0, 0, 0, 0);
    fixedState_ = label({}, "fixedState");
    fixedState_->setWordWrap(false);
    fixedPassword_ = new QLineEdit;
    fixedPassword_->setObjectName("fixedPassword");
    fixedPassword_->setAccessibleName(tr("新的固定密码"));
    fixedPassword_->setEchoMode(QLineEdit::Password);
    fixedPassword_->setMinimumWidth(kFixedPasswordWidth);
    showFixed_ = iconButton({}, "showFixed", "", "eye");
    showFixed_->setCheckable(true);
    showFixed_->setAccessibleName(tr("显示固定密码"));
    showFixed_->setToolTip(showFixed_->accessibleName());
    setFixed_ = button(tr("设置", "固定密码"), "setFixed");
    setFixed_->setAccessibleName(tr("设置固定密码"));
    clearFixed_ = button(tr("清除"), "clearFixed");
    clearFixed_->setAccessibleName(tr("清除固定密码"));
    fixedLayout->addWidget(fixedState_);
    fixedLayout->addWidget(fixedPassword_, 1);
    fixedLayout->addWidget(showFixed_);
    fixedLayout->addWidget(setFixed_);
    fixedLayout->addWidget(clearFixed_);
    row(security, tr("固定密码"), fixedRow);
    connect(fixedPassword_, &QLineEdit::textChanged, this, [this] { showFixed(); });
    connect(fixedPassword_, &QLineEdit::returnPressed, setFixed_, &QPushButton::click);
    connect(showFixed_, &QPushButton::toggled, this, [this](bool shown) {
        fixedPassword_->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(setFixed_, &QPushButton::clicked, this, [this] {
        fixedAsked_ = true;
        host_.send(QStringLiteral("password fixed ") + fixedPassword_->text().trimmed());
        // 不用 clear()：它留着撤销记录，密码正显示着的时候按 Ctrl+Z 能把刚设的密码找回来
        fixedPassword_->setText({});
    });
    connect(clearFixed_, &QPushButton::clicked, this, [this] {
        fixedAsked_ = true;
        host_.send(QStringLiteral("password fixed-clear"));
    });
    security->addStretch();
    auto *appearance = category(tr("外观"));
    theme_ = choice("theme", {tr("跟随系统"), tr("浅色"), tr("深色")}, options_.theme);
    material_ = choice("material", {tr("实色"), tr("磨砂"), tr("液态玻璃"), tr("色散")}, options_.material);
    row(appearance, tr("主题"), theme_);
    row(appearance, tr("材质"), material_);
    // 语言名用各自的文字写，不随界面语言翻译
    language_ = choice("language", {tr("跟随系统"), QStringLiteral("中文"), QStringLiteral("English")}, options_.language);
    row(appearance, tr("语言"), language_);
    appearance->addWidget(label(tr("系统开启高对比度时自动使用实色。"), "muted"));
    appearance->addStretch();
    auto *support = category(tr("诊断"));
    auto *tools = new QHBoxLayout;
    netdiag_ = iconButton(tr("开始诊断"), "startNetdiag", "", "pulse");
    auto *pack = iconButton(tr("打包诊断"), "packDiagnostics", "", "archive");
    tools->addWidget(netdiag_);
    tools->addWidget(pack);
    tools->addStretch();
    support->addLayout(tools);
    netdiagResults_ = new QWidget;
    netdiagResults_->setObjectName("netdiagResults");
    auto *results = new QFormLayout(netdiagResults_);
    results->setContentsMargins(0, 6, 0, 0);
    results->setHorizontalSpacing(16);
    for (const DiagItem &item : kDiagItems) {
        auto *value = label({}, QStringLiteral("diag-") + QLatin1String(item.key));
        netdiagValues_.insert(QLatin1String(item.key), value);
        results->addRow(QCoreApplication::translate("ashdesk", item.label), value);
    }
    netdiagVerdict_ = label({}, "netdiagVerdict");
    results->addRow(netdiagVerdict_);
    support->addWidget(netdiagResults_);
    netdiagResults_->hide();
    support->addStretch();
    connect(netdiag_, &QPushButton::clicked, this, &MainWindow::runNetdiag);
    connect(pack, &QPushButton::clicked, this, &MainWindow::exportDiagnostics);
    box->addWidget(tabs);
    box->addWidget(label(tr("设置自动保存；除外观外，下次启动核心时生效。"), "muted"));
    layout->addStretch();
    return page;
}

void MainWindow::buildTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_ = new QSystemTrayIcon(trayIcon(false), this);
    tray_->setObjectName("tray");
    auto *menu = new QMenu(this);
    menu->setObjectName("trayMenu");
    menu->setAccessibleName(tr("AshDesk 托盘菜单"));
    QAction *show = menu->addAction(tr("显示窗口"));
    show->setObjectName("trayShow");
    trayDisconnect_ = menu->addAction(tr("断开当前会话"));
    trayDisconnect_->setObjectName("trayDisconnect");
    trayTrust_ = menu->addAction(tr("信任这台设备"));
    trayTrust_->setObjectName("trayTrust");
    trayPermissions_ = menu->addMenu(tr("收回权限"));
    trayPermissions_->setObjectName("trayPermissions");
    trayPermissions_->setAccessibleName(tr("收回权限"));
    menu->addSeparator();
    QAction *quit = menu->addAction(tr("退出"));
    quit->setObjectName("trayQuit");
    tray_->setContextMenu(menu);
    connect(show, &QAction::triggered, this, &MainWindow::restoreFromTray);
    connect(trayDisconnect_, &QAction::triggered, this, [this] { host_.send(QStringLiteral("disconnect")); });
    connect(trayTrust_, &QAction::triggered, this, [this] {
        // 确认框要有个看得见的父窗口
        restoreFromTray();
        trustPeer();
    });
    connect(quit, &QAction::triggered, this, [this] {
        restoreFromTray();
        exiting_ = true;
        close();
        exiting_ = false;
    });
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restoreFromTray();
    });
}

void MainWindow::restoreFromTray() {
    inTray_ = false;
    setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
}

void MainWindow::bindSettings() {
    for (auto *control : {theme_, material_, authentication_, language_, totp_, confirm_})
        connect(control, &QComboBox::currentIndexChanged, this, [this] { saveOptions(); });
    for (auto *control : {fps_, bitrate_, port_, socks5_})
        connect(control, &QSpinBox::valueChanged, this, [this] { saveOptions(); });
    for (auto *control : {audio_, microphone_, clipboard_, files_, viewOnly_, metered_, hostControl_, numberHost_, virtualDisplay_})
        connect(control, &QCheckBox::toggled, this, [this] { saveOptions(); });
    for (auto *control : {keyFile_, rendezvous_, forwardAllow_, forward_})
        connect(control, &QLineEdit::textChanged, this, [this] { saveOptions(); });
    keyFile_->setEnabled(options_.authentication == 1);
}

void MainWindow::saveOptions() {
    const bool prevFiles = options_.files;
    const bool prevClipboard = options_.clipboard;
    const bool languageChanged = options_.language != language_->currentIndex();
    options_.theme = theme_->currentIndex();
    options_.material = material_->currentIndex();
    options_.language = language_->currentIndex();
    options_.authentication = authentication_->currentIndex();
    options_.keyFile = keyFile_->text().trimmed();
    options_.port = port_->value();
    options_.fps = fps_->value();
    options_.bitrate = bitrate_->value();
    options_.audio = audio_->isChecked();
    options_.microphone = microphone_->isChecked();
    options_.clipboard = clipboard_->isChecked();
    options_.files = files_->isChecked();
    options_.viewOnly = viewOnly_->isChecked();
    options_.metered = metered_->isChecked();
    options_.hostControl = hostControl_->isChecked();
    options_.numberHost = numberHost_->isChecked();
    options_.virtualDisplay = virtualDisplay_->isChecked();
    options_.totp = totp_->currentIndex();
    options_.confirm = confirm_->currentIndex();
    options_.rendezvous = rendezvous_->text().trimmed();
    // 核心按逗号拆、逐字匹配，规则里不能带空白
    options_.forwardAllow = forwardAllow_->text().simplified().remove(' ');
    options_.forward = forward_->text().simplified().remove(' ');
    options_.socks5 = socks5_->value();
    options_.save(settings_);
    keyFile_->setEnabled(options_.authentication == 1);
    applyTheme();
    if (settings_.status() != QSettings::NoError) message(tr("设置保存失败，请检查配置目录权限"));
    else message(languageChanged ? tr("重新打开 AshDesk 后切换语言") : tr("设置已保存"));
    /* 文件/剪贴板策略是主机的启动参数：界面自己起的主机重启一次（会话里先问）；接着的是系统服务的主机时不动它
     * （stop 只会断开界面，再起就是第二个主机抢端口），只提示要重装服务才生效 */
    if (options_.files != prevFiles || options_.clipboard != prevClipboard) {
        if (host_.attached()) {
            message(tr("设置已保存；系统服务的主机按安装时的设置运行，文件和剪贴板开关要重装系统服务后才对它生效"));
        } else if (host_.busy() && (hostPhase_ != "stream" || confirm(this, tr("设置"), tr("文件和剪贴板开关要重启主机才生效，重启会结束当前会话，现在重启？")))) {
            hostRestart_ = hostPairing_;
            host_.stop();
        }
    }
}

void MainWindow::applyTheme() {
    QStyleHints *hints = QGuiApplication::styleHints();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    {
        // Also themes the native title bar; the blocker avoids re-entering through colorSchemeChanged.
        const QSignalBlocker blocker(hints);
        if (options_.theme == 0) hints->unsetColorScheme();
        else hints->setColorScheme(options_.theme == 2 ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
    }
#endif
    const bool dark = options_.theme == 0 ? hints->colorScheme() == Qt::ColorScheme::Dark : options_.theme == 2;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    const bool highContrast = hints->accessibility()->contrastPreference() == Qt::ContrastPreference::HighContrast;
#else
    const bool highContrast = false;
#endif
    const Material material = highContrast ? Material::Solid : Material(options_.material);
    const int appearance = int(dark) + 2 * int(material) + 8 * int(highContrast);
    if (appearance == appearance_) return;
    appearance_ = appearance;
    surface_->setAppearance(dark, material, highContrast);
    const bool glass = material != Material::Solid;

    const QColor text(dark ? "#E9EDF2" : "#161B22");
    const QColor muted = highContrast ? QColor(dark ? "#CBD3DC" : "#3B4452")
        : glass                       ? QColor(dark ? "#B4BDC9" : "#434C5A")
                                      : QColor(dark ? "#A3ADBA" : "#586272");
    const QColor accent("#0A74DA");
    const QColor accentText(dark ? "#6BBCFF" : "#0063C7");
    const QColor window(dark ? "#0E1217" : "#E8ECF1");
    const QColor field(dark ? "#171C23" : "#FFFFFF");
    const QColor line = highContrast ? (dark ? rgba(255, 255, 255, .45) : rgba(0, 0, 0, .42))
                                     : (dark ? rgba(233, 237, 242, .09) : rgba(22, 27, 34, .10));
    const QColor strong = highContrast ? (dark ? rgba(255, 255, 255, .60) : rgba(0, 0, 0, .55))
                                       : (dark ? rgba(233, 237, 242, .16) : rgba(22, 27, 34, .16));
    const QColor hover = dark ? rgba(255, 255, 255, .07) : rgba(22, 27, 34, .06);
    const QColor buttonFill = glass ? (dark ? rgba(255, 255, 255, .08) : rgba(255, 255, 255, .62))
                                    : QColor(dark ? "#20262F" : "#FFFFFF");
    const QColor buttonHover = glass ? (dark ? rgba(255, 255, 255, .14) : rgba(255, 255, 255, .86))
                                     : QColor(dark ? "#272E38" : "#F3F5F8");
    const QColor selected = glass ? (dark ? rgba(255, 255, 255, .12) : rgba(255, 255, 255, .78))
                                  : (dark ? rgba(107, 188, 255, .16) : rgba(10, 116, 218, .13));
    const QColor warnText(dark ? "#FFC255" : "#8A5600");
    const QColor warnFill = dark ? rgba(255, 194, 85, .12) : rgba(217, 138, 0, .12);

    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, field);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, field);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::ToolTipBase, field);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Highlight, accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, accentText);
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, muted);
    qApp->setPalette(palette);

    // Longer keys first so "@accentHover" is not consumed by "@accent".
    const QList<QPair<QString, QColor>> tokens{
        {"@accentHover", QColor("#0B67C2")}, {"@buttonHover", buttonHover}, {"@warnText", warnText},
        {"@warnFill", warnFill}, {"@selected", selected}, {"@window", window},
        {"@button", buttonFill}, {"@accent", accent}, {"@danger", QColor("#D93036")}, {"@strong", strong},
        {"@muted", muted}, {"@hover", hover}, {"@field", field}, {"@text", text}, {"@line", line},
        {"@ring", QColor(dark ? "#6BBCFF" : "#0A74DA")}};
    QString sheet = QString::fromUtf8(kStyleSheet);
    for (const auto &[key, color] : tokens) sheet.replace(key, css(color));
    setStyleSheet(sheet);

    for (const auto &[target, symbol] : icons_) {
        const QString role = target->property("role").toString();
        if (role == "nav") target->setIcon(lineIcon(symbol, muted, accentText));
        else if (role == "primary" || role == "danger") target->setIcon(lineIcon(symbol, Qt::white, {}, muted));
        else target->setIcon(lineIcon(symbol, text, {}, muted));
    }
    logo_->setIcon(brandIcon(dark));
    searchIcon_->setIcon(lineIcon("search", muted));
    recentIcon_ = lineIcon("monitor", muted);
    recentOnlineIcon_ = dotted(recentIcon_, recent_->iconSize(), QColor("#2EB872"), devicePixelRatioF());
    recentBusyIcon_ = dotted(recentIcon_, recent_->iconSize(), QColor("#F5A524"), devicePixelRatioF());
    for (int i = 0; i < recent_->count(); ++i) {
        QListWidgetItem *item = recent_->item(i);
        if (item->data(Qt::UserRole).isValid()) {
            item->setIcon(recentIcon(RecentStatus(item->data(kRecentStatusRole).toInt())));
        }
    }
    setProperty("darkTheme", dark);
}

void MainWindow::showPage(int index) {
    pages_->setCurrentIndex(index);
    navigation_[index]->setChecked(true);
    if (index == 0) {
        probeRecent();
    }
    if (index == 3 && !pairedSection_->isHidden()) refreshPaired();
}

void MainWindow::connectToAddress(const QString &address, const QString &totp) {
    NumberTarget target;
    if (parseNumberTarget(address, target)) {
        // 对方复制过来的“号码 + 密码”：直接连，地址栏里只留号码，密码不留在屏幕上
        address_->setText(groupDigits(target.number));
        connectToNumber(target);
        return;
    }
    const int numbered = parseNumber(address, target.number);
    if (numbered < 0) {
        // 纯数字不往下当地址认：QUrl 会把它当成一个整数写的 IPv4 地址
        message(tr("号码是 9 位数字，例如 123 456 789"));
        address_->setFocus();
        return;
    }
    if (numbered > 0) {
        connectToNumber(target);
        return;
    }
    Endpoint endpoint;
    QString error;
    if (!parseEndpoint(address, options_.port, endpoint, error)) {
        message(error);
        address_->setFocus();
        return;
    }
    if (!authenticationReady() || client_.busy()) return;
    // 没存过共享密钥、也没选密钥文件：按号码认，号码从对方的发现应答拿，要密码就问（docs/CrossDesk借鉴设计.md CD10）
    if (options_.authentication != 1 && !QFileInfo::exists(QDir(dataDirectory_).filePath("psk.dpapi"))) {
        connectDirect(endpoint, {});
        return;
    }
    pendingAddress_ = endpoint.address();
    pendingNumber_.clear();
    pendingDirect_ = {};
    launchClient(clientArguments(endpoint, options_, totp));
}

void MainWindow::connectToNumber(const NumberTarget &target) {
    if (client_.busy()) {
        return;
    }
    pendingAddress_ = groupDigits(target.number);
    pendingNumber_ = target.number;
    pendingDirect_ = {};
    pendingPassword_ = target.password;
    numberTrial_ = target.password.isEmpty();
    launchClient(numberArguments(target, options_));
}

void MainWindow::connectDirect(const Endpoint &endpoint, const QString &password) {
    if (client_.busy()) {
        return;
    }
    pendingAddress_ = endpoint.address();
    pendingNumber_.clear();
    pendingDirect_ = endpoint;
    pendingPassword_ = password;
    numberTrial_ = password.isEmpty();
    launchClient(directArguments(endpoint, !password.isEmpty(), options_));
}

QString MainWindow::askPassword(const QString &title, bool again) {
    QDialog dialog(this);
    dialog.setObjectName("passwordPrompt");
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(kPasswordPromptWidth);
    // 两列：密码框和它的“显示”按钮占一行，原因和下面的按钮占满两列
    auto *layout = new QGridLayout(&dialog);
    if (again && !clientError_.isEmpty()) {
        layout->addWidget(label(clientError_, "passwordHint"), 0, 0, 1, 2);
    }
    auto *code = new QLineEdit;
    code->setObjectName("passwordCode");
    code->setAccessibleName(tr("对方的连接密码"));
    code->setPlaceholderText(code->accessibleName());
    // 默认遮着，同连接页上本机的密码；遮着时复制、拖出照 QLineEdit::Password 的默认（不让）
    code->setEchoMode(QLineEdit::Password);
    // 不用 iconButton：对话框用完就没了，留在换主题时重画图标的那张表（icons_）里就是野指针。图标照现在的配色画一次
    auto *reveal = button({}, "showPasswordCode");
    reveal->setCheckable(true);
    reveal->setIcon(lineIcon("eye", palette().color(QPalette::WindowText), {}, palette().color(QPalette::Disabled, QPalette::WindowText)));
    reveal->setIconSize(QSize(kButtonIcon, kButtonIcon));
    reveal->setAccessibleName(tr("显示连接密码"));
    reveal->setToolTip(reveal->accessibleName());
    connect(reveal, &QPushButton::toggled, code, [code](bool shown) { code->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password); });
    layout->addWidget(code, 1, 0);
    layout->addWidget(reveal, 1, 1);
    auto *actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton *accept = actions->button(QDialogButtonBox::Ok);
    accept->setText(tr("连接"));
    accept->setEnabled(false);
    actions->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(actions, 2, 0, 1, 2);
    connect(code, &QLineEdit::textChanged, accept,
            [accept](const QString &text) { accept->setEnabled(!parsePassword(text).isEmpty()); });
    connect(actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted ? parsePassword(code->text()) : QString();
}

void MainWindow::launchClient(const QStringList &arguments) {
    if (!client_.start(coreProgram(false), arguments + QStringList{"--log", coreLog("client")})) {
        pendingPassword_.clear();
        numberTrial_ = false;
        message(client_.status());
        return;
    }
    clientLog_->clear();
    clientError_.clear();
    clientErrorCode_ = 0;
    sessionAddress_->setText(pendingAddress_);
    // 没带密码的试连先不切页：对方不认得这台设备的话，核心只问一下信令服务对方在不在、收不收密码（不叫醒对方；
    // 一般几十毫秒，信令服务不回话时一秒多）就退出，要密码就在原地弹密码框。这段时间状态栏说一声
    if (!numberTrial_) {
        showPage(1);
    }
    message(numberTrial_ ? tr("正在查找对方…") : QString());
}

void MainWindow::rememberPending() {
    QList<RecentConnection> recent = loadRecent(settings_);
    rememberRecent(recent, pendingAddress_);
    saveRecent(settings_, recent);
    refreshRecent();
}

QString MainWindow::probeAddress(const QString &address) const {
    QString number;
    return parseNumber(address, number) == 1 ? number + u'@' + signalServer(options_) : address;
}

void MainWindow::showLocal() {
    const bool numbered = !hostNumber_.isEmpty();
    const bool password = !hostPassword_.isEmpty();
    localName_->setVisible(!numbered);
    localAddress_->setVisible(!numbered);
    localNumber_->setVisible(numbered);
    copyNumber_->setText(groupDigits(hostNumber_));
    QString tip = password ? tr("复制号码和密码") : tr("复制号码");
    if (hostOffline_) {
        tip = tr("没连上信令服务器，别人暂时用不了本机号码");
    }
    copyNumber_->setToolTip(tip);
    // 点号码复制出去的文字：开着连接密码就连密码一起，对方把这一句整个贴进地址栏就能连（parseNumberTarget）
    copyNumber_->setProperty(
        "share", password ? tr("号码 %1，密码 %2").arg(groupDigits(hostNumber_), groupDigits(hostPassword_)) : hostNumber_);
    if (copyNumber_->property("offline").toBool() != hostOffline_) {
        copyNumber_->setProperty("offline", hostOffline_);
        copyNumber_->style()->unpolish(copyNumber_);
        copyNumber_->style()->polish(copyNumber_);
    }
    for (QWidget *part : {static_cast<QWidget *>(localPassword_), static_cast<QWidget *>(showPassword_),
                          static_cast<QWidget *>(newPassword_)}) {
        part->setVisible(password);
    }
    localPassword_->setText(showPassword_->isChecked() ? groupDigits(hostPassword_) : QStringLiteral("•••• ••••"));
}

// 主机要二次验证码时客户端报 error=5 并以退出码 3 结束：问到码就带着它重连，码不对会再回到这里；取消就停在失败状态
void MainWindow::retryWithTotp() {
    QDialog dialog(this);
    dialog.setObjectName("totpPrompt");
    dialog.setWindowTitle(tr("二次验证"));
    dialog.setMinimumWidth(320);
    auto *layout = new QVBoxLayout(&dialog);
    auto *code = new QLineEdit;
    code->setObjectName("totpCode");
    code->setAccessibleName(tr("验证器上的 6–8 位验证码"));
    code->setPlaceholderText(code->accessibleName());
    code->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{6,8}")), code));
    layout->addWidget(code);
    auto *actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton *accept = actions->button(QDialogButtonBox::Ok);
    accept->setText(tr("连接"));
    accept->setEnabled(false);
    actions->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(actions);
    connect(code, &QLineEdit::textChanged, accept, [accept, code] { accept->setEnabled(code->hasAcceptableInput()); });
    connect(actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted) connectToAddress(pendingAddress_, code->text());
}

void MainWindow::startHost(Pairing pairing) {
    if (!authenticationReady() || !totpReady()) return;
    if (host_.busy()) {
        // 配对窗口是主机的启动参数：先停，hostFinished 里再带着它启动
        if (hostPhase_ == "stream" &&
            !confirm(this, pairing == Pairing::Temporary ? tr("临时口令") : tr("配对新设备"),
                     tr("重启主机会结束当前会话，继续？")))
            return;
        hostRestart_ = pairing;
        host_.stop();
        return;
    }
    const QDir directory(dataDirectory_);
    const HostStore store{QFileInfo::exists(directory.filePath("psk.dpapi")), QDir::toNativeSeparators(directory.filePath("number.txt"))};
    const QStringList arguments = hostArguments(options_, pairing, store) << "--log" << coreLog("host");
    if (!host_.start(coreProgram(true), arguments)) {
        message(host_.status());
        return;
    }
    hostPairing_ = pairing;
    offeredDrivers_.clear();
    hostLog_->clear();
    message({});
}

void MainWindow::goOnline() {
    if (!options_.numberHost || host_.busy()) {
        return;
    }
    // 先看主机是不是归系统服务管：是的话接上它，再起一个只会抢同一个端口
    host_.attach(serviceName_);
    if (!host_.busy()) {
        startHost(Pairing::None);
    }
}

bool MainWindow::totpReady() {
    if (options_.totp != 0 && !QFileInfo::exists(QDir(dataDirectory_).filePath("totp.dpapi"))) {
        message(tr("请先在设置 → 安全中生成二次验证密钥。"));
        return false;
    }
    return true;
}

QString MainWindow::coreLog(const QString &name) const {
    return QDir::toNativeSeparators(coreLogFile(QDir(dataDirectory_).filePath("logs"), name));
}

void MainWindow::clientMessage(const CoreMessage &message) {
    if (message.topic == "phase") {
        clientPhase_ = message.value("phase");
        clientPath_ = message.value("path");
        clientError_.clear();
        if (std::exchange(numberTrial_, false)) {
            // 没带密码的试连过了核心那一关：对方认得这台设备，这才是真的在连。“正在查找对方…”交给会话页的阶段
            rememberPending();
            showPage(1);
            this->message({});
        }
    } else if (message.topic == "error") {
        clientErrorCode_ = message.value("error").toInt();
        clientError_ = message.value("text");
    } else if (message.topic == "remembered") {
        this->message(message.value("remembered") == "1" ? tr("对方已信任这台设备，下次连接不用密码")
                                                         : tr("对方已信任这台设备，但密钥没能存下来，下次连接还要密码"));
    } else if (message.topic == "stats") {
        clientPath_ = message.value("path");
        clientStats_->setText(statsText(message));
    }
    showStatus();
}

void MainWindow::hostMessage(const CoreMessage &message) {
    if (message.topic == "phase") {
        hostPhase_ = message.value("phase");
        hostPeer_ = message.value("peer");
        hostPeerName_ = message.value("name");
        hostGranted_ = hostPerms_ = permissions(message.value("perms"));
        // 信任入口只给带着设备号的：确认以后要把这一台的设备号带回去（trust 设备号）。trust=1 却不带 dev 的是旧版主机，
        // 它不认带设备号的 trust，不给入口
        hostTrustDevice_ = message.value("trust") == "1" ? message.value("dev") : QString();
        hostStats_->clear();
        listSessionPermissions();
    } else if (message.topic == "fixed") {
        fixedReported(message);
    } else if (message.topic == "temp") {
        tempReported(message);
    } else if (message.topic == "trusted") {
        showTrusted(message);
    } else if (message.topic == "device") {
        pairedListing_.append(message);
    } else if (message.topic == "devices") {
        paired_->parentWidget()->setEnabled(true);
        showPaired(std::exchange(pairedListing_, {}));
    } else if (message.topic == "revoked" || message.topic == "device-perm") {
        pairedChanged(message);
    } else if (message.topic == "stats") {
        hostPerms_ = permissions(message.value("perms"));
        hostStats_->setText(statsText(message));
    } else if (message.topic == "pair") {
        showPairing(message);
    } else if (message.topic == "number") {
        hostNumber_ = message.value("id");
        const bool offline = message.value("online") == "0";
        if (offline && !hostOffline_) {
            this->message(tr("没连上信令服务器，别人暂时用不了本机号码"));
        }
        hostOffline_ = offline;
        showLocal();
    } else if (message.topic == "password") {
        // 临时密码关着时主机报的 code 是空的：连接页收起密码，复制只给号码
        hostPassword_ = message.value("code");
        showLocal();
    } else if (message.topic == "service") {
        // 服务换了代理（退出、重拉、换会话）：上一个代理报的会话状态作废，号码、密码、两种密码的设置等新代理再报
        serviceAgent_ = message.value("agent");
        clearHostSession();
        hostNumber_.clear();
        hostPassword_.clear();
        hostOffline_ = false;
        hostFixed_.reset();
        hostTempOn_.reset();
        showLocal();
        showFixed();
        showTemp();
        // 设备页开着：新代理起来了就按它的设备表重画
        if (serviceAgent_ != "0" && pages_->currentIndex() == kDevicesPage) {
            refreshPaired();
        }
    } else if (message.topic == "need" && message.value("admin") == "vdisplay") {
        // 驱动在，是主机没有管理员权限：再装一遍也没用，只说明原因
        this->message(tr("虚拟屏要以管理员身份运行 AshDesk 才能使用"));
    } else if (message.topic == "need" && message.value("screen") == "1") {
        // 主机随后就退出：不说的话连接页上只是没有号码，看不出为什么
        this->message(tr("这个会话里采不到屏幕，本机没有上线。远程桌面会话里会这样；主机由系统服务运行时不受影响"));
    } else if (message.topic == "need") {
        // 对话框自己转事件循环，不能在核心的状态信号里直接开
        const QString driver = message.value("driver");
        QTimer::singleShot(0, this, [this, driver] { offerDriver(driver); });
    }
    showStatus();
}

// 核心报 "need driver=X"：功能真用到了驱动 X 而它没装（借鉴设计 §4.9：用到时再装），问用户要不要现在装。
// 同一次主机运行里每个驱动只问一次。这次界面运行里已经装成功过的不再问：主机还报缺它，
// 是装了也用不上（虚拟屏的控制接口只许管理员打开；包里没有这个驱动时安装脚本只警告、照样返回 0），再装一遍也没用
void MainWindow::offerDriver(const QString &driver) {
    QString question = driver == "vdisplay" ? tr("虚拟屏驱动还没有安装，现在安装？")
                       : driver == "vaudio" ? tr("虚拟声卡驱动还没有安装，放不出对方的麦克风。现在安装？")
                                            : QString();
    if (question.isEmpty() || offeredDrivers_.contains(driver)) return;
    if (!driverCertificate(installDirectory(), driver).isEmpty()) {
        question += tr("驱动是开发版自签的，安装时会把它的签名证书加进本机信任的证书。");
    }
    offeredDrivers_ << driver;
    if (installedDrivers_.contains(driver)) message(tr("驱动已经装过，主机仍然用不了它，原因见被控端的日志"));
    else if (confirm(this, tr("安装驱动"), question, tr("安装"))) installDriver(driver);
}

void MainWindow::installDriver(const QString &driver) {
    const QString directory = installDirectory();
    if (directory.isEmpty()) {
        message(tr("没有找到安装脚本。请在管理员 PowerShell 里运行安装包中的 %1").arg(QStringLiteral("install.ps1 -Drivers ") + driver));
        return;
    }
    const bool started = runElevated(driverInstallCommand(directory, driver), this,
                                     [this, driver](int code) { driverInstalled(driver, code); });
    message(started ? tr("正在安装驱动…") : tr("驱动安装没有启动"));
}

void MainWindow::driverInstalled(const QString &driver, int code) {
    if (code != 0) {
        message(tr("驱动安装失败（退出码 %1）。在管理员 PowerShell 里运行安装目录的 %2 可以看到原因")
                    .arg(code)
                    .arg(QStringLiteral("install.ps1 -Drivers ") + driver));
        return;
    }
    installedDrivers_ << driver;
    // 虚拟屏：主机就是因为缺它才退出的，照原样再启动一次；虚拟声卡要到主机下次启动才用得上
    // 服务的主机：没有亮着的屏时每次开流都会去插，下一次连进来就用上了
    if (driver == "vdisplay" && !host_.busy()) {
        startHost(hostPairing_);
    } else if (driver == "vdisplay" && host_.attached()) {
        message(tr("驱动已安装，下次有人连进来时用虚拟屏"));
    } else {
        message(tr("驱动已安装，下次启动主机时生效"));
    }
}

void MainWindow::offerService() {
    if (settings_.value("service/offered", false).toBool() || installDirectory().isEmpty() || serviceInstalled(serviceName_)) {
        return;
    }
    settings_.setValue("service/offered", true);
    if (confirm(this, tr("系统服务"),
                tr("装成系统服务？关掉远程桌面以后、锁屏、登录界面、重启后没人登录时都能连进来。要管理员授权一次，本机号码会换成服务的号码。"),
                tr("安装"))) {
        installService();
    }
}

void MainWindow::installService() {
    const QString programData = qEnvironmentVariable("ProgramData");
    const std::optional<QStringList> arguments =
        serviceHostArguments(options_, programData.isEmpty() ? QString() : QDir(programData).filePath(QStringLiteral("AshDesk")));
    if (!arguments) {
        message(tr("系统服务读不到你账户里存的共享密钥和二次验证密钥：改用号码连接或密钥文件、关掉二次验证后再装"));
        return;
    }
    // 自己起的主机占着端口和号码：先停，hostFinished 里接着装
    if (host_.busy() && !host_.attached()) {
        if (hostPhase_ == "stream" && !confirm(this, tr("系统服务"), tr("安装会结束当前会话，继续？"))) {
            return;
        }
        serviceAfterStop_ = true;
        host_.stop();
        return;
    }
    serviceBusy_ = runElevated(serviceInstallCommand(installDirectory(), *arguments), this,
                               [this](int code) { serviceChanged(true, code); });
    message(serviceBusy_ ? tr("正在安装系统服务…") : tr("系统服务没有安装"));
    if (!serviceBusy_) {
        goOnline();
    }
    showService();
}

void MainWindow::uninstallService() {
    if (!confirm(this, tr("系统服务"), tr("卸载系统服务？之后关掉远程桌面以后、锁屏时就连不进来了。"), tr("卸载"))) {
        return;
    }
    serviceBusy_ = runElevated(serviceUninstallCommand(installDirectory()), this, [this](int code) { serviceChanged(false, code); });
    message(serviceBusy_ ? tr("正在卸载系统服务…") : tr("系统服务没有卸载"));
    showService();
}

void MainWindow::serviceChanged(bool installing, int code) {
    serviceBusy_ = false;
    showService();
    if (code != 0) {
        message((installing ? tr("系统服务安装失败（退出码 %1）。在管理员 PowerShell 里运行安装目录的 %2 可以看到原因")
                            : tr("系统服务卸载失败（退出码 %1）。在管理员 PowerShell 里运行安装目录的 %2 可以看到原因"))
                    .arg(code)
                    .arg(installing ? QStringLiteral("install.ps1 -InPlace -Service") : QStringLiteral("bin\\adrd_svc.exe uninstall")));
        if (installing) {
            goOnline();
        }
        return;
    }
    message(installing ? tr("已装成系统服务") : tr("系统服务已卸载"));
    // 装好：服务的主机起来以后 serviceTimer_ 接上它。卸掉：服务的主机断开以后本机照常上线
    if (installing) {
        host_.attach(serviceName_);
    } else if (host_.busy()) {
        onlineAfterService_ = true;
    } else {
        goOnline();
    }
}

// 服务装在别的目录（以前的绿色包）：装的命令同一条，install.ps1 先卸掉旧的再照这一份装
void MainWindow::switchService(const QString &commandLine) {
    const QString there = QDir::toNativeSeparators(QFileInfo(QDir::fromNativeSeparators(serviceProgram(commandLine))).path());
    if (confirm(this, tr("系统服务"), tr("系统服务现在用的是另一份 AshDesk（%1），换成这一份？").arg(there), tr("换成这一份"))) {
        installService();
    }
}

void MainWindow::showService() {
    if (service_ == nullptr) {
        return;
    }
    const QString line = serviceCommandLine(serviceName_);
    const bool here = serviceRunsHere(line);
    const QString action = line.isEmpty() ? tr("安装") : here ? tr("卸载") : tr("换成这一份");
    service_->setText(action);
    // 按钮的名字是行标题，读屏和 UI 自动化读不到上面的字：把动作也写进去
    service_->setAccessibleName(tr("系统服务：%1").arg(action));
    service_->setToolTip(line.isEmpty() || here ? QString()
                                                : tr("系统服务现在用的是 %1").arg(QDir::toNativeSeparators(serviceProgram(line))));
    service_->setEnabled(!serviceBusy_);
}

// "pair code=… ttl=… temp=…" 把码亮出来并开始倒计时；"pair done" / "pair closed" 没有 code，收起
void MainWindow::showPairing(const CoreMessage &pairing) {
    QString code = pairing.value("code");
    pairing_->setVisible(!code.isEmpty());
    if (code.isEmpty()) {
        pairingTimer_.stop();
        if (pairing.action != "done") return;
        message(tr("已配对：%1").arg(pairing.value("name")));
        if (pages_->currentIndex() == 3) refreshPaired();
        return;
    }
    for (qsizetype i = 3; i < code.size(); i += 4) code.insert(i, ' ');
    pairingTitle_->setText(pairing.value("temp") == "1" ? tr("临时口令") : tr("配对码"));
    pairingCode_->setText(code);
    pairingDeadline_ = QDateTime::currentDateTime().addSecs(pairing.value("ttl").toInt());
    showPairingTime();
    pairingTimer_.start();
}

void MainWindow::showPairingTime() {
    const qint64 left = QDateTime::currentDateTime().secsTo(pairingDeadline_);
    pairingTime_->setText(QStringLiteral("%1:%2").arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0')));
    if (left > 0) return;
    pairing_->hide();
    pairingTimer_.stop();
}

// 托盘“收回权限”子菜单：列出这个会话开始时设备有的权限，勾选状态在 showStatus 里跟着核心报的有效权限走
void MainWindow::listSessionPermissions() {
    if (!tray_) return;
    trayPermissions_->clear();
    for (const Permission &permission : kPermissions) {
        const QString key = QLatin1String(permission.key);
        if (!hostGranted_.contains(key)) continue;
        QAction *action = trayPermissions_->addAction(QCoreApplication::translate("ashdesk", permission.label));
        action->setCheckable(true);
        action->setData(key);
        connect(action, &QAction::triggered, this, [this, key](bool allowed) {
            host_.send((allowed ? QStringLiteral("perm +") : QStringLiteral("perm -")) + key);
        });
    }
}

void MainWindow::trustPeer() {
    // 确认框转着自己的事件循环，开着的时候会话可能换了人（新的 phase 行照常进来）：先记下弹框这一刻的设备号
    const QString device = hostTrustDevice_;
    if (confirm(this, tr("信任这台设备"), tr("信任 %1？以后它按号码连接本机不用输密码，可以在设备页吊销。").arg(hostPeerName_),
                tr("信任"))) {
        host_.send(QStringLiteral("trust ") + device);
    }
}

void MainWindow::showTrusted(const CoreMessage &trusted) {
    const QString error = trusted.value("error");
    if (trusted.value("ok") != "1") {
        // changed：确认时的那台已经走了、换了一台连进来。入口照最新的 phase 行（新来的那台能不能信任）
        message(error == "none"         ? tr("现在没有用密码连进来的设备，没有可以信任的")
                : error == "changed"    ? tr("确认时的那台已经断开，换了别的设备连进来，没有信任新来的这台")
                : error == "old-client" ? tr("对方的程序太旧，记不住密钥，没法信任它")
                : error == "store"      ? tr("设备表没能保存，没有信任成功")
                                        : tr("没有信任成功"));
        return;
    }
    message(tr("已信任 %1").arg(trusted.value("name")));
    if (pages_->currentIndex() == kDevicesPage) {
        refreshPaired();
    }
}

// @ui fixed set=0|1 [error=…]：主机启动、界面设置 / 清除、本机换号以后各来一条
void MainWindow::fixedReported(const CoreMessage &fixed) {
    const bool set = fixed.value("set") == "1";
    const bool asked = std::exchange(fixedAsked_, false);
    const QString error = fixed.value("error");
    hostFixed_ = set;
    if (error == "weak") {
        message(tr("固定密码要 8–64 个英文字母、数字或符号，不能只有数字、空格和连字符"));
    } else if (error == "store") {
        message(tr("固定密码没能存取，原因见被控端的日志"));
    } else if (error == "off") {
        // 这个主机根本没有连接密码：行里的 set 说明不了什么，这一行收起来，和没报过一样
        hostFixed_.reset();
        message(tr("这个主机没开连接密码，用不了固定密码"));
    } else if (error == "renumbered") {
        message(tr("本机换了号码，原来的固定密码作废了，请重新设置"));
    } else if (!error.isEmpty()) {
        message(tr("固定密码没有改成（%1）").arg(error));
    } else if (asked) {
        const QString notice = passwordsOffNotice();
        message(!notice.isEmpty() ? notice : set ? tr("固定密码已设置") : tr("固定密码已清除"));
    }
    showFixed();
}

// @ui temp on=0|1 keep=0|1 [error=…]：主机启动、改了设置以后各来一条；带 error 时设置没改，行里是原来的
void MainWindow::tempReported(const CoreMessage &temp) {
    const bool asked = std::exchange(tempAsked_, false);
    const QString error = temp.value("error");
    hostTempOn_ = temp.value("on") == "1";
    hostTempKeep_ = temp.value("keep") == "1";
    if (error == "off") {
        // 同固定密码：这个主机没有连接密码，行里的两个值是默认的，不是它的设置
        hostTempOn_.reset();
        message(tr("这个主机没开连接密码，用不了临时密码"));
    } else if (error == "store") {
        message(tr("临时密码的设置没能保存，原因见被控端的日志"));
    } else if (!error.isEmpty()) {
        message(tr("临时密码的设置没有改成（%1）").arg(error));
    } else if (asked) {
        const QString notice = passwordsOffNotice();
        message(notice.isEmpty() ? tr("设置已保存") : notice);
    }
    showTemp();
}

void MainWindow::showTemp() {
    const bool known = hostTempOn_.has_value();
    const bool on = hostTempOn_.value_or(false);
    // 同固定密码那一行：主机在跑并且开着连接密码才会报（@ui temp），没报过就没处改
    const QString tip = known          ? QString()
                        : host_.busy() ? tr("这个主机没开连接密码，用不了临时密码")
                                       : tr("主机在运行时才能设置临时密码");
    // setChecked 不发 clicked：照主机报的摆开关，不会又发一遍命令
    tempPassword_->setChecked(on);
    tempKeep_->setChecked(known && hostTempKeep_);
    tempPassword_->setEnabled(known);
    tempKeep_->setEnabled(on);
    tempPassword_->setToolTip(tip);
    tempKeep_->setToolTip(tip);
}

QString MainWindow::passwordsOffNotice() const {
    const bool tempOff = hostTempOn_.has_value() && !*hostTempOn_;
    const bool fixedOff = hostFixed_.has_value() && !*hostFixed_;
    return tempOff && fixedOff ? tr("临时密码关着，也没设固定密码：现在只有信任过的设备能用号码连进来") : QString();
}

void MainWindow::showFixed() {
    const bool known = hostFixed_.has_value();
    const bool set = hostFixed_.value_or(false);
    // 主机在跑并且开着连接密码才会报固定密码设没设（@ui fixed）；没报过就没处设
    const QString tip = known          ? QString()
                        : host_.busy() ? tr("这个主机没开连接密码，用不了固定密码")
                                       : tr("主机在运行时才能设置固定密码");
    fixedState_->setText(!known ? QString() : set ? tr("已设置") : tr("未设置"));
    fixedPassword_->setEnabled(known);
    showFixed_->setEnabled(known);
    setFixed_->setEnabled(known && validFixedPassword(fixedPassword_->text().trimmed()));
    clearFixed_->setEnabled(set);
    fixedPassword_->setToolTip(tip);
    setFixed_->setToolTip(tip);
    clearFixed_->setToolTip(tip);
}

void MainWindow::clientFinished(int code) {
    const bool trial = std::exchange(numberTrial_, false);
    pendingPassword_.clear();
    clientPhase_.clear();
    clientStats_->clear();
    showStatus();
    if (trial) {
        // “正在查找对方…”到这里为止：要密码就弹密码框，别的原因在会话页上
        message({});
    }
    // 对话框自己转事件循环，不能在核心的退出信号里直接开
    if (code == 3 && clientErrorCode_ == 5 && !closing_) QTimer::singleShot(0, this, &MainWindow::retryWithTotp);
    // 按号码连没连上、问到密码就能再连的几种，取消就停在失败状态：
    // 没给密码而对方收密码、只是没记住这台设备（30）直接问；密码不对、对方换了密码、密码只管一次的会话断了、
    // 对方不再信任这台设备、密码格式不对、对方没开这一种密码（另一种也许能用），带着核心给的原因问。
    // 对方两种密码都没开（21）、不在线（26）这些问了密码也没用，照实显示在会话页
    const bool again = clientErrorCode_ == kPasswordWrong || clientErrorCode_ == kPasswordReplaced || clientErrorCode_ == kPasswordSpent ||
                       clientErrorCode_ == kPasswordForgotten || clientErrorCode_ == kPasswordFormat || clientErrorCode_ == kPasswordKindOff;
    const bool numbered = !pendingNumber_.isEmpty() || !pendingDirect_.host.isEmpty();
    if (code == kExitRejected && (again || clientErrorCode_ == kPasswordNeeded) && numbered && !closing_) {
        QTimer::singleShot(0, this, [this, again] {
            const QString password = askPassword(pendingNumber_.isEmpty() ? pendingAddress_ : groupDigits(pendingNumber_), again);
            if (password.isEmpty()) {
                return;
            }
            if (pendingNumber_.isEmpty()) {
                connectDirect(pendingDirect_, password);
            } else {
                connectToNumber({pendingNumber_, password});
            }
        });
    } else if (trial) {
        // 试连没走到连接那一步，也不是要密码（对方不在线、两种密码都没开、核心没起来）：到会话页看原因
        showPage(1);
    }
}

void MainWindow::hostFinished() {
    clearHostSession();
    hostNumber_.clear();
    hostPassword_.clear();
    hostOffline_ = false;
    hostFixed_.reset();
    hostTempOn_.reset();
    showLocal();
    showFixed();
    showTemp();
    showStatus();
    if (const std::optional<Pairing> pairing = std::exchange(hostRestart_, std::nullopt); pairing && !closing_) {
        startHost(*pairing);
        return;
    }
    if (std::exchange(serviceAfterStop_, false) && !closing_) {
        installService();
        return;
    }
    if (std::exchange(onlineAfterService_, false) && !closing_) {
        goOnline();
    }
    // 托盘图标随主机核心消失：窗口还收着的话就没有入口了
    if (inTray_) restoreFromTray();
}

void MainWindow::clearHostSession() {
    hostPhase_.clear();
    hostGranted_.clear();
    hostTrustDevice_.clear();
    hostStats_->clear();
    pairing_->hide();
    pairingTimer_.stop();
    listSessionPermissions();
    // 界面正等着这个主机的答复（devices / revoke / device-perm、改两种密码的设置）：不会来了。设备那一栏别一直灰着，
    // 下一个主机启动时报的密码设置也不是这次改动的结果
    pairedListing_.clear();
    paired_->parentWidget()->setEnabled(true);
    fixedAsked_ = false;
    tempAsked_ = false;
}

void MainWindow::runHostTool(const QStringList &arguments, const std::function<void(const CoreMessage &)> &report,
                             const std::function<void(int)> &done) {
    runCoreTool(coreProgram(true), arguments, report, done);
}

void MainWindow::runCoreTool(const QString &program, const QStringList &arguments,
                             const std::function<void(const CoreMessage &)> &report, const std::function<void(int)> &done,
                             const QString &input) {
    auto *tool = new CoreProcess(this);
    const auto finish = [tool, done](int code) {
        tool->deleteLater();
        done(code);
    };
    if (report) connect(tool, &CoreProcess::message, this, report);
    if (!input.isEmpty()) {
        connect(tool, &CoreProcess::launched, tool, [tool, input] { tool->send(input); });
    }
    connect(tool, &CoreProcess::finished, this, finish);
    if (!tool->start(program, arguments)) finish(-1);
}

// 导出问两遍（对不上不让过）、至少 8 个字符；导入问一遍。取消返回空
QString MainWindow::askPassphrase(bool twice) {
    // 子控件都在栈上、声明在对话框之后：先于对话框析构，析构时自己从对话框里摘掉
    QDialog dialog(this);
    dialog.setObjectName("passphrasePrompt");
    dialog.setWindowTitle(twice ? tr("导出身份") : tr("导入身份"));
    dialog.setMinimumWidth(kPasswordPromptWidth);
    QVBoxLayout layout(&dialog);
    QLineEdit first;
    QLineEdit again;
    QDialogButtonBox actions(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    const auto field = [&layout](QLineEdit &edit, const QString &accessible) {
        edit.setEchoMode(QLineEdit::Password);
        edit.setAccessibleName(accessible);
        edit.setPlaceholderText(accessible);
        layout.addWidget(&edit);
    };
    first.setObjectName("passphrase");
    field(first, twice ? tr("给导出文件设一个口令（至少 8 个字符）") : tr("导出时设的口令"));
    if (twice) {
        again.setObjectName("passphraseAgain");
        field(again, tr("再输一遍口令"));
    }
    QPushButton *accept = actions.button(QDialogButtonBox::Ok);
    accept->setText(twice ? tr("导出") : tr("导入"));
    accept->setEnabled(false);
    actions.button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout.addWidget(&actions);
    const auto check = [&first, &again, accept, twice] {
        accept->setEnabled(first.text().toUtf8().size() >= kPassphraseMin && (!twice || again.text() == first.text()));
    };
    connect(&first, &QLineEdit::textChanged, accept, check);
    connect(&again, &QLineEdit::textChanged, accept, check);
    connect(&actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    return dialog.exec() == QDialog::Accepted ? first.text() : QString();
}

// 导入要主机停着：它跑着会拿内存里的旧设备表、旧密码设置盖回去。导入前说清楚两台电脑别同时用这份身份
void MainWindow::transferIdentity(bool out) {
    if (!out && host_.busy()) {
        message(tr("先停掉主机再导入身份"));
        return;
    }
    const QString filter = tr("AshDesk 身份 (*.ashid)");
    const QString file = out ? QFileDialog::getSaveFileName(this, tr("导出身份"), QString(), filter)
                             : QFileDialog::getOpenFileName(this, tr("导入身份"), QString(), filter);
    if (file.isEmpty()) {
        return;
    }
    if (!out && QMessageBox::question(this, tr("导入身份"),
                                      tr("导入以后这台电脑用导进来的号码、设备和密钥，原来的改名留底。两台电脑别同时用这份身份：同一个号码会在信令上互相抢。继续？")) !=
                    QMessageBox::Yes) {
        return;
    }
    const QString passphrase = askPassphrase(out);
    if (passphrase.isEmpty()) {
        return;
    }
    auto result = std::make_shared<CoreMessage>();
    runCoreTool(
        coreProgram(false), identityArguments(out, file, dataDirectory_),
        [result](const CoreMessage &reply) {
            if (reply.topic == "identity") {
                *result = reply;
            }
        },
        [this, out, result](int) {
            const QString why = result->value("text");
            if (result->value("ok") == "1") {
                message(out ? tr("身份已导出（%1 个文件）。拿到这个文件和口令的人就能用这台电脑的号码和设备，收好").arg(result->value("n"))
                            : tr("身份已导入（%1 个文件），原来的改名留底").arg(result->value("n")));
            } else {
                message((out ? tr("身份没能导出：%1") : tr("身份没能导入：%1")).arg(why.isEmpty() ? tr("核心没起来") : why));
            }
        },
        passphrase);
}

// 给“最近”列表标在线状态（借鉴设计 M1）：让客户端核心往每个地址发一个发现请求（adrd_client --probe），
// 主机在就回名字和是不是正在被控。列表每次重排、每次回到连接页都重问；上一轮还没回来就不问
void MainWindow::probeRecent() {
    QStringList addresses;
    for (int i = 0; i < recent_->count(); ++i) {
        const QVariant address = recent_->item(i)->data(Qt::UserRole);
        if (address.isValid()) {
            addresses << probeAddress(address.toString());
        }
    }
    if (addresses.isEmpty() || probing_) {
        return;
    }
    probing_ = true;
    runCoreTool(
        coreProgram(false), {"--probe", addresses.mid(0, kProbeLimit).join(','), "--ui-control"},
        [this](const CoreMessage &probe) {
            if (probe.topic == "probe") {
                showRecentStatus(probe);
            }
        },
        [this](int) { probing_ = false; });
}

// known=0：经信令服务问的、服务没回（旧版服务或连不上），不知道主机在不在，不能标成离线
void MainWindow::showRecentStatus(const CoreMessage &probe) {
    const bool online = probe.value("online") == "1";
    const RecentStatus status = probe.value("known") == "0" ? RecentStatus::Unknown
                                : !online                   ? RecentStatus::Offline
                                : probe.value("busy") == "1" ? RecentStatus::Busy
                                                             : RecentStatus::Online;
    const QString text = status == RecentStatus::Unknown   ? tr("信令服务没有回答")
                         : status == RecentStatus::Offline ? tr("离线")
                         : status == RecentStatus::Busy    ? tr("在线，正在被控")
                                                           : tr("在线");
    for (int i = 0; i < recent_->count(); ++i) {
        QListWidgetItem *item = recent_->item(i);
        if (!item->data(Qt::UserRole).isValid() || probeAddress(item->data(Qt::UserRole).toString()) != probe.value("addr")) {
            continue;
        }
        item->setData(kRecentStatusRole, int(status));
        item->setIcon(recentIcon(status));
        // 图标上只有一个点：状态的文字放进提示和读屏描述里
        item->setToolTip(QStringList{item->data(Qt::UserRole).toString(), probe.value("name"), text}.filter(QRegularExpression(".")).join(" · "));
        item->setData(Qt::AccessibleDescriptionRole, text);
    }
}

const QIcon &MainWindow::recentIcon(RecentStatus status) const {
    return status == RecentStatus::Online ? recentOnlineIcon_ : status == RecentStatus::Busy ? recentBusyIcon_ : recentIcon_;
}

void MainWindow::refreshPaired() {
    if (host_.busy()) {
        // 设备表在跑着的主机手里：问它，答复（@ui device … / @ui devices）在 hostMessage 里攒齐了再画
        host_.send(QStringLiteral("devices"));
        return;
    }
    const auto devices = std::make_shared<QList<CoreMessage>>();
    runHostTool(
        {"--devices", "--ui-control"},
        [devices](const CoreMessage &device) {
            if (device.topic == "device") devices->append(device);
        },
        [this, devices](int code) {
            paired_->parentWidget()->setEnabled(true);
            if (code == 0) showPaired(*devices);
            else message(tr("读取已配对设备失败"));
        });
}

void MainWindow::showPaired(const QList<CoreMessage> &devices) {
    while (QLayoutItem *item = paired_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const CoreMessage &device : devices) paired_->addWidget(pairedRow(device));
    if (devices.isEmpty()) paired_->addWidget(label(tr("尚无已配对设备"), "muted"));
}

// 一台已配对设备：名称、配对日期、吊销，下面一排七项权限的勾选；勾选一动就按整排的状态改设备表
QWidget *MainWindow::pairedRow(const CoreMessage &device) {
    const QString id = device.value("id");
    const QString name = device.value("name");
    auto *row = new QFrame;
    row->setObjectName("pairedRow");
    auto *layout = new QVBoxLayout(row);
    layout->setContentsMargins(6, 8, 6, 10);
    layout->setSpacing(6);
    auto *head = new QHBoxLayout;
    head->setSpacing(10);
    head->addWidget(label(name, "pairedName"), 1);
    if (device.value("temp") == "1") head->addWidget(label(tr("临时"), "tag"));
    const QDate paired = QDateTime::fromSecsSinceEpoch(device.value("paired").toLongLong()).date();
    auto *date = label(QLocale().toString(paired, QLocale::ShortFormat), "muted");
    date->setToolTip(tr("配对日期"));
    date->setWordWrap(false);
    head->addWidget(date);
    auto *revoke = button(tr("吊销"), QStringLiteral("revoke-") + id.left(8));
    revoke->setAccessibleName(tr("吊销 %1").arg(name));
    head->addWidget(revoke);
    layout->addLayout(head);
    auto *choices = new FlowLayout(14);
    layout->addLayout(choices);
    const QStringList granted = permissions(device.value("perms"));
    for (const Permission &permission : kPermissions) {
        const QString key = QLatin1String(permission.key);
        const QString text = QCoreApplication::translate("ashdesk", permission.label);
        auto *choice = new QCheckBox(text);
        choice->setObjectName(QStringLiteral("perm-%1-%2").arg(id.left(8), key));
        choice->setAccessibleName(QStringLiteral("%1 · %2").arg(name, text));
        choice->setProperty("permission", key);
        choice->setChecked(granted.contains(key));
        choices->addWidget(choice);
        connect(choice, &QCheckBox::clicked, this, [this, id, row] {
            QStringList keys;
            const QList<QCheckBox *> boxes = row->findChildren<QCheckBox *>();
            for (const QCheckBox *box : boxes)
                if (box->isChecked()) keys << box->property("permission").toString();
            const QString perms = keys.isEmpty()                                       ? QStringLiteral("view")
                                  : keys.size() == qsizetype(std::size(kPermissions)) ? QStringLiteral("all")
                                                                                       : keys.join(',');
            changePaired({"device-perm", id, perms}, tr("设备权限已更新，下次连接生效"), tr("设备权限修改失败"));
        });
    }
    connect(revoke, &QPushButton::clicked, this, [this, id, name] {
        if (confirm(this, tr("吊销设备"), tr("吊销 %1？它之后无法再连接本机。").arg(name)))
            changePaired({"revoke", id}, tr("已吊销 %1").arg(name), tr("吊销失败"));
    });
    return row;
}

// 吊销 / 改权限：说一句结果，再按设备表的实际内容重画这一栏；重画完（refreshPaired）之前这一栏不可点。
// 主机在跑就把命令发给它（答复到了走 pairedChanged），没在跑另起一个带 --revoke / --device-perm 的主机核心
void MainWindow::changePaired(const QStringList &command, const QString &succeeded, const QString &failed) {
    paired_->parentWidget()->setEnabled(false);
    if (host_.busy()) {
        pairedSucceeded_ = succeeded;
        pairedFailed_ = failed;
        host_.send(command.join(u' '));
        return;
    }
    QStringList arguments = command;
    arguments[0].prepend(QStringLiteral("--"));
    runHostTool(arguments, {}, [this, succeeded, failed](int code) {
        message(code == 0 ? succeeded : failed);
        refreshPaired();
    });
}

void MainWindow::pairedChanged(const CoreMessage &result) {
    const QString error = result.value("error");
    // 吊销时设备表存不下来：主机已经把它从内存里的表删了、正连着也断开了，只是重启后会读回旧表
    message(result.value("ok") == "1" ? pairedSucceeded_
            : error == "store"        ? tr("已吊销，但设备表没能保存，主机重启后这台设备会回来")
            : error == "none"         ? tr("设备表里已经没有这台设备")
                                      : pairedFailed_);
    refreshPaired();
}

void MainWindow::runNetdiag() {
    for (QLabel *value : std::as_const(netdiagValues_)) value->setText(QStringLiteral("…"));
    netdiagVerdict_->clear();
    netdiagResults_->show();
    netdiagRunning_ = true;
    updateProcesses();
    const FirewallRule rule = hostFirewallRule(coreProgram(true));
    showDiagItem("firewall", rule == FirewallRule::Allowed,
                 rule == FirewallRule::Allowed   ? tr("已放行")
                 : rule == FirewallRule::Missing ? tr("没有放行主机核心的 AshDesk 规则")
                                                 : tr("无法查询"));
    QStringList arguments{"--netdiag", "--port", QString::number(options_.port), "--ui-control"};
    // 号码连接开着时测的就是它在用的那台（没填就是内置的）；关着时只有填了才测中转
    const QString server = options_.numberHost ? signalServer(options_) : options_.rendezvous;
    if (!server.isEmpty()) {
        arguments << "--rendezvous" << server;
    }
    runHostTool(
        arguments,
        [this](const CoreMessage &diag) {
            if (diag.topic != "diag") return;
            if (diag.value("item") != "verdict") {
                showDiagItem(diag.value("item"), diag.value("ok") == "1", diag.value("value"));
                return;
            }
            const QString verdict = diag.value("value");
            netdiagVerdict_->setText(!diag.value("text").isEmpty() ? diag.value("text")
                                     : verdict == "direct"         ? tr("可以直连")
                                     : verdict == "relay"          ? tr("需要经中转连接")
                                     : verdict == "tcp"            ? tr("只能走 TCP 中转")
                                                                   : tr("无法判断"));
        },
        [this](int) {
            // 核心没报的项（比如没填信令服务器就测不了中转）不留省略号
            for (QLabel *value : std::as_const(netdiagValues_))
                if (value->text() == QStringLiteral("…")) value->setText(QStringLiteral("—"));
            if (netdiagVerdict_->text().isEmpty()) netdiagVerdict_->setText(tr("诊断没有完成"));
            netdiagRunning_ = false;
            updateProcesses();
        });
}

void MainWindow::showDiagItem(const QString &item, bool ok, const QString &value) {
    if (QLabel *target = netdiagValues_.value(item)) target->setText((ok ? QStringLiteral("✓ ") : QStringLiteral("✗ ")) + value);
}

void MainWindow::showStatus() {
    const QString phase = clientPhaseText(clientPhase_, clientPath_);
    clientStatus_->setText(phase.isEmpty() && clientError_.isEmpty() ? client_.status()
                           : phase.isEmpty()                         ? tr("连接失败：%1").arg(clientError_)
                           : clientError_.isEmpty()                  ? phase
                                                                     : phase + QStringLiteral(" · ") + clientError_);
    clientStats_->setVisible(!clientStats_->text().isEmpty());
    const bool controlled = hostPhase_ == "stream";
    hostStatus_->setText(controlled                                     ? tr("被控中 · %1 · %2").arg(hostPeerName_, hostPeer_)
                         : hostPhase_ == "listening"                    ? tr("等待连接")
                         : hostPhase_ == "negotiate"                    ? tr("正在连接")
                         : host_.attached() && serviceAgent_ == "0"     ? tr("系统服务正在拉起主机")
                                                                        : host_.status());
    hostDetail_->setText(controlled ? permissionSummary(hostPerms_) : QString());
    hostDetail_->setVisible(controlled);
    hostStats_->setVisible(!hostStats_->text().isEmpty());
    trustDevice_->setVisible(!hostTrustDevice_.isEmpty());
    if (!tray_) return;
    trayTrust_->setVisible(!hostTrustDevice_.isEmpty());
    trayDisconnect_->setEnabled(controlled);
    trayPermissions_->setEnabled(controlled && !hostGranted_.isEmpty());
    const QList<QAction *> revocable = trayPermissions_->actions();
    for (QAction *action : revocable) action->setChecked(hostPerms_.contains(action->data().toString()));
    tray_->setToolTip(QStringLiteral("AshDesk · ") + hostStatus_->text());
    if (trayControlled_ != controlled) tray_->setIcon(trayIcon(controlled));
    trayControlled_ = controlled;
    tray_->setVisible(host_.busy());
}

bool MainWindow::authenticationReady() {
    if (options_.authentication == 1 && !QFileInfo(options_.keyFile).isFile()) {
        message(tr("请先在设置 → 安全中选择有效的密钥文件。"));
        return false;
    }
    return true;
}

QString MainWindow::coreProgram(bool host) const {
    QString name = host ? "adrd_host" : "adrd_client";
#ifdef Q_OS_WIN
    name += ".exe";
#endif
    return QDir(coreDirectory_).filePath(name);
}

void MainWindow::updateProcesses() {
    const bool clientAvailable = QFileInfo(coreProgram(false)).isExecutable();
    const bool hostAvailable = QFileInfo(coreProgram(true)).isExecutable();
    connect_->setEnabled(clientAvailable && !client_.busy());
    connect_->setToolTip(clientAvailable ? QString() : tr("未安装此平台的远程客户端核心"));
    // 主机归系统服务管时，这里起不了、停不了它，也不能另起一个带配对窗口的
    const bool service = host_.attached();
    const QString serviceTip = service ? tr("主机由系统服务运行") : QString();
    startHost_->setEnabled(hostAvailable && !host_.busy());
    startHost_->setToolTip(service ? serviceTip : hostAvailable ? QString() : tr("未安装此平台的主机核心"));
    pairDevice_->setEnabled(hostAvailable && !closing_ && !service);
    pairTemporary_->setEnabled(hostAvailable && !closing_ && !service);
    pairDevice_->setToolTip(serviceTip);
    pairTemporary_->setToolTip(serviceTip);
    netdiag_->setEnabled(hostAvailable && !netdiagRunning_);
    stopClient_->setEnabled(client_.busy());
    stopHost_->setEnabled(host_.busy() && !service);
    stopHost_->setToolTip(serviceTip);
    hostControl_->setEnabled(!host_.busy());
    numberHost_->setEnabled(!host_.busy());
    connectDevice_->setEnabled(clientAvailable && !client_.busy() && devices_->currentRow() >= 0);
    showStatus();
    showFixed();
    showTemp();
    hostBanner_->setVisible(host_.busy() && !service);
    showService();
    if (closing_ && !client_.busy() && !host_.busy())
        QTimer::singleShot(0, this, [this] { close(); });
}

void MainWindow::refreshDevices() {
    devices_->setRowCount(0);
    const int count = settings_.beginReadArray("devices");
    for (int i = 0; i < count; ++i) {
        settings_.setArrayIndex(i);
        const int row = devices_->rowCount();
        devices_->insertRow(row);
        devices_->setItem(row, 0, new QTableWidgetItem(settings_.value("name").toString()));
        devices_->setItem(row, 1, new QTableWidgetItem(settings_.value("address").toString()));
        devices_->setRowHeight(row, 48);
    }
    settings_.endArray();
    removeDevice_->setEnabled(false);
    connectDevice_->setEnabled(false);
}

void MainWindow::addDevice() {
    QDialog dialog(this);
    dialog.setObjectName("deviceDialog");
    dialog.setWindowTitle(tr("添加设备"));
    dialog.setMinimumWidth(360);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = new QLineEdit;
    name->setObjectName("deviceName");
    auto *address = new QLineEdit;
    address->setObjectName("deviceAddress");
    form->addRow(tr("名称"), name);
    form->addRow(tr("地址"), address);
    layout->addLayout(form);
    auto *error = label({}, "deviceError");
    layout->addWidget(error);
    auto *actions = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    actions->button(QDialogButtonBox::Save)->setText(tr("保存"));
    actions->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    layout->addWidget(actions);
    Endpoint endpoint;
    connect(actions, &QDialogButtonBox::accepted, &dialog, [&] {
        QString reason;
        if (!parseEndpoint(address->text(), options_.port, endpoint, reason)) {
            error->setText(reason);
            address->setFocus();
            return;
        }
        dialog.accept();
    });
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    int row = devices_->rowCount();
    for (int i = 0; i < devices_->rowCount(); ++i)
        if (devices_->item(i, 1)->text() == endpoint.address()) { row = i; break; }
    const int count = qMax(devices_->rowCount(), row + 1);
    settings_.beginWriteArray("devices", count);
    settings_.setArrayIndex(row);
    settings_.setValue("name", name->text().trimmed().isEmpty() ? endpoint.host : name->text().trimmed());
    settings_.setValue("address", endpoint.address());
    settings_.endArray();
    settings_.sync();
    refreshDevices();
    message(settings_.status() == QSettings::NoError ? tr("设备已收藏") : tr("设备保存失败"));
}

void MainWindow::removeDevice() {
    const int selected = devices_->currentRow();
    if (selected < 0) return;
    if (!confirm(this, tr("移除收藏"), tr("移除 %1？不会吊销该设备的远程权限。")
            .arg(devices_->item(selected, 0)->text()))) return;
    settings_.remove("devices");
    settings_.beginWriteArray("devices", devices_->rowCount() - 1);
    int target = 0;
    for (int i = 0; i < devices_->rowCount(); ++i) {
        if (i == selected) continue;
        settings_.setArrayIndex(target++);
        settings_.setValue("name", devices_->item(i, 0)->text());
        settings_.setValue("address", devices_->item(i, 1)->text());
    }
    settings_.endArray();
    settings_.sync();
    refreshDevices();
    message(settings_.status() == QSettings::NoError ? tr("已移除收藏") : tr("设备保存失败"));
}

void MainWindow::refreshRecent() {
    recent_->clear();
    QList<RecentConnection> entries = loadRecent(settings_);
    // 未分组的在前（不加组标题），其余按组名排；组内仍是最近优先
    std::stable_sort(entries.begin(), entries.end(), [](const RecentConnection &a, const RecentConnection &b) {
        return QString::localeAwareCompare(a.group, b.group) < 0;
    });
    QFont headerFont = recent_->font();
    headerFont.setBold(true);
    QString group;
    for (const RecentConnection &entry : entries) {
        if (entry.group != group) {
            group = entry.group;
            auto *header = new QListWidgetItem(group, recent_);
            header->setFlags(Qt::NoItemFlags);
            header->setFont(headerFont);
        }
        auto *item = new QListWidgetItem(recentIcon_, entry.alias.isEmpty() ? entry.address : entry.alias, recent_);
        item->setData(Qt::UserRole, entry.address);
        item->setToolTip(entry.address);
    }
    if (!recent_->count()) {
        auto *item = new QListWidgetItem(tr("尚无连接记录"), recent_);
        item->setFlags(Qt::NoItemFlags);
    }
    probeRecent();
}

void MainWindow::editRecent(bool regroup) {
    const QListWidgetItem *item = recent_->currentItem();
    if (!item || !item->data(Qt::UserRole).isValid()) return;
    const QString address = item->data(Qt::UserRole).toString();
    QList<RecentConnection> entries = loadRecent(settings_);
    const auto entry = std::find_if(entries.begin(), entries.end(),
                                    [&](const RecentConnection &candidate) { return candidate.address == address; });
    if (entry == entries.end()) return;
    bool accepted = false;
    QString value;
    if (regroup) {
        QStringList groups;
        for (const RecentConnection &candidate : entries)
            if (!candidate.group.isEmpty() && !groups.contains(candidate.group)) groups << candidate.group;
        value = QInputDialog::getItem(this, item->text(), tr("分组"), groups, int(groups.indexOf(entry->group)), true,
                                      &accepted);
    } else {
        value = QInputDialog::getText(this, address, tr("别名"), QLineEdit::Normal, entry->alias, &accepted);
    }
    if (!accepted) return;
    (regroup ? entry->group : entry->alias) = value.trimmed();
    saveRecent(settings_, entries);
    refreshRecent();
    for (int i = 0; i < recent_->count(); ++i)
        if (recent_->item(i)->data(Qt::UserRole).toString() == address) recent_->setCurrentRow(i);
    if (settings_.status() != QSettings::NoError) message(tr("保存失败，请检查配置目录权限"));
}

void MainWindow::exportDiagnostics() {
    const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                                  .filePath(QStringLiteral("AshDesk-diagnostics-%1.zip").arg(stamp));
    const QString target = QFileDialog::getSaveFileName(this, tr("打包诊断"), suggested, tr("Zip 压缩包 (*.zip)"));
    if (target.isEmpty()) return;
    QString version = QStringLiteral("AshDesk %1\nQt %2\n%3 %4 %5\n")
                          .arg(QStringLiteral(ASHDESK_VERSION_STRING), QString::fromLatin1(qVersion()),
                               QSysInfo::prettyProductName(), QSysInfo::kernelVersion(),
                               QSysInfo::currentCpuArchitecture());
    for (const bool host : {true, false}) {
        const QFileInfo core(coreProgram(host));
        version += QStringLiteral("%1 %2 %3\n")
                       .arg(core.fileName(), QString::number(core.size()), core.lastModified().toString(Qt::ISODate));
    }
    // 核心在自己的目录里写崩溃 dump；数据目录有审计日志和 caps.json，它的 logs 子目录是界面让核心落盘的日志
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    const QString error = packDiagnostics(
        target,
        {{"core", coreDirectory_}, {"data", dataDirectory_}, {"logs", QDir(dataDirectory_).filePath("logs")}},
        {{"version.txt", version}, {"ui/client.txt", clientLog_->toPlainText()}, {"ui/host.txt", hostLog_->toPlainText()}});
    QGuiApplication::restoreOverrideCursor();
    // 日志脱敏了，崩溃转储没有（可能带着会话临时密钥），主机名、设备名也留着：保存时就说，免得直接贴到公开的地方
    message(error.isEmpty() ? tr("诊断包已保存到 %1。崩溃转储原样放在里面，主机名、设备名也在，分享前先看一眼")
                                  .arg(QDir::toNativeSeparators(target))
                            : error);
}

// 种子由核心生成并用 DPAPI 存在设备表旁边；界面只转述核心打印的 TOTP-URI 一行，不保存
void MainWindow::setupTotp() {
    const QString program = coreProgram(true);
    if (!QFileInfo(program).isExecutable()) {
        message(tr("未安装此平台的主机核心"));
        return;
    }
    const QString store = QDir(dataDirectory_).filePath("totp.dpapi");
    if (QFileInfo::exists(store) && !confirm(this, tr("二次验证 (TOTP)"), tr("换成新密钥？验证器里的旧条目会失效。")))
        return;
    auto *process = new QProcess(this);
    totpSetup_->setEnabled(false);
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        totpSetup_->setEnabled(true);
        QString uri;
        for (const QByteArray &line : process->readAllStandardOutput().split('\n'))
            if (line.startsWith("TOTP-URI ")) uri = QString::fromUtf8(line.mid(9).trimmed());
        if (status == QProcess::NormalExit && code == 0 && uri.startsWith("otpauth://")) showTotp(uri);
        else message(tr("二次验证密钥生成失败"));
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        process->deleteLater();
        totpSetup_->setEnabled(true);
        message(tr("二次验证密钥生成失败"));
    });
    process->setWorkingDirectory(coreDirectory_);
    process->start(program, {QStringLiteral("--totp-setup")});
}

void MainWindow::showTotp(const QString &uri) {
    QString secret = QUrlQuery(QUrl(uri)).queryItemValue("secret");
    for (qsizetype i = 4; i < secret.size(); i += 5) secret.insert(i, ' ');
    QDialog dialog(this);
    dialog.setObjectName("totpDialog");
    dialog.setWindowTitle(tr("二次验证 (TOTP)"));
    dialog.setMinimumWidth(460);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    const auto field = [](const QString &text, const QString &name) {
        auto *edit = new QLineEdit(text);
        edit->setReadOnly(true);
        edit->setCursorPosition(0);
        edit->setAccessibleName(name);
        return edit;
    };
    auto *key = field(secret, tr("密钥"));
    key->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *link = field(uri, tr("链接"));
    auto *copy = button(tr("复制"), "copyTotp");
    copy->setAccessibleName(tr("复制链接"));
    auto *linkRow = new QHBoxLayout;
    linkRow->addWidget(link, 1);
    linkRow->addWidget(copy);
    form->addRow(tr("密钥"), key);
    form->addRow(tr("链接"), linkRow);
    layout->addLayout(form);
    auto *actions = new QDialogButtonBox(QDialogButtonBox::Close);
    actions->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    layout->addWidget(actions);
    connect(copy, &QPushButton::clicked, &dialog, [uri] { QGuiApplication::clipboard()->setText(uri); });
    connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::message(const QString &text) {
    message_->setText(text);
    status_->setVisible(!text.isEmpty());
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (tray_ && host_.busy() && !exiting_ && !closing_) {
        event->ignore();
        inTray_ = true;
        hide();
        return;
    }
    if (client_.busy() || (host_.busy() && !host_.attached())) {
        event->ignore();
        if (!closing_ && confirm(this, tr("退出 AshDesk"), tr("停止本窗口启动的核心进程并退出？"))) {
            closing_ = true;
            client_.stop();
            host_.stop();
            message(tr("等待核心正常清理；不会强制终止进程。"));
        }
        return;
    }
    // 服务拉起的主机不归界面停：断开管道就走
    serviceTimer_.stop();
    host_.stop();
    settings_.setValue("window/geometry", saveGeometry());
    settings_.sync();
    event->accept();
}

} // namespace ashdesk
