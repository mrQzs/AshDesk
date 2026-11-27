#include "core_process.h"
#include "desktop_options.h"
#include "diagnostics.h"
#include "driver_install.h"
#include "firewall.h"
#include "liquid_glass.h"
#include "main_window.h"

#include <gtest/gtest.h>
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkDatagram>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <numeric>
#include <utility>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#endif

namespace {

bool waitUntil(const std::function<bool()> &predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) QTest::qWait(10);
    return predicate();
}

template<typename T>
T *control(QWidget &window, const char *name) {
    auto *widget = window.findChild<T *>(name);
    if (!widget) ADD_FAILURE() << "Missing widget: " << name;
    return widget;
}

// 临时目录里的两份假核心（tests/gtest/fake_core.cpp），文件名和真核心相同，交给 MainWindow 当核心目录
struct FakeCores {
    QTemporaryDir directory;

    FakeCores() {
        const QFileInfo self(QCoreApplication::applicationFilePath());
        const QString suffix = self.suffix().isEmpty() ? QString() : '.' + self.suffix();
        for (const char *name : {"adrd_host", "adrd_client"})
            EXPECT_TRUE(QFile::copy(self.dir().filePath("ashdesk_fake_core" + suffix), directory.filePath(name + suffix)));
    }

    void write(const QString &name, const QByteArray &content) const {
        QFile file(directory.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(content), content.size());
    }

    void remove(const QString &name) const { EXPECT_TRUE(QFile::remove(directory.filePath(name))) << name.toStdString(); }

    // 界面的数据目录（窗口的第三个参数用 directory/data）里放一份共享密钥：按地址连时界面就照旧带 --psk-stored，
    // 不走按号码认（docs/CrossDesk借鉴设计.md CD10）。假核心不读它，内容随便
    void storeKey() const {
        ASSERT_TRUE(QDir(directory.path()).mkpath("data"));
        write("data/psk.dpapi", "key");
    }

    // 假核心每次被拉起时记下的参数，一次一行。界面给“最近”列表问在线状态的那几次（--probe）什么时候来不一定，另算
    QStringList calls() const { return invocations(false); }
    QStringList probes() const { return invocations(true); }

    // 按号码连的假客户端从 stdin 读到的密码（--password -），一次一行
    QStringList typed() const {
        QFile file(directory.filePath("stdin.txt"));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return QString::fromLocal8Bit(file.readAll()).remove('\r').split('\n', Qt::SkipEmptyParts);
    }

    QStringList invocations(bool probes) const {
        QFile file(directory.filePath("calls.txt"));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QStringList result;
        for (const QString &call : QString::fromLocal8Bit(file.readAll()).remove('\r').split('\n', Qt::SkipEmptyParts)) {
            if (call.startsWith("--probe") == probes) {
                result << call;
            }
        }
        return result;
    }
};

// 一个窗口或对话框里给人看、给读屏读的全部文字，托盘菜单和托盘提示也算
QStringList screenTexts(const QWidget &root) {
    QStringList texts{root.windowTitle()};
    for (const auto *widget : root.findChildren<QWidget *>()) texts << widget->accessibleName() << widget->toolTip();
    for (const auto *label : root.findChildren<QLabel *>()) texts << label->text();
    for (const auto *button : root.findChildren<QAbstractButton *>()) texts << button->text();
    for (const auto *line : root.findChildren<QLineEdit *>()) texts << line->placeholderText();
    for (const auto *combo : root.findChildren<QComboBox *>())
        for (int i = 0; i < combo->count(); ++i) texts << combo->itemText(i);
    for (const auto *tabs : root.findChildren<QTabWidget *>())
        for (int i = 0; i < tabs->count(); ++i) texts << tabs->tabText(i);
    for (const auto *list : root.findChildren<QListWidget *>())
        for (int i = 0; i < list->count(); ++i) texts << list->item(i)->text();
    for (const auto *action : root.findChildren<QAction *>()) texts << action->text() << action->toolTip();
    for (const auto *tray : root.findChildren<QSystemTrayIcon *>()) texts << tray->toolTip();
    return texts;
}

// 假核心把收到的每条命令回显成一行 "command …"（进被控端页的日志框）：整条整条取出来，比较时不会把前缀当成命中
QStringList commandsSeen(const QPlainTextEdit &log) {
    const QString marker = QStringLiteral("command ");
    QStringList commands;
    for (const QString &entry : log.toPlainText().split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        if (entry.startsWith(marker)) {
            commands << entry.mid(marker.size());
        }
    }
    return commands;
}

// 语言下拉里的“中文”是语言自己的名字，不算漏译
void expectNoChinese(QStringList texts) {
    texts.removeAll(QStringLiteral("中文"));
    const QRegularExpression chinese(QStringLiteral("[\\x{3000}-\\x{303f}\\x{4e00}-\\x{9fff}\\x{ff00}-\\x{ffef}]"));
    for (const QString &text : texts) EXPECT_FALSE(text.contains(chinese)) << text.toStdString();
}

void addDevice(ashdesk::MainWindow &window, const QString &name, const QString &address, bool checkInvalid = false) {
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        auto *actions = dialog->findChild<QDialogButtonBox *>();
        auto *addressInput = control<QLineEdit>(*dialog, "deviceAddress");
        if (checkInvalid) {
            actions->button(QDialogButtonBox::Save)->click();
            EXPECT_TRUE(dialog->isVisible());
            EXPECT_FALSE(control<QLabel>(*dialog, "deviceError")->text().isEmpty());
        }
        control<QLineEdit>(*dialog, "deviceName")->setText(name);
        addressInput->setText(address);
        actions->button(QDialogButtonBox::Save)->click();
    });
    control<QPushButton>(window, "addDevice")->click();
}

// Review aid only: captures every page in each appearance; restores the light glass look afterwards.
void saveSnapshots(ashdesk::MainWindow &window, const QString &directory) {
    ASSERT_TRUE(QDir().mkpath(directory));
    const struct {
        int theme;
        int material;
        const char *name;
    } looks[]{{1, 2, "light"}, {2, 2, "dark"}, {1, 0, "solid"}};
    for (const auto &look : looks) {
        control<QComboBox>(window, "theme")->setCurrentIndex(look.theme);
        control<QComboBox>(window, "material")->setCurrentIndex(look.material);
        for (int i = 0; i < 5; ++i) {
            control<QPushButton>(window, qPrintable(QString("nav%1").arg(i)))->click();
            QTest::qWait(20);
            EXPECT_TRUE(window.grab().save(QDir(directory).filePath(QString("%1-%2.png").arg(look.name).arg(i))));
        }
        control<QTabWidget>(window, "settingsTabs")->setCurrentIndex(4);
        QTest::qWait(20);
        EXPECT_TRUE(window.grab().save(QDir(directory).filePath(QString("%1-appearance.png").arg(look.name))));
        control<QTabWidget>(window, "settingsTabs")->setCurrentIndex(0);
    }
    control<QComboBox>(window, "theme")->setCurrentIndex(1);
    control<QComboBox>(window, "material")->setCurrentIndex(2);
}

// Review aid only: with ASHDESK_UI_SNAPSHOT_DIR set, saves the current screen (or `dialog`, a dialog of the window)
// in the light and the dark theme.
void snapshot(ashdesk::MainWindow &window, const QString &name, QWidget *dialog = nullptr) {
    const QString directory = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
    if (directory.isEmpty()) return;
    ASSERT_TRUE(QDir().mkpath(directory));
    auto *theme = control<QComboBox>(window, "theme");
    const int chosen = theme->currentIndex();
    const QList<QPair<int, QString>> looks{{1, "light"}, {2, "dark"}};
    for (const auto &[index, look] : looks) {
        theme->setCurrentIndex(index);
        QTest::qWait(80);
        EXPECT_TRUE((dialog ? dialog : &window)->grab().save(QDir(directory).filePath(name + '-' + look + ".png")));
    }
    theme->setCurrentIndex(chosen);
}

// 界面问“要不要装驱动”的对话框是排进事件循环再开的，测试没法在触发它的那一行同步作答。
// 这个对象活着的时候每 20 ms 看一次：对话框出来了就记下上面的文字，按 answers 里轮到的按钮作答，轮完了一律取消
struct DriverPrompts {
    QTimer poller;
    QList<QStringList> seen;

    DriverPrompts(ashdesk::MainWindow &window, const QList<QMessageBox::StandardButton> &answers, const QString &name) {
        QObject::connect(&poller, &QTimer::timeout, &poller, [this, &window, answers, name] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!dialog) return;
            // 存图要转一会儿事件循环，其间定时器不能再进来
            poller.stop();
            seen << screenTexts(*dialog);
            snapshot(window, QStringLiteral("%1-%2").arg(name).arg(seen.size()), dialog);
            QMessageBox::StandardButton answer = answers.value(seen.size() - 1, QMessageBox::Cancel);
            // 界面找得到安装脚本时，点“安装”会真的提权去跑它：测试里绝不能发生，改成取消并报失败
            if (answer == QMessageBox::Yes && !ashdesk::installDirectory().isEmpty()) {
                ADD_FAILURE() << "install.ps1 sits above the test binary; refusing to click Install";
                answer = QMessageBox::Cancel;
            }
            dialog->button(answer)->click();
            poller.start();
        });
        poller.start(20);
    }
};

double percentileMilliseconds(std::vector<qint64> samples, size_t percent = 95) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    return double(samples[samples.size() * percent / 100]) / 1e6;
}

double compositionP95(const ashdesk::GlassStats &stats) {
    const auto count = std::min<size_t>(size_t(stats.compositions), stats.recentCompositionNanoseconds.size());
    return percentileMilliseconds({stats.recentCompositionNanoseconds.begin(),
                            stats.recentCompositionNanoseconds.begin() + qsizetype(count)});
}

} // namespace

// 测试放在匿名命名空间外面：同一个命名空间里函数定义后面跟着没展开的 TEST()，cppcheck 2.13 会给整份文件报 syntaxError
TEST(DesktopEndpoint, ParsesDirectAddressesAndRejectsUrlsOrInvalidPorts) {
    ashdesk::Endpoint endpoint;
    QString error;
    ASSERT_TRUE(ashdesk::parseEndpoint(" 192.0.2.8:44201 ", 44100, endpoint, error));
    EXPECT_EQ(endpoint.address(), "192.0.2.8:44201");
    ASSERT_TRUE(ashdesk::parseEndpoint("[2001:db8::8]:44202", 44100, endpoint, error));
    EXPECT_EQ(endpoint.host, "2001:db8::8");
    EXPECT_EQ(endpoint.port, 44202);
    ASSERT_TRUE(ashdesk::parseEndpoint("::1", 44100, endpoint, error));
    EXPECT_EQ(endpoint.address(), "[::1]:44100");
    ASSERT_TRUE(ashdesk::parseEndpoint("workstation.local", 44100, endpoint, error));
    EXPECT_EQ(endpoint.host, "workstation.local");
    const QStringList invalidInputs{"", "host:0", "host:70000", "https://host", "user@host", "host/path", "host?x=1", "two hosts"};
    for (const QString &invalid : invalidInputs) {
        EXPECT_FALSE(ashdesk::parseEndpoint(invalid, 44100, endpoint, error)) << invalid.toStdString();
        EXPECT_FALSE(error.isEmpty());
    }
}

TEST(DesktopArguments, SettingsReachTheCoreWithoutPlaintextSecretsOrShellExpansion) {
    QTemporaryDir directory;
    ashdesk::DesktopOptions options;
    options.authentication = 1;
    options.keyFile = directory.filePath("key with spaces.txt");
    options.viewOnly = true;
    options.audio = false;
    options.microphone = true;
    options.hostControl = false;
    options.fps = 120;
    const QStringList client = ashdesk::clientArguments({"192.0.2.1", 44201}, options);
    EXPECT_TRUE(client.contains("--ui-control"));
    EXPECT_TRUE(client.contains("--no-input"));
    EXPECT_TRUE(client.contains("--no-audio"));
    EXPECT_TRUE(client.contains("--mic"));
    EXPECT_FALSE(client.contains("--psk"));
    const qsizetype fileIndex = client.indexOf("--psk-file");
    ASSERT_GE(fileIndex, 0);
    EXPECT_EQ(client[fileIndex + 1], QFileInfo(options.keyFile).absoluteFilePath());
    const QStringList host = ashdesk::hostArguments(options);
    EXPECT_FALSE(host.contains("--inject"));
    EXPECT_FALSE(host.contains("--allow-plaintext"));
    EXPECT_TRUE(host.contains("--keep-desktop"));
    EXPECT_EQ(host[host.indexOf("--takeover-default") + 1], "deny");
    EXPECT_EQ(host[host.indexOf("--fps") + 1], "120");
    options.hostControl = true;
    EXPECT_TRUE(ashdesk::hostArguments(options).contains("--inject"));
    // 端口转发默认不开：没填就不带这几个选项
    for (const char *off : {"--forward", "--socks5"}) EXPECT_FALSE(client.contains(off)) << off;
    EXPECT_FALSE(host.contains("--forward-allow"));
    options.forwardAllow = "192.168.1.10:3389,nas.local:445";
    options.forward = "13389=192.168.1.10:3389";
    options.socks5 = 1080;
    const QStringList forwardingHost = ashdesk::hostArguments(options);
    const QStringList forwardingClient = ashdesk::clientArguments({"192.0.2.1", 44201}, options);
    EXPECT_EQ(forwardingHost[forwardingHost.indexOf("--forward-allow") + 1], "192.168.1.10:3389,nas.local:445");
    EXPECT_EQ(forwardingClient[forwardingClient.indexOf("--forward") + 1], "13389=192.168.1.10:3389");
    EXPECT_EQ(forwardingClient[forwardingClient.indexOf("--socks5") + 1], "1080");
    // 客户端窗口上的文字跟界面语言走：中文是客户端的默认，不带选项
    options.language = 1;
    EXPECT_FALSE(ashdesk::clientArguments({"192.0.2.1", 44201}, options).contains("--lang"));
    options.language = 2;
    const QStringList englishClient = ashdesk::clientArguments({"192.0.2.1", 44201}, options);
    EXPECT_EQ(englishClient[englishClient.indexOf("--lang") + 1], "en");
}

TEST(DesktopWidgets, AllPagesRemainReachableWithoutACore) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath("desktop.ini"), QSettings::IniFormat);
    settings.setValue("appearance/theme", 1);
    ashdesk::MainWindow window(settings, directory.path());
    window.show();
    QTest::qWait(30);
    EXPECT_FALSE(control<QPushButton>(window, "connect")->isEnabled());
    EXPECT_FALSE(control<QPushButton>(window, "startHost")->isEnabled());
    for (const char *needsHostCore : {"pairDevice", "pairTemporary", "startNetdiag"})
        EXPECT_FALSE(control<QPushButton>(window, needsHostCore)->isEnabled()) << needsHostCore;
    EXPECT_TRUE(control<QWidget>(window, "pairedSection")->isHidden());
    const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
    for (int i = 0; i < 5; ++i) {
        auto *navigation = control<QPushButton>(window, qPrintable(QString("nav%1").arg(i)));
        QTest::mouseClick(navigation, Qt::LeftButton);
        EXPECT_EQ(control<QStackedWidget>(window, "pages")->currentIndex(), i);
        auto *accessible = QAccessible::queryAccessibleInterface(navigation);
        ASSERT_NE(accessible, nullptr);
        EXPECT_FALSE(accessible->text(QAccessible::Name).isEmpty());
    }
    if (!snapshots.isEmpty()) saveSnapshots(window, snapshots);
    window.resize(640, 480);
    QTest::qWait(30);
    EXPECT_EQ(window.width(), 640);
    EXPECT_EQ(window.height(), 480);
    for (int i = 0; i < 5; ++i) {
        control<QPushButton>(window, qPrintable(QString("nav%1").arg(i)))->click();
        QTest::qWait(10);
        auto *scroll = qobject_cast<QScrollArea *>(control<QStackedWidget>(window, "pages")->currentWidget());
        ASSERT_NE(scroll, nullptr);
        EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0) << "page " << i;
    }
    control<QPushButton>(window, "nav0")->click();
    auto *address = control<QLineEdit>(window, "address");
    QTest::keyClick(address, Qt::Key_Return);
    EXPECT_FALSE(control<QLabel>(window, "message")->text().isEmpty());
    EXPECT_EQ(control<QStackedWidget>(window, "pages")->currentIndex(), 0);
    window.close();
}

TEST(DesktopWidgets, PreferencesSurviveReopeningAndThemeAppliesImmediately) {
    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    {
        QSettings settings(path, QSettings::IniFormat);
        ashdesk::MainWindow window(settings, directory.path());
        control<QComboBox>(window, "theme")->setCurrentIndex(2);
        control<QSpinBox>(window, "port")->setValue(44202);
        control<QSpinBox>(window, "fps")->setValue(120);
        control<QCheckBox>(window, "hostControl")->setChecked(true);
        control<QCheckBox>(window, "files")->setChecked(true);
        control<QLineEdit>(window, "forwardAllow")->setText(" 192.168.1.10:3389, nas.local:445 ");
        control<QSpinBox>(window, "socks5")->setValue(1080);
        EXPECT_TRUE(window.property("darkTheme").toBool());
        window.close();
    }
    QSettings restored(path, QSettings::IniFormat);
    EXPECT_EQ(ashdesk::DesktopOptions::load(restored).forwardAllow, "192.168.1.10:3389,nas.local:445");
    ashdesk::MainWindow reopened(restored, directory.path());
    EXPECT_EQ(control<QLineEdit>(reopened, "forwardAllow")->text(), "192.168.1.10:3389,nas.local:445");
    EXPECT_EQ(control<QSpinBox>(reopened, "socks5")->value(), 1080);
    EXPECT_TRUE(control<QLineEdit>(reopened, "forward")->text().isEmpty());
    EXPECT_EQ(control<QSpinBox>(reopened, "port")->value(), 44202);
    EXPECT_EQ(control<QSpinBox>(reopened, "fps")->value(), 120);
    EXPECT_TRUE(control<QCheckBox>(reopened, "hostControl")->isChecked());
    EXPECT_TRUE(control<QCheckBox>(reopened, "files")->isChecked());
    EXPECT_TRUE(reopened.property("darkTheme").toBool());
}

// “有人来连时”三档（11-26）：默认凭据对了就直接连；以前存的两档设置换过来时，选过“每次都询问”的照旧，原来的默认按新的默认；
// 存过一次以后只留新的键
TEST(DesktopSettings, ConnectConfirmationDefaultsToNoPromptAndOldChoiceCarriesOver) {
    const QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    {
        QSettings fresh(path, QSettings::IniFormat);
        EXPECT_EQ(ashdesk::DesktopOptions::load(fresh).confirm, 0);
        fresh.setValue("security/confirm", 1);
        EXPECT_EQ(ashdesk::DesktopOptions::load(fresh).confirm, 2);
        fresh.setValue("security/confirm", 0);
        EXPECT_EQ(ashdesk::DesktopOptions::load(fresh).confirm, 0);
        ashdesk::DesktopOptions options = ashdesk::DesktopOptions::load(fresh);
        options.confirm = 1;
        options.save(fresh);
        EXPECT_FALSE(fresh.contains("security/confirm"));
    }
    QSettings restored(path, QSettings::IniFormat);
    EXPECT_EQ(ashdesk::DesktopOptions::load(restored).confirm, 1);
    ashdesk::MainWindow window(restored, directory.path());
    auto *box = control<QComboBox>(window, "confirm");
    EXPECT_EQ(box->count(), 3);
    EXPECT_EQ(box->currentIndex(), 1);
}

TEST(DesktopSettings, UnreadableFileIsKeptAsBackupAndDefaultsAreUsed) {
    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    const QList<QByteArray> unreadable{"[appearance\ntheme=2\n", "[meta]\nversion=99\n[appearance]\ntheme=2\n",
                                       "[meta]\nversion=two\n[appearance]\ntheme=2\n"};
    for (const QByteArray &content : unreadable) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(content);
        file.close();
        const QString notice = ashdesk::prepareSettings(path);
        const QStringList backups = QDir(directory.path()).entryList({"desktop.ini.bad-*"});
        ASSERT_EQ(backups.size(), 1) << content.toStdString();
        EXPECT_TRUE(notice.contains(backups[0])) << notice.toStdString();
        QFile backup(QDir(directory.path()).filePath(backups[0]));
        ASSERT_TRUE(backup.open(QIODevice::ReadOnly));
        EXPECT_EQ(backup.readAll(), content);
        backup.close();
        ASSERT_TRUE(backup.remove());
        QSettings settings(path, QSettings::IniFormat);
        EXPECT_EQ(settings.status(), QSettings::NoError);
        ashdesk::MainWindow window(settings, directory.path());
        window.message(notice);
        EXPECT_EQ(control<QComboBox>(window, "theme")->currentIndex(), 0);
        EXPECT_EQ(control<QLabel>(window, "message")->text(), notice);
    }
}

TEST(DesktopSettings, UnversionedFileLoadsAndGainsTheCurrentVersion) {
    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    {
        QSettings legacy(path, QSettings::IniFormat);
        legacy.setValue("appearance/theme", 2);
        legacy.setValue("quality/fps", 90);
    }
    EXPECT_TRUE(ashdesk::prepareSettings(path).isEmpty());
    EXPECT_TRUE(QDir(directory.path()).entryList({"desktop.ini.bad-*"}).isEmpty());
    QSettings settings(path, QSettings::IniFormat);
    EXPECT_EQ(settings.value("meta/version").toInt(), ashdesk::kSettingsVersion);
    const ashdesk::DesktopOptions options = ashdesk::DesktopOptions::load(settings);
    EXPECT_EQ(options.theme, 2);
    EXPECT_EQ(options.fps, 90);
}

TEST(DesktopRecent, NewConnectionsMoveToTopWithoutDroppingNamedEntries) {
    QList<ashdesk::RecentConnection> recent{{"192.0.2.200:44100", "NAS", {}}, {"192.0.2.201:44100", {}, "Office"}};
    for (int i = 0; i < 20; ++i) ashdesk::rememberRecent(recent, QString("192.0.2.%1:44100").arg(i));
    ASSERT_EQ(recent.size(), ashdesk::kRecentLimit + 2);
    EXPECT_EQ(recent[0].address, "192.0.2.19:44100");
    EXPECT_EQ(recent[ashdesk::kRecentLimit].alias, "NAS");
    ashdesk::rememberRecent(recent, "192.0.2.201:44100");
    EXPECT_EQ(recent.size(), ashdesk::kRecentLimit + 2);
    EXPECT_EQ(recent[0].group, "Office");
}

TEST(DesktopRecent, EachAddressShowsWhetherItsHostIsOnline) {
    const FakeCores cores;
    cores.write("probe.txt", "@ui probe addr=192.0.2.1:44100 online=1 name=NAS busy=0\n"
                             "@ui probe addr=192.0.2.2:44100 online=1 name=Office\\sPC busy=1\n"
                             "@ui probe addr=192.0.2.3:44100 online=0\n"
                             "@ui probe addr=pc@rdv.example:44200 online=1 busy=0\n"
                             "@ui probe addr=nas@rdv.example:44200 online=0 known=0\n");
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::saveRecent(settings, {{"192.0.2.1:44100", {}, {}}, {"192.0.2.2:44100", {}, {}}, {"192.0.2.3:44100", {}, {}},
                                   {"pc@rdv.example:44200", {}, {}}, {"nas@rdv.example:44200", {}, {}}});
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *list = control<QListWidget>(window, "recent");
    ASSERT_EQ(list->count(), 5);
    const auto described = [&](int row) { return list->item(row)->data(Qt::AccessibleDescriptionRole).toString(); };
    ASSERT_TRUE(waitUntil([&] { return !described(4).isEmpty(); }));

    // 问的是列表里的全部地址，一次问完；经信令服务的（名字@服务）也在里面
    const QStringList probes = cores.probes();
    ASSERT_FALSE(probes.isEmpty());
    EXPECT_EQ(probes[0], "--probe 192.0.2.1:44100,192.0.2.2:44100,192.0.2.3:44100,pc@rdv.example:44200,nas@rdv.example:44200 --ui-control");
    // 图标上只多一个点，文字在提示和读屏描述里
    EXPECT_EQ(described(0), QStringLiteral("在线"));
    EXPECT_EQ(described(1), QStringLiteral("在线，正在被控"));
    EXPECT_EQ(described(2), QStringLiteral("离线"));
    EXPECT_EQ(described(3), QStringLiteral("在线"));
    EXPECT_EQ(described(4), QStringLiteral("信令服务没有回答"));
    EXPECT_EQ(list->item(0)->toolTip(), QStringLiteral("192.0.2.1:44100 · NAS · 在线"));
    EXPECT_EQ(list->item(1)->toolTip(), QStringLiteral("192.0.2.2:44100 · Office PC · 在线，正在被控"));
    EXPECT_EQ(list->item(2)->toolTip(), QStringLiteral("192.0.2.3:44100 · 离线"));
    EXPECT_EQ(list->item(3)->toolTip(), QStringLiteral("pc@rdv.example:44200 · 在线"));
    EXPECT_EQ(list->item(4)->toolTip(), QStringLiteral("nas@rdv.example:44200 · 信令服务没有回答"));
    const auto pixels = [&](int row) { return list->item(row)->icon().pixmap(list->iconSize()).toImage(); };
    EXPECT_NE(pixels(0), pixels(2));
    EXPECT_NE(pixels(0), pixels(1));
    EXPECT_EQ(pixels(3), pixels(0));
    EXPECT_EQ(pixels(4), pixels(2));                 // 不知道和离线一样不带点：没有点就是没说在线

    // 换主题重画图标，状态还在
    control<QComboBox>(window, "theme")->setCurrentIndex(2);
    EXPECT_NE(pixels(0), pixels(2));
    EXPECT_NE(pixels(0), pixels(1));
    EXPECT_EQ(described(1), QStringLiteral("在线，正在被控"));
}

TEST(DesktopRecent, OldListMigratesAndAliasesAndGroupsPersist) {
    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    {
        QSettings legacy(path, QSettings::IniFormat);
        legacy.setValue("connections/recent", QStringList{"192.0.2.1:44100", "[2001:db8::2]:44201", "192.0.2.3:44100"});
    }
    ASSERT_TRUE(ashdesk::prepareSettings(path).isEmpty());
    const auto texts = [](const QListWidget *list) {
        QStringList result;
        for (int i = 0; i < list->count(); ++i) result << list->item(i)->text();
        return result;
    };
    const auto answer = [](QWidget &window, const QString &text) {
        QTimer::singleShot(0, &window, [text] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            dialog->setTextValue(text);
            dialog->accept();
        });
    };
    {
        QSettings settings(path, QSettings::IniFormat);
        EXPECT_FALSE(settings.contains("connections/recent"));
        ashdesk::MainWindow window(settings, directory.path());
        window.show();
        auto *list = control<QListWidget>(window, "recent");
        ASSERT_EQ(texts(list), QStringList({"192.0.2.1:44100", "[2001:db8::2]:44201", "192.0.2.3:44100"}));
        auto *rename = window.findChild<QAction *>("renameRecent");
        auto *regroup = window.findChild<QAction *>("groupRecent");
        ASSERT_NE(rename, nullptr);
        ASSERT_NE(regroup, nullptr);
        EXPECT_FALSE(rename->isEnabled());
        list->setCurrentRow(1);
        answer(window, " Laptop ");
        rename->trigger();
        list->setCurrentRow(2);
        answer(window, QStringLiteral("家里"));
        regroup->trigger();
        list->setCurrentRow(0);
        answer(window, QStringLiteral("家里"));
        regroup->trigger();
        const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            QTest::qWait(50);
            EXPECT_TRUE(QDir().mkpath(snapshots));
            EXPECT_TRUE(window.grab().save(QDir(snapshots).filePath("recent.png")));
        }
        window.close();
    }
    QSettings settings(path, QSettings::IniFormat);
    ashdesk::MainWindow reopened(settings, directory.path());
    auto *list = control<QListWidget>(reopened, "recent");
    ASSERT_EQ(texts(list), QStringList({"Laptop", QStringLiteral("家里"), "192.0.2.1:44100", "192.0.2.3:44100"}));
    EXPECT_EQ(list->item(0)->toolTip(), "[2001:db8::2]:44201");
    EXPECT_EQ(list->item(1)->flags(), Qt::NoItemFlags);
    const QList<ashdesk::RecentConnection> recent = ashdesk::loadRecent(settings);
    ASSERT_EQ(recent.size(), 3);
    EXPECT_EQ(recent[1].alias, "Laptop");
    EXPECT_EQ(recent[2].group, QStringLiteral("家里"));
}

TEST(DesktopDiagnostics, ArchiveHoldsRecentDumpsLogsAndCapsWithoutSecrets) {
    QTemporaryDir directory;
    const QDir root(directory.path());
    ASSERT_TRUE(root.mkpath("core") && root.mkpath("data") && root.mkpath("out"));
    const QDateTime now = QDateTime::currentDateTime();
    const auto write = [&](const QString &name, const QByteArray &data, int age) {
        QFile file(root.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(data), data.size());
        // 先刷出 Qt 的缓冲再改时间：macOS 上缓冲留到关文件时才写，一写修改时间又成了现在（Windows 不会）
        ASSERT_TRUE(file.flush());
        ASSERT_TRUE(file.setFileTime(now.addSecs(-age), QFileDevice::FileModificationTime));
    };
    const QByteArray key = "00112233445566778899aabbccddeeff00112233445566778899AABBCCDDEEFF";
    for (int i = 0; i < 7; ++i)
        write(QString("core/adrd_host-crash-2026010%1-000000.dmp").arg(i), QByteArray("MDMP\0\x01", 6) + char(i), 70 - i);
    write("core/adrd_host.exe", "MZ", 0);
    write("core/host.log",
          "命令行：adrd_client.exe --pair 135792 --host 192.0.2.1 --psk " + key +
              "\n配对码：135 792（10 分钟内有效）\n密钥 0x" + key + "\nadrd_host --pair --port 44100\n",
          5);
    write("core/big.log", "head-marker\n" + QByteArray(ashdesk::kDiagnosticLogTail + 1024, 'x') + "\ntail-marker\n", 4);
    write("core/net.log",
          "12:34:56.789 listen 0.0.0.0:44100 and [::]:44100\npeer=192.168.1.23:50000 relay=203.0.113.9:3478 lo=127.0.0.1\n"
          "v6 [fe80::1%12]:44100 [::1]:44100 fd00::1 2001:db8::8 ::ffff:10.0.0.5\nNVIDIA 32.0.15.6094 os 10.0.26200.1\n",
          4);
    write("data/audit.log", "SESSION_START\tpeer=192.0.2.1:50000 name=笔记本\n", 3);
    write("data/devices.dpapi", key, 2);
    write("data/psk.dpapi", key, 2);
    write("data/caps.json", R"({"adapters":[]})", 1);
    const QString target = root.filePath(QStringLiteral("out/诊断.zip"));
    ASSERT_EQ(ashdesk::packDiagnostics(target, {{"core", root.filePath("core")}, {"data", root.filePath("data")}},
                                       {{"version.txt", "AshDesk test"},
                                        {"ui/host.txt", "配对码：246 801\npeer=[2001:db8::7]:50000 name=Desk"}}),
              QString());
    const auto tar = [](const QStringList &arguments) {
        QProcess process;
        process.start(ashdesk::zipTool(), arguments);
        EXPECT_TRUE(process.waitForFinished(30000));
        EXPECT_EQ(process.exitCode(), 0);
        return QString::fromUtf8(process.readAllStandardOutput());
    };
    QStringList entries = tar({"-tf", target}).split('\n', Qt::SkipEmptyParts);
    for (QString &entry : entries) entry = entry.trimmed();
    entries.sort();
    QStringList expected{"version.txt", "ui/host.txt", "core/host.log", "core/big.log", "core/net.log", "data/audit.log",
                         "data/caps.json"};
    for (int i = 2; i < 7; ++i) expected << QString("core/adrd_host-crash-2026010%1-000000.dmp").arg(i);
    expected.sort();
    EXPECT_EQ(entries, expected);
    tar({"-xf", target, "-C", root.filePath("out")});
    const auto read = [&root](const QString &name) {
        QFile file(root.filePath("out/" + name));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << name.toStdString();
        return file.readAll();
    };
    for (const QString &entry : expected) {
        const QByteArray content = read(entry).toLower();
        EXPECT_FALSE(content.contains(key.left(32).toLower())) << entry.toStdString();
        for (const char *code : {"135792", "135 792", "246 801"})
            EXPECT_FALSE(content.contains(code)) << entry.toStdString();
        for (const char *address : {"192.0.2.1", "192.168.1.23", "203.0.113.9", "127.0.0.1", "fe80::1", "::1]", "fd00::1",
                                    "2001:db8", "10.0.0.5"})
            EXPECT_FALSE(content.contains(address)) << entry.toStdString() << " " << address;
    }
    EXPECT_EQ(read("core/adrd_host-crash-20260106-000000.dmp"), QByteArray("MDMP\0\x01\x06", 7));
    EXPECT_EQ(read("data/caps.json"), R"({"adapters":[]})");
    EXPECT_TRUE(read("core/host.log").contains("--psk <已隐藏>"));
    EXPECT_TRUE(read("core/host.log").contains("--host <ipv4-public> "));
    EXPECT_TRUE(read("core/host.log").contains("adrd_host --pair --port 44100"));
    EXPECT_TRUE(read("data/audit.log").contains("peer=<ipv4-public>:50000 name=笔记本"));
    EXPECT_TRUE(read("ui/host.txt").contains("peer=[<ipv6-public>]:50000 name=Desk"));
    // 地址只留类别；未指定地址、时间和版本号这些长得像地址的不动
    EXPECT_EQ(read("core/net.log"),
              "12:34:56.789 listen 0.0.0.0:44100 and [::]:44100\n"
              "peer=<ipv4-private>:50000 relay=<ipv4-public>:3478 lo=<ipv4-loopback>\n"
              "v6 [<ipv6-link-local>%12]:44100 [<ipv6-loopback>]:44100 <ipv6-private> <ipv6-public> <ipv6-private>\n"
              "NVIDIA 32.0.15.6094 os 10.0.26200.1\n");
    const QByteArray big = read("core/big.log");
    EXPECT_LE(big.size(), ashdesk::kDiagnosticLogTail);
    EXPECT_TRUE(big.endsWith("tail-marker\n"));
    EXPECT_FALSE(big.contains("head-marker"));
}

TEST(DesktopDiagnostics, CoreLogRotatesOnceItPassesTheLimitAndKeepsTheNewestFew) {
    QTemporaryDir directory;
    const QDir logs(directory.filePath("data/logs"));
    const QString current = ashdesk::coreLogFile(logs.path(), "host");
    EXPECT_EQ(current, logs.filePath("host.log"));
    EXPECT_TRUE(logs.exists());
    const auto fill = [&](char marker, qint64 size) {
        QFile file(current);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(QByteArray(size, marker)), size);
    };
    const auto marker = [&](const QString &name) {
        QFile file(logs.filePath(name));
        return file.open(QIODevice::ReadOnly) ? file.read(1) : QByteArray();
    };
    fill('a', ashdesk::kCoreLogLimit);
    EXPECT_EQ(ashdesk::coreLogFile(logs.path(), "host"), current);
    EXPECT_EQ(logs.entryList(QDir::Files), QStringList({"host.log"}));
    // 写满五轮：最新的三份留作 .1 .2 .3，更早的两份删掉；别的核心的日志不受影响
    fill('z', 16);
    QFile::rename(current, logs.filePath("client.log"));
    for (const char round : {'1', '2', '3', '4', '5'}) {
        fill(round, ashdesk::kCoreLogLimit + 1);
        EXPECT_EQ(ashdesk::coreLogFile(logs.path(), "host"), current);
        EXPECT_FALSE(QFile::exists(current));
    }
    EXPECT_EQ(logs.entryList(QDir::Files, QDir::Name), QStringList({"client.log", "host.1.log", "host.2.log", "host.3.log"}));
    EXPECT_EQ(marker("host.1.log"), "5");
    EXPECT_EQ(marker("host.2.log"), "4");
    EXPECT_EQ(marker("host.3.log"), "3");
    EXPECT_EQ(marker("client.log"), "z");
}

TEST(DesktopLanguage, EnglishLeavesNoChineseOnScreenAndTheChoicePersists) {
    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    ashdesk::useLanguage(2);
    {
        QSettings settings(path, QSettings::IniFormat);
        ashdesk::MainWindow window(settings, directory.path());
        window.show();
        QTest::qWait(30);
        QStringList texts = screenTexts(window);
        auto *devices = control<QTableWidget>(window, "devices");
        for (int i = 0; i < devices->columnCount(); ++i) texts << devices->horizontalHeaderItem(i)->text();
        expectNoChinese(texts);
        const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) saveSnapshots(window, QDir(snapshots).filePath("en"));
        control<QComboBox>(window, "language")->setCurrentIndex(2);
        EXPECT_EQ(control<QLabel>(window, "message")->text(), "The language changes after AshDesk restarts");
    }
    ashdesk::useLanguage(1);
    QSettings restored(path, QSettings::IniFormat);
    ashdesk::MainWindow reopened(restored, directory.path());
    EXPECT_EQ(control<QComboBox>(reopened, "language")->currentIndex(), 2);
    EXPECT_EQ(reopened.windowTitle(), QStringLiteral("AshDesk · 极帧"));
}

TEST(DesktopLanguage, EnglishCoversLiveStatusTrayPairedDevicesDiagnosticsAndDialogs) {
    const struct English {
        English() { ashdesk::useLanguage(2); }
        ~English() { ashdesk::useLanguage(1); }
    } english;
    const FakeCores cores;
    cores.write("devices.txt", "@ui device id=00112233445566778899aabbccddeeff name=Laptop perms=control,files "
                               "paired=1762000000 temp=1\n");
    cores.write("totp.txt", "246810");
    cores.storeKey();                            // 按地址连走共享密钥（没存过的走号码，见 DesktopDirect）
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    QStringList texts;
    const auto collect = [&] { texts << screenTexts(window); };
    const auto answer = [&](QMessageBox::StandardButton button) {
        QTimer::singleShot(0, &window, [&texts, button] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            texts << screenTexts(*dialog);
            dialog->button(button)->click();
        });
    };

    auto *client = control<ashdesk::CoreProcess>(window, "clientCore");
    for (const char *phase : {"lookup", "handshake", "negotiate", "configure"}) {
        client->consume(QByteArray("@ui phase=") + phase + " path=direct secure=1 reconnects=0\n");
        collect();
    }
    for (const char *path : {"direct", "relay", "tcp"}) {
        client->consume(QByteArray("@ui phase=stream path=") + path + " secure=1 reconnects=0\n");
        collect();
    }
    client->consume("@ui stats fps=59.8 lat_ms=4.2 rtt_ms=1.6 mbps=0.10 loss=0.0 path=direct enc=1 cap=4 perms=127 state=0\n");
    EXPECT_EQ(control<QLabel>(window, "clientStats")->text(), "59.8 fps · 0.10 Mb/s · RTT 1.6 ms · loss 0.0% · NVENC · Test pattern");
    control<QPushButton>(window, "nav1")->click();
    snapshot(window, "en-session");

    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    for (const char *report : {"@ui phase=listening\n", "@ui phase=negotiate\n",
                               "@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control,files,clipboard,audio,mic,curtain,forward\n",
                               "@ui stats fps=60 mbps=35.29 rtt_ms=0.2 loss=0.0 path=direct enc=1 cap=1 perms=view curtain=0 blocked=0\n",
                               "@ui pair code=135792 ttl=600 temp=0\n", "@ui pair code=246801 ttl=600 temp=1\n",
                               "@ui pair done name=Laptop id=00112233 temp=1\n"}) {
        host->consume(report);
        collect();
    }
    EXPECT_EQ(control<QLabel>(window, "message")->text(), "Paired: Laptop");
    if (auto *permissions = window.findChild<QMenu *>("trayPermissions")) {
        EXPECT_EQ(permissions->actions().size(), 7);
    }
    host->consume("@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control,files,clipboard,audio,mic,curtain,forward\n"
                  "@ui stats fps=60 mbps=35.29 rtt_ms=0.2 loss=0.0 path=direct enc=1 cap=1 perms=control,files curtain=0 blocked=0\n"
                  "@ui pair code=135792 ttl=600 temp=1\n");
    control<QPushButton>(window, "nav2")->click();
    snapshot(window, "en-host-pairing");
    host->consume("@ui pair closed reason=expired\n@ui phase=listening\n");

    // N4：信任这台设备（入口、确认框、成了和没成的各句），固定密码的各句，对方信任了本机
    host->consume("@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control trust=1 dev=00112233445566778899aabbccddeeff\n");
    EXPECT_FALSE(control<QPushButton>(window, "trustDevice")->isHidden());
    collect();
    snapshot(window, "en-host-trust");
    answer(QMessageBox::Cancel);
    control<QPushButton>(window, "trustDevice")->click();
    for (const char *report : {"@ui trusted ok=1 name=Laptop id=00112233\n", "@ui trusted ok=0 error=none\n", "@ui trusted ok=0 error=changed\n",
                               "@ui trusted ok=0 error=old-client\n", "@ui trusted ok=0 error=store\n", "@ui trusted ok=0 error=later\n",
                               "@ui fixed set=0\n", "@ui fixed set=0 error=weak\n", "@ui fixed set=0 error=store\n",
                               "@ui fixed set=0 error=off\n", "@ui fixed set=0 error=renumbered\n", "@ui fixed set=0 error=locked\n"}) {
        host->consume(report);
        collect();
    }
    EXPECT_EQ(control<QLabel>(window, "message")->text(), "The fixed password was not changed (locked)");
    control<QLineEdit>(window, "fixedPassword")->setText("Summer-2026");
    control<QPushButton>(window, "setFixed")->click();
    host->consume("@ui fixed set=1\n");
    EXPECT_EQ(control<QLabel>(window, "message")->text(), "Fixed password set");
    collect();
    control<QPushButton>(window, "clearFixed")->click();
    host->consume("@ui fixed set=0\n");
    collect();
    // 临时密码的两个开关和各句：关了而固定密码也没设时的提醒，存不下来、认不得的原因、主机没开连接密码
    host->consume("@ui temp on=1 keep=0\n");
    collect();
    control<QCheckBox>(window, "tempPassword")->click();
    host->consume("@ui temp on=0 keep=0\n");
    EXPECT_EQ(control<QLabel>(window, "message")->text(),
              "The one-time password is off and no fixed password is set: only trusted devices can connect by number now");
    collect();
    for (const char *report : {"@ui temp on=0 keep=0 error=store\n", "@ui temp on=0 keep=0 error=locked\n", "@ui temp on=1 keep=0 error=off\n"}) {
        host->consume(report);
        collect();
    }
    host->consume("@ui phase=listening\n");
    for (const char *report : {"@ui remembered=1\n", "@ui remembered=0\n"}) {
        client->consume(report);
        collect();
    }

    // 已配对设备：列表、改权限、吊销确认、空列表；窄窗口里权限勾选折行，不撑出水平滚动
    control<QPushButton>(window, "nav3")->click();
    auto *paired = control<QWidget>(window, "paired");
    const auto scrollsSideways = [&] {
        auto *scroll = qobject_cast<QScrollArea *>(control<QStackedWidget>(window, "pages")->currentWidget());
        return !scroll || scroll->horizontalScrollBar()->maximum() > 0;
    };
    ASSERT_TRUE(waitUntil([&] { return window.findChild<QCheckBox *>("perm-00112233-audio") != nullptr; }));
    window.resize(640, 480);
    QTest::qWait(30);
    EXPECT_FALSE(scrollsSideways());
    collect();
    snapshot(window, "en-paired-small");
    window.findChild<QCheckBox *>("perm-00112233-audio")->click();
    ASSERT_TRUE(waitUntil([&] { return paired->isEnabled(); }));
    collect();
    cores.write("devices.txt", {});
    answer(QMessageBox::Yes);
    control<QPushButton>(window, "revoke-00112233")->click();
    ASSERT_TRUE(waitUntil([&] { return paired->isEnabled(); }));
    EXPECT_EQ(control<QLabel>(window, "message")->text(), "Revoked Laptop");
    collect();
    // 在跑的主机对吊销的另外两种答复（N4）
    for (const char *report : {"@ui revoked ok=0 id=00112233 error=store\n", "@ui revoked ok=0 error=none\n"}) {
        host->consume(report);
        collect();
    }
    for (int i = 0; i < 5; ++i) {
        control<QPushButton>(window, qPrintable(QString("nav%1").arg(i)))->click();
        QTest::qWait(10);
        EXPECT_FALSE(scrollsSideways()) << "page " << i;
    }

    // 网络诊断：核心不带说明文字时界面自己给结论；没给结论就退出也要有话说
    control<QPushButton>(window, "nav4")->click();
    auto *tabs = control<QTabWidget>(window, "settingsTabs");
    tabs->setCurrentIndex(tabs->count() - 1);
    auto *netdiag = control<QPushButton>(window, "startNetdiag");
    for (const char *verdict : {"direct", "relay", "tcp", "unknown", ""}) {
        cores.write("netdiag.txt", *verdict ? QByteArray("@ui diag item=nat ok=1 value=cone\n@ui diag item=verdict value=") + verdict + "\n"
                                            : QByteArray());
        netdiag->click();
        ASSERT_TRUE(waitUntil([&] { return netdiag->isEnabled(); }));
        EXPECT_FALSE(control<QLabel>(window, "netdiagVerdict")->text().isEmpty());
        collect();
    }
    snapshot(window, "en-netdiag-small");
    tabs->setCurrentIndex(3);
    snapshot(window, "en-security-small");
    tabs->setCurrentIndex(2);
    snapshot(window, "en-network-small");

    // 驱动没装的询问（两种驱动各一句）：取消；点安装而测试环境里没有安装脚本，状态栏说要跑什么
    {
        DriverPrompts prompts(window, {QMessageBox::Cancel, QMessageBox::Yes}, "en-driver-prompt");
        host->consume("@ui need driver=vdisplay\n");
        ASSERT_TRUE(waitUntil([&] { return prompts.seen.size() == 1; }));
        host->consume("@ui need driver=vaudio\n");
        ASSERT_TRUE(waitUntil([&] { return prompts.seen.size() == 2; }));
        EXPECT_EQ(control<QLabel>(window, "message")->text(),
                  "Install script not found. Run install.ps1 -Drivers vaudio from the install package in an elevated PowerShell");
        for (const QStringList &prompt : std::as_const(prompts.seen)) texts << prompt;
        collect();
        QTest::qWait(30);
        EXPECT_FALSE(scrollsSideways());
        // 不用 snapshot()：它切主题会存一次设置，状态栏的话就被“设置已保存”换掉了
        const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            EXPECT_TRUE(window.grab().save(QDir(snapshots).filePath("en-driver-no-script-small.png")));
        }
    }

    // 二次验证：主机没生成密钥的提示；客户端被要求输码的对话框，取消后的失败状态
    control<QComboBox>(window, "totp")->setCurrentIndex(1);
    control<QPushButton>(window, "startHost")->click();
    collect();
    bool prompted = false;
    QTimer poller;
    QObject::connect(&poller, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("totpPrompt");
        if (!dialog || !dialog->isVisible()) return;
        prompted = true;
        texts << screenTexts(*dialog);
        const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            EXPECT_TRUE(dialog->grab().save(QDir(snapshots).filePath("en-totp-prompt.png")));
        }
        dialog->reject();
    });
    poller.start(20);
    control<QLineEdit>(window, "address")->setText("192.0.2.9:44100");
    control<QPushButton>(window, "connect")->click();
    ASSERT_TRUE(waitUntil([&] { return prompted && !client->busy(); }));
    EXPECT_EQ(control<QLabel>(window, "clientStatus")->text(), "Connection failed: totp required");
    collect();

    // 按号码连：对方不认得这台设备，核心说要密码，弹密码框；取消后的失败状态
    bool asked = false;
    QTimer passwordPoller;
    QObject::connect(&passwordPoller, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("passwordPrompt");
        if (dialog == nullptr || !dialog->isVisible()) {
            return;
        }
        asked = true;
        texts << screenTexts(*dialog);
        const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            EXPECT_TRUE(dialog->grab().save(QDir(snapshots).filePath("en-password-prompt.png")));
        }
        dialog->reject();
    });
    constexpr int kPromptPollMs = 20;
    passwordPoller.start(kPromptPollMs);
    control<QLineEdit>(window, "address")->setText("987 654 321");
    control<QPushButton>(window, "connect")->click();
    EXPECT_EQ(control<QLabel>(window, "message")->text(), "Looking up the other computer…");
    collect();
    ASSERT_TRUE(waitUntil([&] { return asked && !client->busy(); }));
    EXPECT_EQ(control<QLabel>(window, "clientStatus")->text(), "Connection failed: Enter the other computer's connection password");
    collect();

    // 被控中再配对的确认框；会话结束后收尾
    control<QComboBox>(window, "totp")->setCurrentIndex(0);
    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil([&] { return control<QLabel>(window, "hostStatus")->text() == "Waiting for a connection"; }));
    host->consume("@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control\n");
    answer(QMessageBox::Cancel);
    control<QPushButton>(window, "pairTemporary")->click();
    collect();
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    collect();
    expectNoChinese(texts);
    window.close();
}

TEST(DesktopWidgets, DeviceValidationUpdateAndRemovalPersistWithoutTruncatingOtherRows) {
    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    {
        QSettings settings(path, QSettings::IniFormat);
        ashdesk::MainWindow window(settings, directory.path());
        window.show();
        control<QPushButton>(window, "nav3")->click();
        addDevice(window, "Workstation", "192.0.2.1", true);
        addDevice(window, "Laptop", "[2001:db8::2]:44201");
        addDevice(window, "Studio", "192.0.2.1:44100");
        auto *table = control<QTableWidget>(window, "devices");
        ASSERT_EQ(table->rowCount(), 2);
        EXPECT_EQ(table->item(0, 0)->text(), "Studio");
        EXPECT_EQ(table->item(1, 0)->text(), "Laptop");
        table->selectRow(0);
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            dialog->button(QMessageBox::Cancel)->click();
        });
        control<QPushButton>(window, "removeDevice")->click();
        EXPECT_EQ(table->rowCount(), 2);
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            dialog->button(QMessageBox::Yes)->click();
        });
        control<QPushButton>(window, "removeDevice")->click();
        ASSERT_EQ(table->rowCount(), 1);
        window.close();
    }
    QSettings settings(path, QSettings::IniFormat);
    ashdesk::MainWindow reopened(settings, directory.path());
    auto *table = control<QTableWidget>(reopened, "devices");
    ASSERT_EQ(table->rowCount(), 1);
    EXPECT_EQ(table->item(0, 0)->text(), "Laptop");
}

TEST(DesktopGlass, IdleRepaintReusesCachesWhileResizeAndScrollRecompose) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath("desktop.ini"), QSettings::IniFormat);
    settings.setValue("appearance/theme", 1);
    ashdesk::MainWindow window(settings, directory.path());
    window.show();
    ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
    auto *surface = window.findChild<ashdesk::GlassSurface *>();
    ASSERT_NE(surface, nullptr);
    // The surface is opaque and covers the window, so window.repaint() would paint nothing.
    surface->repaint();
    ashdesk::GlassStats &stats = surface->stats();
    const int builds = stats.backdropBuilds;
    const int compositions = stats.compositions;
    ASSERT_GT(compositions, 0);
    // Median and p95 of `count` timed steps, in milliseconds.
    const auto timeSteps = [](int count, const std::function<void(int)> &step) {
        std::vector<qint64> frames;
        for (int i = 0; i < count; ++i) {
            QElapsedTimer timer;
            timer.start();
            step(i);
            frames.push_back(timer.nsecsElapsed());
        }
        return std::pair{percentileMilliseconds(frames, 50), percentileMilliseconds(frames)};
    };
    const auto idle = timeSteps(20, [&](int) { surface->repaint(); });
    EXPECT_EQ(stats.backdropBuilds, builds);
    EXPECT_EQ(stats.compositions, compositions);
    EXPECT_EQ(surface->blurred().size(), QSize(surface->sharp().width() / 4, surface->sharp().height() / 4));
    constexpr qsizetype kKiB = 1024;
    const auto panels = window.findChildren<ashdesk::GlassPanel *>();
    const qsizetype panelBytes =
        std::accumulate(panels.begin(), panels.end(), qsizetype(0),
                        [](qsizetype sum, const ashdesk::GlassPanel *panel) { return sum + panel->cacheBytes(); });
    std::printf("[ perf ] %dx%d@%.2f backdrop %.2f ms, %lld KiB; panel caches %lld KiB; full glass frame %.2f/%.2f ms "
                "(p50/p95)\n",
                window.width(), window.height(), surface->devicePixelRatioF(), double(stats.backdropNanoseconds) / 1e6,
                (surface->sharp().sizeInBytes() + surface->blurred().sizeInBytes()) / kKiB, panelBytes / kKiB,
                idle.first, idle.second);

    const auto resize = timeSteps(12, [&](int i) {
        window.resize(1120 - 8 * (i + 1), 740);
        QCoreApplication::processEvents();
        surface->repaint();
    });
    EXPECT_LE(stats.backdropBuilds, builds + 2) << "a drag must not rebuild the backdrop per step";
    ASSERT_TRUE(waitUntil([&] { return !surface->resizing(); }));
    QTest::qWait(30);
    const qreal dpr = surface->devicePixelRatioF();
    EXPECT_EQ(surface->sharp().size(), QSize(qCeil(surface->width() * dpr), qCeil(surface->height() * dpr)));
    window.resize(640, 480);
    QTest::qWait(30);
    QScrollBar *bar = nullptr;
    for (int i = 0; i < 5 && !bar; ++i) {
        control<QPushButton>(window, qPrintable(QString("nav%1").arg(i)))->click();
        QTest::qWait(10);
        auto *scroll = qobject_cast<QScrollArea *>(control<QStackedWidget>(window, "pages")->currentWidget());
        if (scroll && scroll->verticalScrollBar()->maximum() > 0) bar = scroll->verticalScrollBar();
    }
    ASSERT_NE(bar, nullptr) << "no page scrolls at 640x480";
    surface->repaint();
    const int beforeScroll = stats.compositions;
    const int steps = (bar->maximum() - bar->minimum()) / 4;
    const auto scroll = timeSteps(steps, [&](int i) {
        bar->setValue(bar->minimum() + 4 * (i + 1));
        surface->repaint();
    });
    EXPECT_GT(stats.compositions, beforeScroll);
    std::printf("[ perf ] drag-resize step %.2f/%.2f ms, scroll step %.2f/%.2f ms, panel composition p95 %.2f ms\n",
                resize.first, resize.second, scroll.first, scroll.second, compositionP95(stats));
    const auto glass = timeSteps(20, [&](int) { surface->repaint(); });
    control<QComboBox>(window, "material")->setCurrentIndex(0);
    const auto solid = timeSteps(20, [&](int) { surface->repaint(); });
    std::printf("[ perf ] 640x480 full frame p95: glass %.2f ms, solid %.2f ms\n", glass.second, solid.second);
    window.close();
}
TEST(DesktopGlass, SolidMaterialReleasesTheBackdropAndPersists) {
    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    {
        QSettings settings(path, QSettings::IniFormat);
        ashdesk::MainWindow window(settings, directory.path());
        window.show();
        ASSERT_TRUE(QTest::qWaitForWindowExposed(&window));
        auto *surface = window.findChild<ashdesk::GlassSurface *>();
        ASSERT_NE(surface, nullptr);
        surface->repaint();
        ASSERT_EQ(surface->material(), ashdesk::Material::Liquid);
        EXPECT_FALSE(surface->blurred().isNull());
        control<QComboBox>(window, "material")->setCurrentIndex(0);
        EXPECT_EQ(surface->material(), ashdesk::Material::Solid);
        EXPECT_TRUE(surface->sharp().isNull());
        EXPECT_TRUE(surface->blurred().isNull());
        const int compositions = surface->stats().compositions;
        surface->repaint();
        EXPECT_EQ(surface->stats().compositions, compositions);
        for (const auto *panel : window.findChildren<ashdesk::GlassPanel *>()) {
            EXPECT_EQ(panel->cacheBytes(), 0) << "solid material must release panel images and edge tables";
        }
        window.close();
    }
    QSettings settings(path, QSettings::IniFormat);
    ashdesk::MainWindow reopened(settings, directory.path());
    EXPECT_EQ(control<QComboBox>(reopened, "material")->currentIndex(), 0);
    EXPECT_EQ(reopened.findChild<ashdesk::GlassSurface *>()->material(), ashdesk::Material::Solid);
}

TEST(DesktopWidgets, FocusRingAppearsOnlyAfterKeyboardNavigation) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, directory.path());
    window.show();
    window.activateWindow();
    if (!QTest::qWaitForWindowActive(&window)) GTEST_SKIP() << "Window manager refused activation";
    const auto ringed = [&] {
        const auto buttons = window.findChildren<QPushButton *>();
        return std::count_if(buttons.begin(), buttons.end(),
                             [](const QPushButton *button) { return button->property("focusVisible").toBool(); });
    };
    ASSERT_NE(QApplication::focusWidget(), nullptr);
    EXPECT_EQ(ringed(), 0);
    QPushButton *focused = nullptr;
    for (int i = 0; i < 10 && !focused; ++i) {
        QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
        focused = qobject_cast<QPushButton *>(QApplication::focusWidget());
    }
    ASSERT_NE(focused, nullptr);
    EXPECT_TRUE(focused->property("focusVisible").toBool());
    EXPECT_EQ(ringed(), 1);
    window.close();
}

TEST(DesktopProcess, PipeStopExitsNormallyAndCapturesDiagnostics) {
    ashdesk::CoreProcess process;
    QString output;
    bool launched = false;
    QObject::connect(&process, &ashdesk::CoreProcess::output, &process, [&](const QString &text) { output += text; });
    QObject::connect(&process, &ashdesk::CoreProcess::launched, &process, [&] { launched = true; });
    ASSERT_TRUE(process.start(QCoreApplication::applicationFilePath(), {"--process-fixture"}));
    ASSERT_TRUE(waitUntil([&] { return launched && output.contains("ready"); }));
    EXPECT_TRUE(process.busy());
    process.stop();
    ASSERT_TRUE(waitUntil([&] { return !process.busy(); }));
    EXPECT_TRUE(output.contains("clean exit"));
}

TEST(DesktopProcess, StartupFailureDoesNotLeaveABusySession) {
    QTemporaryDir directory;
    ashdesk::CoreProcess process;
    EXPECT_FALSE(process.start(directory.filePath("missing-core"), {}));
    EXPECT_FALSE(process.busy());
    QString output;
    QObject::connect(&process, &ashdesk::CoreProcess::output, &process, [&](const QString &text) { output += text; });
    ASSERT_TRUE(process.start(QCoreApplication::applicationFilePath(), {"--process-failure"}));
    ASSERT_TRUE(waitUntil([&] { return !process.busy(); }));
    EXPECT_TRUE(output.contains("fixture failed"));
    EXPECT_TRUE(process.status().contains("23"));
}

TEST(DesktopStatus, ControlLinesAreUnescapedAndKeptOutOfTheLogAcrossChunkBoundaries) {
    ashdesk::CoreProcess process;
    QStringList events;
    QList<ashdesk::CoreMessage> messages;
    QObject::connect(&process, &ashdesk::CoreProcess::output, &process,
                     [&](const QString &text) { events << "log:" + text; });
    QObject::connect(&process, &ashdesk::CoreProcess::message, &process, [&](const ashdesk::CoreMessage &message) {
        events << "ui:" + message.topic;
        messages << message;
    });
    process.consume("starting\n@ui phase=stream peer=192.0.2.7:50000 name=My\\sLaptop\\\\A\\nB perms=control,files\nmid");
    ASSERT_EQ(messages.size(), 1);
    EXPECT_EQ(messages[0].topic, "phase");
    EXPECT_TRUE(messages[0].action.isEmpty());
    EXPECT_EQ(messages[0].value("phase"), "stream");
    EXPECT_EQ(messages[0].value("name"), "My Laptop\\A\nB");
    EXPECT_EQ(messages[0].value("perms"), "control,files");
    // 一行状态、一个 UTF-8 字符都可能被管道切成两段
    const QByteArray name = QStringLiteral("笔记本").toUtf8();
    process.consume("dle\r\n@ui pa");
    process.consume("ir done id=0011aabb name=" + name.left(4));
    process.consume(name.mid(4) + "\r\n@ui stats fps=60 mbps=35.29\ntail");
    ASSERT_EQ(messages.size(), 3);
    EXPECT_EQ(messages[1].topic, "pair");
    EXPECT_EQ(messages[1].action, "done");
    EXPECT_EQ(messages[1].value("name"), QStringLiteral("笔记本"));
    EXPECT_EQ(messages[2].topic, "stats");
    EXPECT_EQ(messages[2].value("mbps"), "35.29");
    EXPECT_EQ(events, QStringList({"log:starting\n", "ui:phase", "log:middle\r\n", "ui:pair", "ui:stats"}));
    ashdesk::CoreMessage ignored;
    EXPECT_FALSE(ashdesk::parseCoreMessage(u"@uix phase=stream", ignored));
    EXPECT_FALSE(ashdesk::parseCoreMessage(u" @ui phase=stream", ignored));
}

TEST(DesktopStatus, SessionAndHostPagesShowWhatTheCoreReports) {
    QTemporaryDir directory;
    QSettings settings(directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, directory.path());
    window.show();
    auto *client = control<ashdesk::CoreProcess>(window, "clientCore");
    auto *clientStatus = control<QLabel>(window, "clientStatus");
    auto *clientStats = control<QLabel>(window, "clientStats");
    const QList<QPair<QByteArray, QString>> phases{{"lookup", QStringLiteral("正在查找主机")},
                                                   {"handshake", QStringLiteral("正在握手")},
                                                   {"negotiate", QStringLiteral("正在协商")},
                                                   {"configure", QStringLiteral("等待首帧")}};
    for (const auto &[phase, text] : phases) {
        client->consume("@ui phase=" + phase + " path=direct secure=1 reconnects=0\n");
        EXPECT_EQ(clientStatus->text(), text);
    }
    EXPECT_TRUE(clientStats->isHidden());
    client->consume("@ui error=10 text=握手\\s3\\ss\\s未完成\n");
    EXPECT_EQ(clientStatus->text(), QStringLiteral("等待首帧 · 握手 3 s 未完成"));
    client->consume("@ui phase=stream path=direct secure=1 reconnects=0\nplain log line\n"
                    "@ui stats fps=59.8 lat_ms=4.2 rtt_ms=1.6 mbps=0.10 loss=0.0 path=relay enc=1 cap=1 perms=127 state=0\n");
    EXPECT_EQ(clientStatus->text(), QStringLiteral("已连接 · 中转"));
    EXPECT_EQ(clientStats->text(), QStringLiteral("59.8 fps · 0.10 Mb/s · 往返 1.6 ms · 丢包 0.0% · NVENC · DXGI"));
    EXPECT_FALSE(clientStats->isHidden());
    EXPECT_EQ(control<QPlainTextEdit>(window, "clientLog")->toPlainText(), "plain log line\n");
    control<QPushButton>(window, "nav1")->click();
    snapshot(window, "session");
    client->consume("@ui stats fps=30.0 lat_ms=9 rtt_ms=20.5 mbps=4.00 loss=1.5 path=tcp enc=0 cap=4 perms=0 state=0\n");
    EXPECT_EQ(clientStatus->text(), QStringLiteral("已连接 · TCP 中转"));
    EXPECT_EQ(clientStats->text(), QStringLiteral("30.0 fps · 4.00 Mb/s · 往返 20.5 ms · 丢包 1.5% · 测试图样"));

    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostStatus = control<QLabel>(window, "hostStatus");
    auto *hostDetail = control<QLabel>(window, "hostDetail");
    auto *hostStats = control<QLabel>(window, "hostStats");
    host->consume("@ui phase=listening\n");
    EXPECT_EQ(hostStatus->text(), QStringLiteral("等待连接"));
    EXPECT_TRUE(hostDetail->isHidden());
    host->consume("@ui phase=negotiate\n");
    EXPECT_EQ(hostStatus->text(), QStringLiteral("正在连接"));
    host->consume(QStringLiteral("@ui phase=stream peer=192.0.2.7:50000 name=书房\\s笔记本 perms=control,files,clipboard\n").toUtf8());
    EXPECT_EQ(hostStatus->text(), QStringLiteral("被控中 · 书房 笔记本 · 192.0.2.7:50000"));
    EXPECT_EQ(hostDetail->text(), QStringLiteral("控制、传文件、剪贴板"));
    EXPECT_TRUE(hostStats->isHidden());
    host->consume("@ui stats fps=60 mbps=35.29 rtt_ms=0.2 loss=0.0 path=direct enc=4 cap=3 perms=view curtain=0 blocked=0\n");
    EXPECT_EQ(hostStats->text(), QStringLiteral("60 fps · 35.29 Mb/s · 往返 0.2 ms · 丢包 0.0% · AE264 · IDD"));
    EXPECT_EQ(hostDetail->text(), QStringLiteral("只看画面"));
    EXPECT_TRUE(control<QPlainTextEdit>(window, "hostLog")->toPlainText().isEmpty());
    host->consume("@ui phase=listening\n");
    EXPECT_EQ(hostStatus->text(), QStringLiteral("等待连接"));
    EXPECT_TRUE(hostDetail->isHidden());
    EXPECT_TRUE(hostStats->isHidden());
    window.close();
}

// 借鉴设计 A2：装了系统服务时界面不自己起主机，接到服务的 \\.\pipe\<服务名>.ui 上。这里用 QLocalServer 扮服务
TEST(DesktopService, HostRunByTheServiceIsShownAndDrivenThroughItsPipe) {
#ifndef Q_OS_WIN
    GTEST_SKIP() << "The system service exists only on Windows";
#endif
    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    const QString service = QStringLiteral("AshDeskSvcTest%1").arg(QCoreApplication::applicationPid());
    settings.setValue("serviceName", service);
    QLocalServer server;
    ASSERT_TRUE(server.listen(service + ".ui"));
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    ASSERT_TRUE(waitUntil([&] { return server.hasPendingConnections(); }));
    QLocalSocket *pipe = server.nextPendingConnection();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostStatus = control<QLabel>(window, "hostStatus");
    auto *startHost = control<QPushButton>(window, "startHost");
    const auto received = [&](const QByteArray &line) {
        return waitUntil([&] { return pipe->canReadLine(); }) && pipe->readLine() == line + '\n';
    };
    ASSERT_TRUE(waitUntil([&] { return host->attached(); }));
    EXPECT_FALSE(startHost->isEnabled());
    EXPECT_FALSE(startHost->toolTip().isEmpty());
    EXPECT_FALSE(control<QPushButton>(window, "stopHost")->isEnabled());
    EXPECT_FALSE(control<QPushButton>(window, "pairDevice")->isEnabled());
    EXPECT_TRUE(control<QLabel>(window, "hostBanner")->isHidden());

    pipe->write("@ui service agent=0\n");
    ASSERT_TRUE(waitUntil([&] { return hostStatus->text() == QStringLiteral("系统服务正在拉起主机"); }));
    pipe->write("@ui service agent=4242\n@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control,files\n");
    ASSERT_TRUE(waitUntil([&] { return hostStatus->text() == QStringLiteral("被控中 · Laptop · 192.0.2.7:50000"); }));
    host->send(QStringLiteral("perm -files"));
    EXPECT_TRUE(received("perm -files"));
    if (auto *disconnect = window.findChild<QAction *>("trayDisconnect")) {
        EXPECT_TRUE(disconnect->isEnabled());
        disconnect->trigger();
        EXPECT_TRUE(received("disconnect"));
    }
    // 服务重拉了代理：上一个代理的会话作废
    pipe->write("@ui service agent=4243\n");
    ASSERT_TRUE(waitUntil([&] { return hostStatus->text() == QStringLiteral("由系统服务运行"); }));

    // N4：主机归服务管时，固定密码和设备页一样经这条管道——界面那个账户读不了服务主机的设备表，更不能另起进程去改
    auto *fixedState = control<QLabel>(window, "fixedState");
    auto *fixedPassword = control<QLineEdit>(window, "fixedPassword");
    auto *message = control<QLabel>(window, "message");
    EXPECT_FALSE(fixedPassword->isEnabled());
    pipe->write("@ui fixed set=0\n");
    ASSERT_TRUE(waitUntil([&] { return fixedState->text() == QStringLiteral("未设置"); }));
    fixedPassword->setText("Summer-2026");
    control<QPushButton>(window, "setFixed")->click();
    EXPECT_TRUE(received("password fixed Summer-2026"));
    pipe->write("@ui fixed set=1\n");
    ASSERT_TRUE(waitUntil([&] { return message->text() == QStringLiteral("固定密码已设置"); }));
    control<QPushButton>(window, "clearFixed")->click();
    EXPECT_TRUE(received("password fixed-clear"));
    pipe->write("@ui fixed set=0\n");
    ASSERT_TRUE(waitUntil([&] { return message->text() == QStringLiteral("固定密码已清除"); }));
    // 临时密码的两个开关也一样：服务补发 / 转来的 @ui temp 摆开关，改动经管道发给主机
    auto *temp = control<QCheckBox>(window, "tempPassword");
    auto *keep = control<QCheckBox>(window, "tempKeep");
    EXPECT_FALSE(temp->isEnabled());
    pipe->write("@ui temp on=1 keep=0\n");
    ASSERT_TRUE(waitUntil([&] { return temp->isEnabled() && temp->isChecked(); }));
    keep->click();
    EXPECT_TRUE(received("password temp keep"));
    pipe->write("@ui temp on=1 keep=1\n");
    ASSERT_TRUE(waitUntil([&] { return message->text() == QStringLiteral("设置已保存"); }));
    EXPECT_TRUE(keep->isChecked());
    temp->click();
    EXPECT_TRUE(received("password temp off"));
    pipe->write("@ui temp on=0 keep=1\n@ui password code=\n");
    ASSERT_TRUE(waitUntil([&] { return !temp->isChecked() && !keep->isEnabled(); }));
    control<QPushButton>(window, "nav3")->click();
    EXPECT_TRUE(received("devices"));
    pipe->write("@ui device id=00112233445566778899aabbccddeeff name=Laptop perms=control,files paired=1762000000 temp=0\n@ui devices n=1\n");
    ASSERT_TRUE(waitUntil([&] { return window.findChild<QCheckBox *>("perm-00112233-files") != nullptr; }));
    window.findChild<QCheckBox *>("perm-00112233-files")->click();
    EXPECT_TRUE(received("device-perm 00112233445566778899aabbccddeeff control"));
    pipe->write("@ui device-perm ok=1\n");
    EXPECT_TRUE(received("devices"));
    pipe->write("@ui device id=00112233445566778899aabbccddeeff name=Laptop perms=control paired=1762000000 temp=0\n@ui devices n=1\n");
    ASSERT_TRUE(waitUntil([&] { return message->text() == QStringLiteral("设备权限已更新，下次连接生效") && control<QWidget>(window, "paired")->isEnabled(); }));
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->button(QMessageBox::Yes)->click();
    });
    control<QPushButton>(window, "revoke-00112233")->click();
    EXPECT_TRUE(received("revoke 00112233445566778899aabbccddeeff"));
    pipe->write("@ui revoked ok=1 id=00112233\n");
    EXPECT_TRUE(received("devices"));
    pipe->write("@ui devices n=0\n");
    ASSERT_TRUE(waitUntil([&] { return window.findChild<QPushButton *>("revoke-00112233") == nullptr; }));
    EXPECT_EQ(message->text(), QStringLiteral("已吊销 Laptop"));
    EXPECT_TRUE(cores.calls().isEmpty());
    // 服务换了代理：两种密码的设置清掉，等新代理报；设备页开着就按新代理的设备表重画
    pipe->write("@ui service agent=4244\n");
    EXPECT_TRUE(received("devices"));
    EXPECT_TRUE(fixedState->text().isEmpty());
    EXPECT_FALSE(fixedPassword->isEnabled());
    EXPECT_FALSE(temp->isEnabled());
    EXPECT_FALSE(keep->isEnabled());
    pipe->write("@ui devices n=0\n@ui fixed set=1\n@ui temp on=1 keep=1\n");
    ASSERT_TRUE(waitUntil([&] { return fixedState->text() == QStringLiteral("已设置") && temp->isEnabled(); }));
    EXPECT_TRUE(temp->isChecked());
    EXPECT_TRUE(keep->isChecked());
    control<QPushButton>(window, "nav0")->click();

    // 服务那头断了：界面放开主机这一页，过一会儿自己再接上
    pipe->disconnectFromServer();
    ASSERT_TRUE(waitUntil([&] { return !host->attached(); }));
    EXPECT_TRUE(startHost->isEnabled());
    ASSERT_TRUE(waitUntil([&] { return server.hasPendingConnections(); }));
    ASSERT_TRUE(waitUntil([&] { return host->attached(); }));

    // 退出界面只是断开管道，不发 stop
    pipe = server.nextPendingConnection();
    if (auto *quit = window.findChild<QAction *>("trayQuit")) {
        quit->trigger();
    } else {
        window.close();
    }
    EXPECT_FALSE(window.isVisible());
    EXPECT_FALSE(host->attached());
    ASSERT_TRUE(waitUntil([&] { return pipe->state() == QLocalSocket::UnconnectedState; }));
    EXPECT_FALSE(pipe->readAll().contains("stop"));
}

#ifdef Q_OS_WIN
// 服务管道的属主：真服务（LocalSystem，或 Administrators 当默认属主）和自己认；别的账户（服务没在跑时抢先建同名管道冒充）不认
TEST(DesktopService, PipeOwnedByAnotherAccountIsNotTrusted) {
    const auto wellKnown = [](WELL_KNOWN_SID_TYPE type) {
        std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{};
        DWORD size = DWORD(sid.size());
        EXPECT_TRUE(CreateWellKnownSid(type, nullptr, sid.data(), &size));
        return sid;
    };
    auto system = wellKnown(WinLocalSystemSid);
    auto admins = wellKnown(WinBuiltinAdministratorsSid);
    auto localService = wellKnown(WinLocalServiceSid);
    auto networkService = wellKnown(WinNetworkServiceSid);
    auto everyone = wellKnown(WinWorldSid);
    EXPECT_TRUE(ashdesk::trustedServiceOwner(system.data()));
    EXPECT_TRUE(ashdesk::trustedServiceOwner(admins.data()));
    EXPECT_FALSE(ashdesk::trustedServiceOwner(localService.data()));
    EXPECT_FALSE(ashdesk::trustedServiceOwner(networkService.data()));
    EXPECT_FALSE(ashdesk::trustedServiceOwner(everyone.data()));
    EXPECT_FALSE(ashdesk::trustedServiceOwner(nullptr));

    PSID other = nullptr;                            // 别的本地账户
    ASSERT_TRUE(ConvertStringSidToSidW(L"S-1-5-21-1111111111-2222222222-3333333333-1001", &other));
    EXPECT_FALSE(ashdesk::trustedServiceOwner(other));
    LocalFree(other);

    HANDLE token = nullptr;
    union {
        TOKEN_USER user;
        std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> raw;
    } me{};
    DWORD size = 0;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    ASSERT_TRUE(GetTokenInformation(token, TokenUser, &me, sizeof me, &size));
    CloseHandle(token);
    EXPECT_TRUE(ashdesk::trustedServiceOwner(me.user.User.Sid));
}
#endif

TEST(DesktopTray, RunningHostHidesToTrayAndTheMenuDrivesTheSession) {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) GTEST_SKIP() << "No system tray on this desktop";
    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *tray = window.findChild<QSystemTrayIcon *>("tray");
    ASSERT_NE(tray, nullptr);
    EXPECT_FALSE(tray->isVisible());
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostStatus = control<QLabel>(window, "hostStatus");
    auto *hostLog = control<QPlainTextEdit>(window, "hostLog");
    auto *disconnect = window.findChild<QAction *>("trayDisconnect");
    auto *permissions = window.findChild<QMenu *>("trayPermissions");
    ASSERT_NE(disconnect, nullptr);
    ASSERT_NE(permissions, nullptr);
    EXPECT_FALSE(control<QMenu>(window, "trayMenu")->accessibleName().isEmpty());

    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil([&] { return hostStatus->text() == QStringLiteral("等待连接"); }));
    EXPECT_TRUE(tray->isVisible());
    EXPECT_TRUE(tray->toolTip().contains(QStringLiteral("等待连接")));
    EXPECT_FALSE(disconnect->isEnabled());
    EXPECT_FALSE(permissions->isEnabled());
    const QImage idle = tray->icon().pixmap(16).toImage();

    host->consume(QStringLiteral("@ui phase=stream peer=192.0.2.7:50000 name=笔记本 perms=control,files\n").toUtf8());
    EXPECT_NE(tray->icon().pixmap(16).toImage(), idle);
    EXPECT_TRUE(tray->toolTip().contains(QStringLiteral("笔记本")));
    const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
    if (!snapshots.isEmpty()) {
        EXPECT_TRUE(QDir().mkpath(snapshots));
        EXPECT_TRUE(idle.save(QDir(snapshots).filePath("tray-idle-16.png")));
        for (const int size : {16, 24, 32})
            EXPECT_TRUE(tray->icon().pixmap(size).save(QDir(snapshots).filePath(QString("tray-controlled-%1.png").arg(size))));
    }
    EXPECT_TRUE(disconnect->isEnabled());
    EXPECT_TRUE(permissions->isEnabled());
    ASSERT_EQ(permissions->actions().size(), 2);
    QAction *files = permissions->actions()[1];
    EXPECT_EQ(files->text(), QStringLiteral("传文件"));
    EXPECT_TRUE(files->isChecked());
    files->trigger();
    ASSERT_TRUE(waitUntil([&] { return hostLog->toPlainText().contains("command perm -files"); }));
    host->consume("@ui stats fps=60 mbps=35.29 rtt_ms=0.2 loss=0.0 path=direct enc=1 cap=1 perms=control curtain=0 blocked=0\n");
    ASSERT_EQ(permissions->actions().size(), 2);
    EXPECT_TRUE(permissions->actions()[0]->isChecked());
    EXPECT_FALSE(files->isChecked());
    files->trigger();
    ASSERT_TRUE(waitUntil([&] { return hostLog->toPlainText().contains("command perm +files"); }));
    disconnect->trigger();
    ASSERT_TRUE(waitUntil([&] { return hostLog->toPlainText().contains("command disconnect"); }));
    host->consume("@ui phase=listening\n");
    EXPECT_EQ(tray->icon().pixmap(16).toImage(), idle);
    EXPECT_FALSE(disconnect->isEnabled());
    EXPECT_TRUE(permissions->actions().isEmpty());

    window.close();
    EXPECT_FALSE(window.isVisible());
    EXPECT_TRUE(host->busy());
    window.findChild<QAction *>("trayShow")->trigger();
    EXPECT_TRUE(window.isVisible());
    // 窗口收着的时候主机核心停了：托盘图标跟着消失，窗口要自己回来
    window.close();
    ASSERT_FALSE(window.isVisible());
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    EXPECT_TRUE(window.isVisible());
    EXPECT_FALSE(tray->isVisible());

    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil([&] { return hostStatus->text() == QStringLiteral("等待连接"); }));
    window.close();
    ASSERT_FALSE(window.isVisible());
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->button(QMessageBox::Yes)->click();
    });
    window.findChild<QAction *>("trayQuit")->trigger();
    ASSERT_TRUE(waitUntil([&] { return !host->busy() && !window.isVisible(); }));
    // 核心日志落在交给界面的数据目录里，不碰用户的 %LOCALAPPDATA%
    const QString log = QDir::toNativeSeparators(cores.directory.filePath("data/logs/host.log"));
    EXPECT_TRUE(cores.calls().constLast().endsWith("--log " + log)) << cores.calls().constLast().toStdString();
    EXPECT_TRUE(QDir(cores.directory.filePath("data/logs")).exists());
}

TEST(DesktopTotp, HostModeReachesTheCoreAndTheClientAsksForACodeUntilItIsAccepted) {
    ashdesk::DesktopOptions options;
    EXPECT_FALSE(ashdesk::hostArguments(options).contains("--totp"));
    options.totp = 1;
    QStringList host = ashdesk::hostArguments(options);
    EXPECT_EQ(host[host.indexOf("--totp") + 1], "pair");
    options.totp = 2;
    host = ashdesk::hostArguments(options);
    EXPECT_EQ(host[host.indexOf("--totp") + 1], "session");
    EXPECT_FALSE(ashdesk::clientArguments({"192.0.2.9", 44100}, options).contains("--totp"));
    const QStringList withCode = ashdesk::clientArguments({"192.0.2.9", 44100}, options, "246810");
    EXPECT_EQ(withCode[withCode.indexOf("--totp") + 1], "246810");

    const FakeCores cores;
    cores.write("totp.txt", "246810");
    cores.storeKey();                            // 按地址连走共享密钥（没存过的走号码，见 DesktopDirect）
    const QString path = cores.directory.filePath("desktop.ini");
    QSettings settings(path, QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    control<QComboBox>(window, "totp")->setCurrentIndex(2);
    EXPECT_EQ(settings.value("security/totp").toInt(), 2);
    control<QPushButton>(window, "nav4")->click();
    control<QTabWidget>(window, "settingsTabs")->setCurrentIndex(3);
    snapshot(window, "security");
    // 主机要二次验证却还没生成密钥：不拉起核心，直接说明
    control<QPushButton>(window, "startHost")->click();
    EXPECT_FALSE(control<ashdesk::CoreProcess>(window, "hostCore")->busy());
    EXPECT_EQ(control<QLabel>(window, "message")->text(), QStringLiteral("请先在设置 → 安全中生成二次验证密钥。"));

    auto *client = control<ashdesk::CoreProcess>(window, "clientCore");
    auto *status = control<QLabel>(window, "clientStatus");
    int prompts = 0;
    QTimer poller;
    QObject::connect(&poller, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("totpPrompt");
        if (!dialog || !dialog->isVisible()) return;
        auto *code = control<QLineEdit>(*dialog, "totpCode");
        auto *actions = dialog->findChild<QDialogButtonBox *>();
        QPushButton *accept = actions->button(QDialogButtonBox::Ok);
        ++prompts;
        if (prompts == 3) {
            actions->button(QDialogButtonBox::Cancel)->click();
            return;
        }
        if (prompts == 1) {
            const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
            if (!snapshots.isEmpty()) {
                EXPECT_TRUE(dialog->grab().save(QDir(snapshots).filePath("totp-prompt.png")));
            }
            EXPECT_FALSE(code->accessibleName().isEmpty());
            EXPECT_FALSE(accept->isEnabled());
            QTest::keyClicks(code, "12345x");
            EXPECT_EQ(code->text(), "12345");
            EXPECT_FALSE(accept->isEnabled());
        }
        code->setText(prompts == 1 ? "135790" : "246810");
        EXPECT_TRUE(accept->isEnabled());
        accept->click();
    });
    poller.start(20);
    control<QLineEdit>(window, "address")->setText("192.0.2.9:44100");
    control<QPushButton>(window, "connect")->click();
    ASSERT_TRUE(waitUntil([&] { return status->text() == QStringLiteral("已连接 · 直连"); })) << status->text().toStdString();
    EXPECT_EQ(prompts, 2);
    const QStringList calls = cores.calls();
    ASSERT_EQ(calls.size(), 3);
    EXPECT_FALSE(calls[0].contains("--totp"));
    EXPECT_TRUE(calls[1].contains("--totp 135790"));
    EXPECT_TRUE(calls[2].contains("--totp 246810"));
    EXPECT_TRUE(calls[2].contains("--host 192.0.2.9 --port 44100 --ui-control"));
    control<QPushButton>(window, "stopClient")->click();
    ASSERT_TRUE(waitUntil([&] { return !client->busy(); }));

    control<QPushButton>(window, "nav0")->click();
    control<QPushButton>(window, "connect")->click();
    ASSERT_TRUE(waitUntil([&] { return prompts == 3 && !client->busy(); }));
    QTest::qWait(100);
    EXPECT_EQ(prompts, 3);
    EXPECT_EQ(status->text(), QStringLiteral("连接失败：totp required"));
    EXPECT_EQ(cores.calls().size(), 4);
    window.close();
}

TEST(DesktopPairing, PairActionsRestartTheHostAndShowTheCodeUntilPairingEnds) {
    ashdesk::DesktopOptions options;
    EXPECT_FALSE(ashdesk::hostArguments(options).join(' ').contains("--pair"));
    EXPECT_TRUE(ashdesk::hostArguments(options, ashdesk::Pairing::Device).contains("--pair"));
    EXPECT_TRUE(ashdesk::hostArguments(options, ashdesk::Pairing::Temporary).contains("--pair-temp"));

    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    control<QPushButton>(window, "nav2")->click();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *status = control<QLabel>(window, "hostStatus");
    auto *panel = control<QWidget>(window, "pairing");
    auto *code = control<QLabel>(window, "pairingCode");
    auto *title = control<QLabel>(window, "pairingTitle");
    const auto listening = [&] { return status->text() == QStringLiteral("等待连接"); };
    EXPECT_TRUE(panel->isHidden());
    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil(listening));
    EXPECT_TRUE(panel->isHidden());

    control<QPushButton>(window, "pairDevice")->click();
    ASSERT_TRUE(waitUntil([&] { return panel->isVisible(); }));
    EXPECT_EQ(code->text(), "135 792");
    EXPECT_EQ(title->text(), QStringLiteral("配对码"));
    const QString left = control<QLabel>(window, "pairingTime")->text();
    EXPECT_TRUE(left == "10:00" || left == "9:59") << left.toStdString();
    ASSERT_EQ(cores.calls().size(), 2);
    EXPECT_TRUE(cores.calls()[1].contains("--pair "));
    host->consume(QStringLiteral("@ui phase=stream peer=192.0.2.7:50000 name=书房\\s笔记本 perms=control,files,clipboard\n"
                                 "@ui stats fps=60 mbps=35.29 rtt_ms=0.2 loss=0.0 path=direct enc=1 cap=1 "
                                 "perms=control,files,clipboard curtain=0 blocked=0\n")
                      .toUtf8());
    EXPECT_TRUE(panel->isVisible());
    snapshot(window, "host-pairing");
    host->consume("@ui phase=listening\n");
    host->consume(QStringLiteral("@ui pair done name=书房\\s笔记本 id=0011aabb temp=0\n").toUtf8());
    EXPECT_TRUE(panel->isHidden());
    EXPECT_EQ(control<QLabel>(window, "message")->text(), QStringLiteral("已配对：书房 笔记本"));

    control<QPushButton>(window, "pairTemporary")->click();
    ASSERT_TRUE(waitUntil([&] { return panel->isVisible() && code->text() == "246 801"; }));
    EXPECT_EQ(title->text(), QStringLiteral("临时口令"));
    EXPECT_TRUE(cores.calls().constLast().contains("--pair-temp "));
    host->consume("@ui pair closed reason=expired\n");
    EXPECT_TRUE(panel->isHidden());

    // 被控中再点配对要先确认：重启主机会把对方踢下线
    host->consume("@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control\n");
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->button(QMessageBox::Cancel)->click();
    });
    control<QPushButton>(window, "pairDevice")->click();
    EXPECT_TRUE(host->busy());
    EXPECT_EQ(cores.calls().size(), 3);

    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    control<QPushButton>(window, "pairDevice")->click();
    ASSERT_TRUE(waitUntil([&] { return panel->isVisible(); }));
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    EXPECT_TRUE(panel->isHidden());
    window.close();
}

// 号码连接（号码连接设计 N3）
TEST(DesktopNumber, NumbersAndPasswordsAreRecognisedWithSeparatorsAndFullWidthDigits) {
    QString number;
    EXPECT_EQ(ashdesk::parseNumber("123456789", number), 1);
    EXPECT_EQ(number, "123456789");
    EXPECT_EQ(ashdesk::parseNumber(" 987 654-321 ", number), 1);
    EXPECT_EQ(number, "987654321");
    EXPECT_EQ(ashdesk::parseNumber(QStringLiteral("１２３ ４５６ ７８９"), number), 1);
    EXPECT_EQ(number, "123456789");
    number = "keep";
    for (const char *wrong : {"12345678", "1234567890", "023456789", "44100"}) {
        EXPECT_EQ(ashdesk::parseNumber(wrong, number), -1) << wrong;
        EXPECT_EQ(number, "keep");
    }
    for (const char *address : {"192.168.1.23", "host-01", "pc1", "", " - ", "123456789:44100", "[::1]"}) {
        EXPECT_EQ(ashdesk::parseNumber(address, number), 0) << address;
    }
    EXPECT_EQ(ashdesk::groupDigits("123456789"), "123 456 789");
    EXPECT_EQ(ashdesk::groupDigits("12345678"), "1234 5678");
    EXPECT_EQ(ashdesk::groupDigits(""), "");
    EXPECT_EQ(ashdesk::parsePassword("1234 5678"), "12345678");
    EXPECT_EQ(ashdesk::parsePassword(QStringLiteral("１２３４-５６７８")), "12345678");
    // 固定密码（N4）：只去掉首尾空白，其余原样。规则同核心的 adp_password_kind：8–64 个可打印 ASCII 字符，
    // 不能只有数字、空格、连字符
    constexpr qsizetype kFixedMax = 64;
    EXPECT_EQ(ashdesk::parsePassword("  Summer-2026\t"), "Summer-2026");
    EXPECT_EQ(ashdesk::parsePassword("1234 567a"), "1234 567a");
    EXPECT_EQ(ashdesk::parsePassword("pass~!@#"), "pass~!@#");
    EXPECT_EQ(ashdesk::parsePassword(QString(kFixedMax, u'x')), QString(kFixedMax, u'x'));
    const QStringList wrongPasswords{"1234567",    "123456789",      "",          "abcdefg",   "1234-5678-9", "12 34 56 78 90",
                                     "tab\tinside", QString(kFixedMax + 1, u'x'), QStringLiteral("密码password"),
                                     QStringLiteral("１２３４abcd"), QStringLiteral("pässword1")};
    for (const QString &wrong : wrongPasswords) {
        EXPECT_TRUE(ashdesk::parsePassword(wrong).isEmpty()) << wrong.toStdString();
    }
    // 设固定密码时不认 8 位数字（那是临时密码的格式），也不替用户去掉空格
    for (const char *fixed : {"Summer-2026", "a b c d e", "abcdefgh"}) {
        EXPECT_TRUE(ashdesk::validFixedPassword(fixed)) << fixed;
    }
    for (const char *weak : {"12345678", " Summer-2026", "Summer-2026 ", "abcdefg", "1234-5678 9", ""}) {
        EXPECT_FALSE(ashdesk::validFixedPassword(weak)) << weak;
    }
    // 号码和密码写在一起（对方复制过来的）：夹着文字、标点、全角数字都认，前 9 位是号码、后 8 位是密码
    for (const QString &both : {QStringLiteral("123 456 789 1234 5678"), QStringLiteral("号码 123 456 789，密码 1234 5678"),
                                QStringLiteral("Number 123-456-789, password 1234-5678"), QStringLiteral("１２３４５６７８９１２３４５６７８")}) {
        ashdesk::NumberTarget target;
        EXPECT_TRUE(ashdesk::parseNumberTarget(both, target)) << both.toStdString();
        EXPECT_EQ(target.number, "123456789");
        EXPECT_EQ(target.password, "12345678");
    }
    // 不是 17 位、号码以 0 开头、带着地址里才有的字符：不当成“号码 + 密码”，原样留给后面按号码或地址去认
    for (const char *other : {"123456789", "123 456 789 1234 567", "123 456 789 1234 56789", "023 456 789 1234 5678", "",
                              "1234567.8912345678", "123456789@12345678", "[1234:5678:9123::4567]:8", "http://12345678912345678/"}) {
        ashdesk::NumberTarget target{"keep", "keep"};
        EXPECT_FALSE(ashdesk::parseNumberTarget(other, target)) << other;
        EXPECT_EQ(target.number, "keep");
        EXPECT_EQ(target.password, "keep");
    }
}

TEST(DesktopNumber, HostRegistersItsNumberAndTheClientConnectsByNumberWithoutASharedKey) {
    ashdesk::DesktopOptions options;
    const QString builtin = ashdesk::signalServer(options);
    EXPECT_FALSE(builtin.isEmpty());
    // 默认：登记号码、开连接密码；数据目录里没存过共享密钥就不带 --psk-stored（带了核心起不来）
    const QStringList host = ashdesk::hostArguments(options, ashdesk::Pairing::None, {false, "C:/data/number.txt"});
    EXPECT_EQ(host[host.indexOf("--rendezvous") + 1], builtin);
    EXPECT_EQ(host[host.indexOf("--number-file") + 1], "C:/data/number.txt");
    EXPECT_TRUE(host.contains("--number"));
    EXPECT_TRUE(host.contains("--password"));
    EXPECT_FALSE(host.contains("--psk-stored"));
    EXPECT_EQ(host[host.indexOf("--takeover-confirm") + 1], "never");   // 11-26：凭据对了就直接连，不在本机问
    EXPECT_TRUE(ashdesk::hostArguments(options, ashdesk::Pairing::None, {true, {}}).contains("--psk-stored"));
    EXPECT_FALSE(ashdesk::hostArguments(options, ashdesk::Pairing::None, {true, {}}).contains("--number-file"));
    // 配对窗口和连接密码核心只许开一个
    const QStringList pairing = ashdesk::hostArguments(options, ashdesk::Pairing::Temporary, {false, {}});
    EXPECT_TRUE(pairing.contains("--pair-temp"));
    EXPECT_TRUE(pairing.contains("--number"));
    EXPECT_FALSE(pairing.contains("--password"));
    options.confirm = 1;
    const QStringList attended = ashdesk::hostArguments(options);
    EXPECT_EQ(attended[attended.indexOf("--takeover-confirm") + 1], "attended");
    options.confirm = 2;
    options.rendezvous = " signal.example:44200 ";
    const QStringList asking = ashdesk::hostArguments(options);
    EXPECT_EQ(asking[asking.indexOf("--takeover-confirm") + 1], "always");
    EXPECT_EQ(asking[asking.indexOf("--rendezvous") + 1], "signal.example:44200");

    options.viewOnly = true;
    // 密码不进参数表：只告诉核心“密码从 stdin 来”（--password -）
    const QStringList client = ashdesk::numberArguments({"123456789", "12345678"}, options);
    EXPECT_EQ(client.mid(0, 5), (QStringList{"--connect", "123456789@signal.example:44200", "--password", "-", "--ui-control"}));
    EXPECT_TRUE(client.contains("--no-input"));
    for (const char *absent : {"--host", "--port", "--psk-stored", "--psk-file", "--totp", "12345678"}) {
        EXPECT_FALSE(client.contains(absent)) << absent;
    }
    // 不知道密码：不带 --password，对方记得这台设备才连得上
    const QStringList trial = ashdesk::numberArguments({"123456789", {}}, options);
    EXPECT_EQ(trial.mid(0, 3), (QStringList{"--connect", "123456789@signal.example:44200", "--ui-control"}));
    EXPECT_FALSE(trial.contains("--password"));
    EXPECT_TRUE(trial.contains("--no-input"));

    // 号码连接关掉：和原来一样只听端口、要共享密钥
    options.numberHost = false;
    const QStringList plain = ashdesk::hostArguments(options, ashdesk::Pairing::None, {false, "C:/data/number.txt"});
    for (const char *absent : {"--rendezvous", "--number", "--number-file", "--password"}) {
        EXPECT_FALSE(plain.contains(absent)) << absent;
    }
    EXPECT_TRUE(plain.contains("--psk-stored"));
}

// 按地址连、没存过共享密钥（docs/CrossDesk借鉴设计.md CD10）：按号码认。先不带密码试连，对方要密码就在原地问，
// 问到了带着密码再连；密码不进命令行。存过共享密钥的照旧用它（DesktopTotp、DesktopLanguage 里的 --host … --ui-control）
// 身份的导出、导入（docs/CrossDesk借鉴设计.md CD13）交给客户端核心：口令不在参数里；主机跑着时不让导入
// （它会拿内存里的旧设备表盖回去），点了直接说明，不弹文件框
TEST(DesktopIdentity, ArgumentsCarryNoPassphraseAndImportWaitsForTheHostToStop) {
    const QStringList out = ashdesk::identityArguments(true, "C:/x/me.ashid", "C:/data");
    EXPECT_EQ(out, (QStringList{"--identity-export", QDir::toNativeSeparators("C:/x/me.ashid"), "--identity-dir",
                                QDir::toNativeSeparators("C:/data"), "--ui-control"}));
    EXPECT_EQ(ashdesk::identityArguments(false, "me.ashid", "d").first(), "--identity-import");

    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    control<QPushButton>(window, "startHost")->click();
    // 等假主机自己报出状态：那时它已经把这次启动记进了 calls.txt，下面数的就只是点“导入”以后多出来的
    ASSERT_TRUE(waitUntil([&] { return control<QLabel>(window, "hostStatus")->text() == QStringLiteral("等待连接"); }));
    const int before = static_cast<int>(cores.calls().size());
    auto *import = control<QPushButton>(window, "identityImport");
    EXPECT_EQ(import->accessibleName(), QStringLiteral("导入身份"));
    import->click();
    EXPECT_EQ(control<QLabel>(window, "message")->text(), QStringLiteral("先停掉主机再导入身份"));
    EXPECT_EQ(static_cast<int>(cores.calls().size()), before);
    EXPECT_EQ(window.findChild<QFileDialog *>(), nullptr);
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
}

TEST(DesktopDirect, AddressWithoutStoredKeyConnectsByNumberAndAsksForThePassword) {
    const FakeCores cores;
    cores.write("password.txt", "Summer-2026");
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *client = control<ashdesk::CoreProcess>(window, "clientCore");
    auto *status = control<QLabel>(window, "clientStatus");
    int prompts = 0;
    QString title;
    QString hint;
    QTimer poller;
    QObject::connect(&poller, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("passwordPrompt");
        if (dialog == nullptr || !dialog->isVisible()) {
            return;
        }
        ++prompts;
        title = dialog->windowTitle();
        const auto *reason = dialog->findChild<QLabel *>("passwordHint");
        hint = reason != nullptr ? reason->text() : QString();
        auto *actions = dialog->findChild<QDialogButtonBox *>();
        if (prompts == 3) {
            actions->button(QDialogButtonBox::Cancel)->click();
            return;
        }
        control<QLineEdit>(*dialog, "passwordCode")->setText(" Summer-2026 ");
        actions->button(QDialogButtonBox::Ok)->click();
    });
    constexpr int kPromptPollMs = 20;
    poller.start(kPromptPollMs);
    control<QLineEdit>(window, "address")->setText("192.0.2.9:44100");
    control<QPushButton>(window, "connect")->click();
    ASSERT_TRUE(waitUntil([&] { return status->text() == QStringLiteral("已连接 · 中转"); })) << status->text().toStdString();
    EXPECT_EQ(prompts, 1);
    EXPECT_EQ(title, "192.0.2.9:44100");
    EXPECT_EQ(control<QLabel>(window, "sessionAddress")->text(), "192.0.2.9:44100");
    const QStringList calls = cores.calls();
    ASSERT_GE(calls.size(), 2);
    EXPECT_TRUE(calls[calls.size() - 2].startsWith("--host 192.0.2.9 --port 44100 --by-number --ui-control")) << calls[calls.size() - 2].toStdString();
    EXPECT_TRUE(calls.constLast().startsWith("--host 192.0.2.9 --port 44100 --by-number --password - --ui-control")) << calls.constLast().toStdString();
    EXPECT_EQ(cores.typed(), QStringList{"Summer-2026"});
    for (const QString &call : calls) {
        EXPECT_FALSE(call.contains("Summer-2026") || call.contains("--psk")) << call.toStdString();
    }
    control<QPushButton>(window, "stopClient")->click();
    ASSERT_TRUE(waitUntil([&] { return !client->busy(); }));

    // 对方换了密码：试连要密码、给了旧的，对方说不对，带着原因再问一次；这次取消，停在失败状态，不再连
    cores.write("wrong.txt", "@ui error=20 text=wrong\\spassword");
    cores.write("password.txt", "Winter-2026");
    control<QPushButton>(window, "nav0")->click();
    control<QPushButton>(window, "connect")->click();
    ASSERT_TRUE(waitUntil([&] { return prompts == 3 && !client->busy(); }));
    constexpr int kSettleMs = 100;                       // 取消以后没有再弹、也没有再连
    QTest::qWait(kSettleMs);
    EXPECT_EQ(prompts, 3);
    EXPECT_EQ(hint, "wrong password");
    EXPECT_EQ(status->text(), QStringLiteral("连接失败：wrong password"));
    EXPECT_EQ(cores.typed(), (QStringList{"Summer-2026", "Summer-2026"}));
}

TEST(DesktopNumber, HomeShowsTheHostNumberAndPasswordAndConnectingByNumberAsksForThePassword) {
    const FakeCores cores;
    cores.write("password.txt", "24681357");
    const QString server = ashdesk::signalServer({});
    cores.write("probe.txt", ("@ui probe addr=987654321@" + server + " online=1 busy=0\n").toUtf8());
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *client = control<ashdesk::CoreProcess>(window, "clientCore");
    auto *number = control<QPushButton>(window, "copyNumber");
    auto *password = control<QLabel>(window, "localPassword");
    auto *reveal = control<QPushButton>(window, "showPassword");
    auto *renew = control<QPushButton>(window, "newPassword");
    auto *numberBox = control<QWidget>(window, "localNumber");
    auto *name = control<QLabel>(window, "hostName");
    auto *numberHost = control<QCheckBox>(window, "numberHost");
    auto *message = control<QLabel>(window, "message");
    const QString masked = QStringLiteral("•••• ••••");

    // 主机没在跑：“本机”一行是机器名，没有号码
    EXPECT_TRUE(numberBox->isHidden());
    EXPECT_FALSE(name->isHidden());
    control<QPushButton>(window, "nav2")->click();
    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil([&] { return number->text() == "123 456 789" && !password->text().isEmpty(); }));
    EXPECT_FALSE(numberHost->isEnabled());
    const QString hostCall = cores.calls().constLast();
    EXPECT_TRUE(hostCall.contains("--rendezvous " + server + " --number --number-file ")) << hostCall.toStdString();
    EXPECT_TRUE(hostCall.contains(" --password")) << hostCall.toStdString();
    EXPECT_FALSE(hostCall.contains("--psk-stored")) << hostCall.toStdString();
    control<QPushButton>(window, "nav0")->click();
    EXPECT_FALSE(numberBox->isHidden());
    EXPECT_TRUE(name->isHidden());
    // 密码默认遮着；点开看得到，换一个就变
    EXPECT_EQ(password->text(), masked);
    EXPECT_FALSE(reveal->accessibleName().isEmpty());
    EXPECT_FALSE(renew->accessibleName().isEmpty());
    EXPECT_FALSE(number->accessibleDescription().isEmpty());
    reveal->click();
    EXPECT_EQ(password->text(), "1234 5678");
    snapshot(window, "home-number");
    renew->click();
    ASSERT_TRUE(waitUntil([&] { return password->text() == "8765 4321"; }));
    reveal->click();
    EXPECT_EQ(password->text(), masked);
    // 点号码复制出去的文字：连密码一起（密码遮着也一样），对方整句贴进地址栏就能认出来。
    // 这里不点：界面测试在 Windows 上跑的是真实平台，一点就把这台电脑的剪贴板改了
    EXPECT_EQ(number->toolTip(), QStringLiteral("复制号码和密码"));
    EXPECT_EQ(number->property("share").toString(), QStringLiteral("号码 123 456 789，密码 8765 4321"));
    ashdesk::NumberTarget shared;
    EXPECT_TRUE(ashdesk::parseNumberTarget(number->property("share").toString(), shared));
    EXPECT_EQ(shared.number, "123456789");
    EXPECT_EQ(shared.password, "87654321");
    // 主机报号码还没在信令服务器上登记上：状态栏说明、号码变灰；登记上了恢复
    host->consume("@ui number id=123456789 online=0\n");
    EXPECT_EQ(message->text(), QStringLiteral("没连上信令服务器，别人暂时用不了本机号码"));
    EXPECT_TRUE(number->property("offline").toBool());
    EXPECT_EQ(number->toolTip(), message->text());
    host->consume("@ui number id=123456789 online=1\n");
    EXPECT_FALSE(number->property("offline").toBool());
    EXPECT_EQ(number->toolTip(), QStringLiteral("复制号码和密码"));
    // 带配对窗口重启主机：核心这时不开连接密码，号码还在、密码那几样收起来
    control<QPushButton>(window, "nav2")->click();
    control<QPushButton>(window, "pairTemporary")->click();
    ASSERT_TRUE(waitUntil([&] { return cores.calls().constLast().contains("--pair-temp") && number->text() == "123 456 789"; }));
    EXPECT_FALSE(cores.calls().constLast().contains("--password"));
    EXPECT_TRUE(password->isHidden());
    EXPECT_TRUE(reveal->isHidden());
    EXPECT_TRUE(renew->isHidden());
    // 没有连接密码：只复制号码
    EXPECT_EQ(number->toolTip(), QStringLiteral("复制号码"));
    EXPECT_EQ(number->property("share").toString(), "123456789");
    // 主机停了：换回机器名
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    EXPECT_TRUE(numberBox->isHidden());
    EXPECT_FALSE(name->isHidden());
    // 关掉号码连接：主机照原来的样子起，不登记
    EXPECT_TRUE(numberHost->isEnabled());
    numberHost->setChecked(false);
    EXPECT_FALSE(settings.value("host/number").toBool());
    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil([&] { return control<QLabel>(window, "hostStatus")->text() == QStringLiteral("等待连接"); }));
    EXPECT_FALSE(cores.calls().constLast().contains("--number"));
    EXPECT_TRUE(cores.calls().constLast().contains("--psk-stored"));
    EXPECT_TRUE(numberBox->isHidden());
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));

    // 按号码连（N4）：先不带密码试一次。对方不认得这台设备，核心马上说要密码（error=30），这才问——人还在连接页，
    // 会话页没有闪一下。第一次给错的，核心报密码不对，带着原因再问；第二次给对的（全角数字也认）
    auto *status = control<QLabel>(window, "clientStatus");
    auto *pages = control<QStackedWidget>(window, "pages");
    int prompts = 0;
    QStringList hints;
    QList<int> visited;
    QTimer poller;
    QObject::connect(&poller, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("passwordPrompt");
        if (dialog == nullptr || !dialog->isVisible()) {
            return;
        }
        auto *code = control<QLineEdit>(*dialog, "passwordCode");
        auto *reveal = control<QPushButton>(*dialog, "showPasswordCode");
        auto *actions = dialog->findChild<QDialogButtonBox *>();
        QPushButton *accept = actions->button(QDialogButtonBox::Ok);
        const auto *hint = dialog->findChild<QLabel *>("passwordHint");
        ++prompts;
        hints << (hint != nullptr ? hint->text() : QString());
        // 每次弹出来密码都遮着（同连接页上本机的密码），旁边的“显示”没按下
        EXPECT_EQ(code->echoMode(), QLineEdit::Password) << prompts;
        EXPECT_FALSE(reveal->isChecked()) << prompts;
        if (prompts == 3) {
            actions->button(QDialogButtonBox::Cancel)->click();
            return;
        }
        if (prompts == 1) {
            EXPECT_EQ(dialog->windowTitle(), "987 654 321");
            EXPECT_EQ(code->accessibleName(), QStringLiteral("对方的连接密码"));
            EXPECT_EQ(code->placeholderText(), code->accessibleName());
            // “显示”读屏叫得出名字；按下露出来，再按遮回去
            EXPECT_EQ(reveal->accessibleName(), QStringLiteral("显示连接密码"));
            EXPECT_EQ(reveal->toolTip(), reveal->accessibleName());
            EXPECT_TRUE(reveal->isCheckable());
            reveal->click();
            EXPECT_EQ(code->echoMode(), QLineEdit::Normal);
            reveal->click();
            EXPECT_EQ(code->echoMode(), QLineEdit::Password);
            EXPECT_EQ(pages->currentIndex(), 0);
            EXPECT_TRUE(visited.isEmpty());
            // “正在查找对方…”到弹密码框为止
            EXPECT_TRUE(message->text().isEmpty()) << message->text().toStdString();
            // 试连不算连过：还没进“最近”
            EXPECT_TRUE(ashdesk::loadRecent(settings).isEmpty());
            EXPECT_FALSE(accept->isEnabled());
            // “连接”跟着密码合不合格式亮灭：8 位数字，或者对方设的固定密码
            code->setText("1111 222");
            EXPECT_FALSE(accept->isEnabled());
            code->setText(" Summer-2026 ");
            EXPECT_TRUE(accept->isEnabled());
            code->setText("1234-5678-9");
            EXPECT_FALSE(accept->isEnabled());
            const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
            if (!snapshots.isEmpty()) {
                EXPECT_TRUE(dialog->grab().save(QDir(snapshots).filePath("password-prompt.png")));
            }
        }
        code->setText(prompts == 1 ? QStringLiteral("1111 2222") : QStringLiteral("２４６８ １３５７"));
        EXPECT_TRUE(accept->isEnabled());
        accept->click();
    });
    constexpr int kPromptPollMs = 20;
    poller.start(kPromptPollMs);
    control<QPushButton>(window, "nav0")->click();
    QObject::connect(pages, &QStackedWidget::currentChanged, &window, [&](int page) { visited << page; });
    control<QLineEdit>(window, "address")->setText("987-654-321");
    control<QPushButton>(window, "connect")->click();
    // 试连这一段：真客户端在问信令服务对方在不在、收不收密码（几十毫秒，信令服务不回话时一秒多）。人还在连接页，状态栏说一声
    EXPECT_EQ(message->text(), QStringLiteral("正在查找对方…"));
    EXPECT_EQ(pages->currentIndex(), 0);
    ASSERT_TRUE(waitUntil([&] { return status->text() == QStringLiteral("已连接 · 中转"); })) << status->text().toStdString();
    EXPECT_EQ(prompts, 2);
    EXPECT_EQ(hints, (QStringList{QString(), "wrong password"}));
    EXPECT_EQ(visited, QList<int>{1});
    EXPECT_EQ(control<QLabel>(window, "sessionAddress")->text(), "987 654 321");
    // 三次启动：不带密码的试连，再两次带密码的。密码不在参数表里（任务管理器看得到命令行），是写进核心 stdin 的一行
    const QString byNumber = "--connect 987654321@" + server;
    const QStringList calls = cores.calls();
    ASSERT_GE(calls.size(), 3);
    EXPECT_TRUE(calls[calls.size() - 3].startsWith(byNumber + " --ui-control")) << calls[calls.size() - 3].toStdString();
    EXPECT_TRUE(calls[calls.size() - 2].startsWith(byNumber + " --password - --ui-control")) << calls[calls.size() - 2].toStdString();
    EXPECT_TRUE(calls.constLast().startsWith(byNumber + " --password - --ui-control")) << calls.constLast().toStdString();
    EXPECT_EQ(cores.typed(), (QStringList{"11112222", "24681357"}));
    for (const QString &call : calls) {
        EXPECT_FALSE(call.contains("11112222") || call.contains("24681357")) << call.toStdString();
    }
    for (const char *absent : {"--host", "--psk"}) {
        EXPECT_FALSE(calls.constLast().contains(absent)) << absent;
    }
    // 连过的号码进了“最近”，问在线状态时带上信令服务器
    auto *recent = control<QListWidget>(window, "recent");
    ASSERT_GE(recent->count(), 1);
    EXPECT_EQ(recent->item(0)->text(), "987 654 321");
    ASSERT_TRUE(waitUntil([&] { return recent->item(0)->data(Qt::AccessibleDescriptionRole).toString() == QStringLiteral("在线"); }));
    EXPECT_TRUE(cores.probes().constLast().startsWith("--probe 987654321@" + server));
    control<QPushButton>(window, "stopClient")->click();
    ASSERT_TRUE(waitUntil([&] { return !client->busy(); }));

    // 对方复制过来的“号码 + 密码”整句贴进地址栏：不再问密码，直接连；地址栏里只留号码
    control<QPushButton>(window, "nav0")->click();
    control<QLineEdit>(window, "address")->setText(QStringLiteral("号码 987 654 321，密码 2468 1357"));
    control<QPushButton>(window, "connect")->click();
    ASSERT_TRUE(waitUntil([&] { return status->text() == QStringLiteral("已连接 · 中转"); })) << status->text().toStdString();
    EXPECT_EQ(prompts, 2);
    EXPECT_EQ(control<QLineEdit>(window, "address")->text(), "987 654 321");
    EXPECT_EQ(control<QLabel>(window, "sessionAddress")->text(), "987 654 321");
    // 已经知道密码：只起一次，直接带着密码（核心自己会先试记住的密钥）
    EXPECT_EQ(cores.calls().size(), calls.size() + 1);
    EXPECT_TRUE(cores.calls().constLast().startsWith(byNumber + " --password - --ui-control")) << cores.calls().constLast().toStdString();
    EXPECT_EQ(cores.typed(), (QStringList{"11112222", "24681357", "24681357"}));
    control<QPushButton>(window, "stopClient")->click();
    ASSERT_TRUE(waitUntil([&] { return !client->busy(); }));

    // 不是 9 位的纯数字：当场说明，不拉起核心，也不当成地址去连
    const qsizetype launched = cores.calls().size();
    control<QPushButton>(window, "nav0")->click();
    control<QLineEdit>(window, "address")->setText("12345678");
    control<QPushButton>(window, "connect")->click();
    EXPECT_EQ(message->text(), QStringLiteral("号码是 9 位数字，例如 123 456 789"));
    EXPECT_EQ(cores.calls().size(), launched);
    // 密码框里点取消：只有那一次不带密码的试连，之后不再起核心，人留在连接页
    control<QLineEdit>(window, "address")->setText("987654321");
    control<QPushButton>(window, "connect")->click();
    ASSERT_TRUE(waitUntil([&] { return prompts == 3 && !client->busy(); }));
    constexpr int kSettleMs = 100;
    QTest::qWait(kSettleMs);
    EXPECT_FALSE(client->busy());
    EXPECT_EQ(cores.calls().size(), launched + 1);
    EXPECT_TRUE(cores.calls().constLast().startsWith(byNumber + " --ui-control")) << cores.calls().constLast().toStdString();
    EXPECT_EQ(pages->currentIndex(), 0);
    EXPECT_EQ(hints.constLast(), QString());
    window.close();
}

// N4：对方信任过这台设备（记住了密钥）就不问密码；对方把它吊销了、没开这一种密码，带着原因问密码；
// 对方不在线、两种密码都没开、核心自己出错，照实显示、不问密码
TEST(DesktopNumber, RememberedDeviceConnectsWithoutAPasswordAndTheReasonDecidesWhetherToAsk) {
    const FakeCores cores;
    const QString server = ashdesk::signalServer({});
    const QString byNumber = "--connect 987654321@" + server;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *client = control<ashdesk::CoreProcess>(window, "clientCore");
    auto *status = control<QLabel>(window, "clientStatus");
    auto *message = control<QLabel>(window, "message");
    auto *pages = control<QStackedWidget>(window, "pages");
    auto *address = control<QLineEdit>(window, "address");
    QStringList hints;
    QString answer;
    QTimer poller;
    QObject::connect(&poller, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<QDialog *>("passwordPrompt");
        if (dialog == nullptr || !dialog->isVisible()) {
            return;
        }
        const auto *hint = dialog->findChild<QLabel *>("passwordHint");
        hints << (hint != nullptr ? hint->text() : QString());
        auto *actions = dialog->findChild<QDialogButtonBox *>();
        if (answer.isEmpty()) {
            actions->button(QDialogButtonBox::Cancel)->click();
            return;
        }
        control<QLineEdit>(*dialog, "passwordCode")->setText(std::exchange(answer, {}));
        actions->button(QDialogButtonBox::Ok)->click();
    });
    constexpr int kPromptPollMs = 20;
    constexpr int kSettleMs = 100;
    poller.start(kPromptPollMs);
    const auto connectTo = [&](const QString &text) {
        control<QPushButton>(window, "nav0")->click();
        address->setText(text);
        control<QPushButton>(window, "connect")->click();
    };
    const auto connected = [&] { return status->text() == QStringLiteral("已连接 · 中转"); };
    const auto disconnect = [&] {
        control<QPushButton>(window, "stopClient")->click();
        return waitUntil([&] { return !client->busy(); });
    };
    // 真客户端报原因的那一行：原话照 proto/client/client_text.h 的英文，值里的空格写成 \s（adp_ui_esc）
    const auto reasonLine = [](int code, const QString &text) {
        return QStringLiteral("@ui error=%1 text=%2\n").arg(QString::number(code), QString(text).replace(u' ', QStringLiteral("\\s"))).toUtf8();
    };

    // 对方记得这台设备：一次启动、不带密码、不问，直接连上；连上了才进“最近”、切到会话页，状态栏那句“正在查找对方…”也就收了
    cores.write("known.txt", "trusted");
    connectTo("987 654 321");
    EXPECT_EQ(message->text(), QStringLiteral("正在查找对方…"));
    ASSERT_TRUE(waitUntil(connected)) << status->text().toStdString();
    EXPECT_TRUE(message->text().isEmpty()) << message->text().toStdString();
    EXPECT_TRUE(hints.isEmpty());
    ASSERT_EQ(cores.calls().size(), 1);
    EXPECT_TRUE(cores.calls()[0].startsWith(byNumber + " --ui-control")) << cores.calls()[0].toStdString();
    EXPECT_TRUE(cores.typed().isEmpty());
    EXPECT_EQ(pages->currentIndex(), 1);
    ASSERT_EQ(ashdesk::loadRecent(settings).size(), 1);
    EXPECT_EQ(ashdesk::loadRecent(settings)[0].address, "987 654 321");
    // 会话里对方点了“信任这台设备”，核心说密钥存下来没有
    client->consume("@ui remembered=1\n");
    EXPECT_EQ(message->text(), QStringLiteral("对方已信任这台设备，下次连接不用密码"));
    client->consume("@ui remembered=0\n");
    EXPECT_EQ(message->text(), QStringLiteral("对方已信任这台设备，但密钥没能存下来，下次连接还要密码"));
    ASSERT_TRUE(disconnect());

    // 对方把这台设备吊销了（error=29）：带着核心给的原因问密码，问到了再带着密码起一次。固定密码原样写进 stdin
    cores.write("known.txt", "revoked");
    cores.write("password.txt", "Summer-2026");
    answer = " Summer-2026 ";
    connectTo("987654321");
    ASSERT_TRUE(waitUntil(connected)) << status->text().toStdString();
    EXPECT_EQ(hints, QStringList{"The other computer no longer trusts this device: enter the password"});
    ASSERT_EQ(cores.calls().size(), 3);
    EXPECT_TRUE(cores.calls()[1].startsWith(byNumber + " --ui-control")) << cores.calls()[1].toStdString();
    EXPECT_TRUE(cores.calls()[2].startsWith(byNumber + " --password - --ui-control")) << cores.calls()[2].toStdString();
    EXPECT_EQ(cores.typed(), QStringList{"Summer-2026"});
    EXPECT_FALSE(cores.calls()[2].contains("Summer")) << cores.calls()[2].toStdString();
    ASSERT_TRUE(disconnect());

    // 对方换了密码（22）、连上以后断了（25）、密码格式不对（31）、对方没开这一种密码（32，另一种也许能用）：
    // 和密码不对（20）一样带着原因再问；这里第二次点取消
    cores.remove("known.txt");
    const QList<QPair<int, QString>> askAgain{
        {22, QStringLiteral("Too many wrong passwords: the other computer has a new password now")},
        {25, QStringLiteral("The connection was lost. A password is good for one connection: enter the password again")},
        {31, QStringLiteral("A password is 8 digits, or the fixed password set on the other computer (8–64 letters, digits or symbols)")},
        {32, QStringLiteral("The other computer has turned off its one-time password: use the fixed password set on it")}};
    for (const auto &[code, text] : askAgain) {
        hints.clear();
        cores.write("wrong.txt", reasonLine(code, text));
        const qsizetype before = cores.calls().size();
        connectTo(QStringLiteral("号码 987 654 321，密码 1111 2222"));
        ASSERT_TRUE(waitUntil([&] { return hints.size() == 1 && !client->busy(); })) << code;
        EXPECT_EQ(hints[0], text) << code;
        QTest::qWait(kSettleMs);
        EXPECT_EQ(cores.calls().size(), before + 1) << code;
    }
    // 带着密码连、对方两种密码都没开（21）：问了也没用，不问，会话页照实显示
    constexpr int kNoPasswords = 21;
    const QString closed = QStringLiteral("The other computer accepts no passwords, only devices it remembers");
    hints.clear();
    cores.write("wrong.txt", reasonLine(kNoPasswords, closed));
    qsizetype before = cores.calls().size();
    connectTo(QStringLiteral("号码 987 654 321，密码 1111 2222"));
    ASSERT_TRUE(waitUntil([&] { return cores.calls().size() == before + 1 && !client->busy(); }));
    QTest::qWait(kSettleMs);
    EXPECT_TRUE(hints.isEmpty());
    EXPECT_EQ(status->text(), QStringLiteral("连接失败：") + closed);
    EXPECT_EQ(cores.calls().size(), before + 1);
    cores.remove("wrong.txt");

    // 对方记得这台设备、可是不在线（error=26）：不问密码，会话页照实显示
    const QString offline = QStringLiteral("The other computer is offline, or the number is wrong");
    hints.clear();
    cores.write("known.txt", "offline");
    before = cores.calls().size();
    connectTo("987654321");
    ASSERT_TRUE(waitUntil([&] { return cores.calls().size() == before + 1 && !client->busy(); }));
    QTest::qWait(kSettleMs);
    EXPECT_TRUE(hints.isEmpty());
    EXPECT_EQ(status->text(), QStringLiteral("连接失败：") + offline);
    EXPECT_EQ(pages->currentIndex(), 1);
    EXPECT_EQ(cores.calls().size(), before + 1);

    // 对方没记住这台设备：真客户端先问信令服务（不叫醒对方），在报任何阶段之前就有结论。不在线（26）、两种密码都没开（21）
    // 不问密码：“正在查找对方…”收起，切到会话页照实显示。27 真客户端的试连里报不出来（信令服务不回话也报 30），
    // 这里借它验“别的原因一样照实显示”
    cores.remove("known.txt");
    const QList<QPair<int, QString>> trialFailures{
        {26, offline}, {kNoPasswords, closed}, {27, QStringLiteral("The rendezvous server did not answer")}};
    for (const auto &[code, text] : trialFailures) {
        hints.clear();
        cores.write("presence.txt", reasonLine(code, text));
        before = cores.calls().size();
        connectTo("987654321");
        EXPECT_EQ(message->text(), QStringLiteral("正在查找对方…")) << code;
        EXPECT_EQ(pages->currentIndex(), 0) << code;
        ASSERT_TRUE(waitUntil([&] { return cores.calls().size() == before + 1 && !client->busy(); })) << code;
        QTest::qWait(kSettleMs);
        EXPECT_TRUE(hints.isEmpty()) << code;
        EXPECT_EQ(status->text(), QStringLiteral("连接失败：") + text) << code;
        EXPECT_EQ(pages->currentIndex(), 1) << code;
        EXPECT_TRUE(message->text().isEmpty()) << code << " " << message->text().toStdString();
        EXPECT_TRUE(cores.calls().constLast().startsWith(byNumber + " --ui-control")) << cores.calls().constLast().toStdString();
    }
    cores.remove("presence.txt");

    // 试连时核心自己出了错、什么都没报：不问密码，也不能停在连接页上什么都不说——切到会话页，上面有核心的退出码
    cores.write("known.txt", "broken");
    before = cores.calls().size();
    connectTo("987654321");
    ASSERT_TRUE(waitUntil([&] { return cores.calls().size() == before + 1 && !client->busy(); }));
    QTest::qWait(kSettleMs);
    EXPECT_TRUE(hints.isEmpty());
    EXPECT_EQ(pages->currentIndex(), 1);
    EXPECT_EQ(status->text(), QStringLiteral("核心异常退出（1），请查看诊断"));
    window.close();
}

// 和 RustDesk、ToDesk 一样打开就有号码：程序入口在窗口显示之后调 goOnline，不用先去点“启动主机”
TEST(DesktopNumber, OpeningTheProgramPutsThisComputerOnlineUnlessNumberConnectionIsOff) {
    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *number = control<QPushButton>(window, "copyNumber");
    auto *password = control<QLabel>(window, "localPassword");
    EXPECT_FALSE(host->busy());
    EXPECT_TRUE(control<QWidget>(window, "localNumber")->isHidden());

    window.goOnline();
    ASSERT_TRUE(waitUntil([&] { return number->text() == "123 456 789" && !password->text().isEmpty(); }));
    EXPECT_FALSE(control<QWidget>(window, "localNumber")->isHidden());
    EXPECT_TRUE(control<QLabel>(window, "hostName")->isHidden());
    const qsizetype launched = cores.calls().size();
    EXPECT_TRUE(cores.calls().constLast().contains(" --number --number-file ")) << cores.calls().constLast().toStdString();
    EXPECT_TRUE(cores.calls().constLast().contains(" --password")) << cores.calls().constLast().toStdString();
    // 已经在线：再调一次不另起一个
    window.goOnline();
    EXPECT_EQ(cores.calls().size(), launched);

    // 关了“允许用本机号码连接”：打开时不上线，主机要自己去启动
    control<QPushButton>(window, "nav2")->click();
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    control<QCheckBox>(window, "numberHost")->setChecked(false);
    window.goOnline();
    constexpr int kSettleMs = 100;
    QTest::qWait(kSettleMs);
    EXPECT_FALSE(host->busy());
    EXPECT_EQ(cores.calls().size(), launched);

    // 这个会话里采不到屏幕（远程桌面会话）：主机报完就退出。连接页上没有号码，状态栏要说明为什么，不能只是悄悄没有
    control<QCheckBox>(window, "numberHost")->setChecked(true);
    cores.write("noscreen.txt", "1");
    control<QPushButton>(window, "nav0")->click();
    window.goOnline();
    auto *message = control<QLabel>(window, "message");
    ASSERT_TRUE(waitUntil([&] { return cores.calls().size() == launched + 1 && !host->busy() && !message->text().isEmpty(); }));
    EXPECT_EQ(message->text(), QStringLiteral("这个会话里采不到屏幕，本机没有上线。远程桌面会话里会这样；主机由系统服务运行时不受影响"));
    EXPECT_FALSE(control<QWidget>(window, "status")->isHidden());
    EXPECT_TRUE(control<QWidget>(window, "localNumber")->isHidden());
    window.close();
}

TEST(DesktopPaired, DevicesPageListsPairedDevicesAndChangesGoThroughTheHostCore) {
    const FakeCores cores;
    const QByteArray laptop = QStringLiteral("@ui device id=00112233445566778899aabbccddeeff name=书房\\s笔记本 perms=%1 "
                                             "paired=1762000000 temp=0\n")
                                  .toUtf8();
    const QByteArray phone = "@ui device id=ffeeddccbbaa99887766554433221100 name=Phone perms=%1 paired=1762500000 temp=1\n";
    const auto list = [&](const QByteArray &laptopPerms, const QByteArray &phonePerms) {
        QByteArray devices = QByteArray(laptop).replace("%1", laptopPerms);
        if (!phonePerms.isEmpty()) devices += QByteArray(phone).replace("%1", phonePerms);
        cores.write("devices.txt", devices);
    };
    list("control,files,clipboard", "control,files,clipboard,audio,mic,curtain,restart");
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    EXPECT_FALSE(control<QWidget>(window, "pairedSection")->isHidden());
    const auto box = [&](const char *name) { return window.findChild<QCheckBox *>(name); };
    const auto checked = [&](const char *name) { return box(name) && box(name)->isChecked(); };
    const auto messageIs = [&](const QString &text) { return control<QLabel>(window, "message")->text() == text; };
    control<QPushButton>(window, "nav3")->click();
    ASSERT_TRUE(waitUntil([&] { return box("perm-00112233-control") != nullptr; }));
    QStringList names;
    for (const QLabel *name : window.findChildren<QLabel *>("pairedName")) names << name->text();
    EXPECT_EQ(names, QStringList({QStringLiteral("书房 笔记本"), "Phone"}));
    for (const char *granted : {"perm-00112233-control", "perm-00112233-files", "perm-00112233-clipboard"})
        EXPECT_TRUE(checked(granted)) << granted;
    for (const char *denied : {"perm-00112233-audio", "perm-00112233-mic", "perm-00112233-curtain", "perm-00112233-forward"})
        EXPECT_FALSE(checked(denied)) << denied;
    EXPECT_EQ(window.findChildren<QCheckBox *>(QRegularExpression("^perm-ffeeddcc-")).size(), 8);
    EXPECT_FALSE(control<QPushButton>(window, "revoke-ffeeddcc")->accessibleName().isEmpty());
    EXPECT_TRUE(box("perm-00112233-files")->accessibleName().contains(QStringLiteral("书房 笔记本")));
    snapshot(window, "paired");
    window.resize(640, 480);
    QTest::qWait(30);
    auto *scroll = qobject_cast<QScrollArea *>(control<QStackedWidget>(window, "pages")->currentWidget());
    ASSERT_NE(scroll, nullptr);
    EXPECT_EQ(scroll->horizontalScrollBar()->maximum(), 0);
    snapshot(window, "paired-small");

    // 假核心不改设备表：每次点之前先把“改完以后核心会列出的内容”写好，界面重新取到的就是它。
    // 这一栏从点下去到按新列表重画完之前不可点，等它恢复可点就是重画完了
    auto *panel = control<QWidget>(window, "paired");
    const auto change = [&](const char *name) {
        box(name)->click();
        EXPECT_FALSE(panel->isEnabled());
        return waitUntil([&] { return panel->isEnabled(); });
    };
    list("control,clipboard", "control,files,clipboard,audio,mic,curtain,restart");
    ASSERT_TRUE(change("perm-00112233-files"));
    EXPECT_TRUE(messageIs(QStringLiteral("设备权限已更新，下次连接生效")));
    EXPECT_FALSE(checked("perm-00112233-files"));
    EXPECT_TRUE(checked("perm-00112233-clipboard"));
    EXPECT_TRUE(cores.calls().contains("--device-perm 00112233445566778899aabbccddeeff control,clipboard"));
    // 核心列设备时八项全有也是逐项列出来，不写 all
    const QByteArray everything = "control,files,clipboard,audio,mic,curtain,forward,restart";
    list("control,clipboard", everything);
    ASSERT_TRUE(change("perm-ffeeddcc-forward"));
    EXPECT_TRUE(checked("perm-ffeeddcc-forward"));
    EXPECT_TRUE(cores.calls().contains("--device-perm ffeeddccbbaa99887766554433221100 all"));
    list("clipboard", everything);
    ASSERT_TRUE(change("perm-00112233-control"));
    list("view", everything);
    ASSERT_TRUE(change("perm-00112233-clipboard"));
    EXPECT_FALSE(checked("perm-00112233-clipboard"));
    EXPECT_TRUE(cores.calls().contains("--device-perm 00112233445566778899aabbccddeeff view"));

    list("view", {});
    QTimer::singleShot(0, &window, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        dialog->button(QMessageBox::Yes)->click();
    });
    control<QPushButton>(window, "revoke-ffeeddcc")->click();
    ASSERT_TRUE(waitUntil([&] { return panel->isEnabled(); }));
    EXPECT_TRUE(messageIs(QStringLiteral("已吊销 Phone")));
    EXPECT_EQ(box("perm-ffeeddcc-control"), nullptr);
    EXPECT_NE(box("perm-00112233-control"), nullptr);
    EXPECT_TRUE(cores.calls().contains("--revoke ffeeddccbbaa99887766554433221100"));

    cores.write("devices.txt", {});
    control<QPushButton>(window, "nav3")->click();
    ASSERT_TRUE(waitUntil([&] { return !box("perm-00112233-control"); }));
    QStringList empty;
    for (const QLabel *label : control<QWidget>(window, "pairedSection")->findChildren<QLabel *>()) empty << label->text();
    EXPECT_TRUE(empty.contains(QStringLiteral("尚无已配对设备")));
    window.close();
}

// N4：主机在跑的时候设备表在它手里。另起进程改文件它不知道（吊销不生效，下次存盘还会盖回去），
// 所以列表、改权限、吊销都经控制管道问它；主机停了才照旧另起 adrd_host --devices 这类进程
TEST(DesktopPaired, RunningHostOwnsTheDeviceTableAndAnswersThroughTheControlPipe) {
    const FakeCores cores;
    const QByteArray laptop = QStringLiteral("@ui device id=00112233445566778899aabbccddeeff name=书房\\s笔记本 perms=%1 "
                                             "paired=1762000000 temp=0\n")
                                  .toUtf8();
    const QByteArray phone = "@ui device id=ffeeddccbbaa99887766554433221100 name=Phone perms=control,files paired=1762500000 temp=1\n";
    const auto list = [&](const QByteArray &laptopPerms, bool withPhone) {
        cores.write("devices.txt", QByteArray(laptop).replace("%1", laptopPerms) + (withPhone ? phone : QByteArray()));
    };
    list("control,files,clipboard", true);
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostLog = control<QPlainTextEdit>(window, "hostLog");
    auto *message = control<QLabel>(window, "message");
    auto *panel = control<QWidget>(window, "paired");
    const auto box = [&](const char *name) { return window.findChild<QCheckBox *>(name); };
    const auto checked = [&](const char *name) { return box(name) != nullptr && box(name)->isChecked(); };
    const auto sent = [&](const char *command) { return commandsSeen(*hostLog).contains(QLatin1String(command)); };
    const auto change = [&](const char *name) {
        box(name)->click();
        EXPECT_FALSE(panel->isEnabled());
        return waitUntil([&] { return panel->isEnabled(); });
    };
    const auto revoke = [&](const char *name, const QString &outcome) {
        QTimer::singleShot(0, &window, [] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            dialog->button(QMessageBox::Yes)->click();
        });
        control<QPushButton>(window, name)->click();
        return waitUntil([&] { return panel->isEnabled() && message->text() == outcome; });
    };
    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil([&] { return control<QLabel>(window, "hostStatus")->text() == QStringLiteral("等待连接"); }));

    // 打开设备页：问在跑的主机，一台一行，收到 devices n=… 才画
    control<QPushButton>(window, "nav3")->click();
    ASSERT_TRUE(waitUntil([&] { return box("perm-00112233-control") != nullptr; }));
    EXPECT_TRUE(sent("devices"));
    QStringList names;
    for (const QLabel *name : window.findChildren<QLabel *>("pairedName")) {
        names << name->text();
    }
    EXPECT_EQ(names, QStringList({QStringLiteral("书房 笔记本"), "Phone"}));
    EXPECT_TRUE(checked("perm-00112233-files"));
    EXPECT_FALSE(checked("perm-00112233-audio"));

    // 改权限：发 device-perm，主机答了再按它的设备表重画
    list("control,clipboard", true);
    ASSERT_TRUE(change("perm-00112233-files"));
    EXPECT_EQ(message->text(), QStringLiteral("设备权限已更新，下次连接生效"));
    EXPECT_FALSE(checked("perm-00112233-files"));
    EXPECT_TRUE(sent("device-perm 00112233445566778899aabbccddeeff control,clipboard"));
    // 主机说没改成：照说，再按主机手里的实际内容画回去
    cores.write("device-perm.txt", "@ui device-perm ok=0\n");
    ASSERT_TRUE(change("perm-00112233-clipboard"));
    EXPECT_EQ(message->text(), QStringLiteral("设备权限修改失败"));
    EXPECT_TRUE(checked("perm-00112233-clipboard"));
    EXPECT_TRUE(sent("device-perm 00112233445566778899aabbccddeeff control"));
    cores.remove("device-perm.txt");

    // 吊销：确认以后发 revoke（设备号是完整的）
    list("control,clipboard", false);
    ASSERT_TRUE(revoke("revoke-ffeeddcc", QStringLiteral("已吊销 Phone")));
    EXPECT_TRUE(sent("revoke ffeeddccbbaa99887766554433221100"));
    EXPECT_EQ(box("perm-ffeeddcc-control"), nullptr);
    EXPECT_NE(box("perm-00112233-control"), nullptr);
    // 吊销了、可是设备表存不下来；设备表里已经没有这台：各说一句
    cores.write("revoked.txt", "@ui revoked ok=0 id=00112233 error=store\n");
    ASSERT_TRUE(revoke("revoke-00112233", QStringLiteral("已吊销，但设备表没能保存，主机重启后这台设备会回来")));
    cores.write("revoked.txt", "@ui revoked ok=0 error=none\n");
    ASSERT_TRUE(revoke("revoke-00112233", QStringLiteral("设备表里已经没有这台设备")));
    cores.remove("revoked.txt");
    // 从头到尾没有另起改设备表的进程：假核心只被拉起过主机那一次
    ASSERT_EQ(cores.calls().size(), 1);

    // 主机停了：设备表在盘上，照旧另起进程去列、去改
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    list("control", false);
    control<QPushButton>(window, "nav3")->click();
    ASSERT_TRUE(waitUntil([&] { return !checked("perm-00112233-clipboard"); }));
    EXPECT_TRUE(cores.calls().contains("--devices --ui-control"));
    ASSERT_TRUE(revoke("revoke-00112233", QStringLiteral("已吊销 书房 笔记本")));
    EXPECT_TRUE(cores.calls().contains("--revoke 00112233445566778899aabbccddeeff"));
    window.close();
}

// N4（号码连接设计 §4.5）：用密码连进来的设备，被控端可以点“信任这台设备”，以后它按号码连不用输密码。
// 确认后发的是弹框那一刻那台的设备号（trust 设备号）：确认框开着时会话换了人，主机认得出来、不信任新来的那台
TEST(DesktopTrust, PasswordSessionOffersTrustOnTheHostPageAndInTheTrayAndReportsTheOutcome) {
    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostLog = control<QPlainTextEdit>(window, "hostLog");
    auto *message = control<QLabel>(window, "message");
    auto *trust = control<QPushButton>(window, "trustDevice");
    const auto *trayTrust = window.findChild<QAction *>("trayTrust");           // 桌面没有系统托盘时没有这一项
    const QByteArray laptop = "00112233445566778899aabbccddeeff";
    const QByteArray phone = "ffeeddccbbaa99887766554433221100";
    // 主机的 phase 行（照真主机的 ui_report）：用密码连进来、对方记得住密钥的临时设备带 trust=1 和它的设备号，别的 trust=0
    const auto session = [](const QByteArray &name, const QByteArray &device) {
        return "@ui phase=stream peer=192.0.2.7:50000 name=" + name + " perms=control,files trust=" +
               (device.isEmpty() ? QByteArray("0") : "1 dev=" + device) + "\n";
    };
    const auto offered = [&] { return !trust->isHidden() && (trayTrust == nullptr || trayTrust->isVisible()); };
    const auto withdrawn = [&] { return trust->isHidden() && (trayTrust == nullptr || !trayTrust->isVisible()); };
    // 发给主机的 trust 命令，一次一项
    const auto requests = [&] { return commandsSeen(*hostLog).filter(QRegularExpression(QStringLiteral("^trust( |$)"))); };
    // 挂了的时候 gtest 把 QString 打成一串 UTF-16 字节，比 std::string 看得清
    const auto trusting = [](const QByteArray &device) { return "trust " + device.toStdString(); };
    QStringList questions;
    const auto answer = [&](QMessageBox::StandardButton button) {
        QTimer::singleShot(0, &window, [&questions, button] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            ASSERT_NE(dialog, nullptr);
            questions << dialog->text();
            dialog->button(button)->click();
        });
    };
    constexpr int kSettleMs = 150;
    control<QPushButton>(window, "nav2")->click();
    control<QPushButton>(window, "startHost")->click();
    ASSERT_TRUE(waitUntil([&] { return control<QLabel>(window, "hostStatus")->text() == QStringLiteral("等待连接"); }));
    EXPECT_TRUE(withdrawn());

    // 已配对的设备、用共享密钥连进来的（trust=0；老主机不带这一项）：没有入口
    host->consume(session("Laptop", {}));
    EXPECT_TRUE(withdrawn());
    host->consume("@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control,files\n");
    EXPECT_TRUE(withdrawn());
    // trust=1 却不带设备号：是不认 trust 设备号 的旧版主机，界面发不出它认得的命令，也不给入口
    host->consume("@ui phase=stream peer=192.0.2.7:50000 name=Laptop perms=control,files trust=1\n");
    EXPECT_TRUE(withdrawn());
    // 用密码连进来的临时设备：被控端页的会话那一块和托盘菜单里各有一个入口
    host->consume(session("Laptop", laptop));
    EXPECT_TRUE(offered());
    EXPECT_EQ(trust->text(), QStringLiteral("信任这台设备"));
    snapshot(window, "host-trust");

    // 点了先确认，说清楚后果；取消就什么都不发
    answer(QMessageBox::Cancel);
    trust->click();
    ASSERT_EQ(questions.size(), 1);
    EXPECT_EQ(questions[0], QStringLiteral("信任 Laptop？以后它按号码连接本机不用输密码，可以在设备页吊销。"));
    QTest::qWait(kSettleMs);
    EXPECT_TRUE(requests().isEmpty());
    // 确认了才发，带着这一台的设备号。主机答成了：状态栏说一声；它随后把 phase 那一行带着 trust=0 再发一遍，入口收起
    cores.write("trusted.txt", "@ui trusted ok=1 name=Laptop id=00112233\n" + session("Laptop", {}));
    answer(QMessageBox::Yes);
    trust->click();
    ASSERT_TRUE(waitUntil([&] { return requests().size() == 1 && withdrawn(); }));
    EXPECT_EQ(requests().constLast().toStdString(), trusting(laptop));
    EXPECT_EQ(message->text(), QStringLiteral("已信任 Laptop"));
    // 结果都说在状态栏里：读屏读到的得是这句话本身，不是一个固定的名字
    auto *spoken = QAccessible::queryAccessibleInterface(message);
    ASSERT_NE(spoken, nullptr);
    EXPECT_EQ(spoken->text(QAccessible::Name), message->text());
    cores.remove("trusted.txt");

    // 确认框开着的时候会话换了人：Laptop 断开，Phone 用密码连进来（新的 phase 行照常进来）。点“信任”发出去的还是
    // 弹框时 Laptop 的设备号；主机认出不是现在这台，回 changed：界面说明没有信任新来的。入口照新的 phase 行，Phone 也能信任
    const QString changed = QStringLiteral("确认时的那台已经断开，换了别的设备连进来，没有信任新来的这台");
    cores.write("trusted.txt", "@ui trusted ok=0 error=changed\n");
    host->consume(session("Laptop", laptop));
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        ASSERT_NE(dialog, nullptr);
        questions << dialog->text();
        host->consume("@ui phase=listening\n" + session("Phone", phone));
        dialog->button(QMessageBox::Yes)->click();
    });
    trust->click();
    ASSERT_TRUE(waitUntil([&] { return message->text() == changed; })) << message->text().toStdString();
    EXPECT_TRUE(questions.constLast().startsWith(QStringLiteral("信任 Laptop？"))) << questions.constLast().toStdString();
    ASSERT_EQ(requests().size(), 2);
    EXPECT_EQ(requests().constLast().toStdString(), trusting(laptop));
    EXPECT_TRUE(offered());
    cores.remove("trusted.txt");
    // 再点一次，确认的就是现在这台：发的是 Phone 的设备号
    cores.write("trusted.txt", "@ui trusted ok=1 name=Phone id=ffeeddcc\n" + session("Phone", {}));
    answer(QMessageBox::Yes);
    trust->click();
    ASSERT_TRUE(waitUntil([&] { return requests().size() == 3 && withdrawn(); }));
    EXPECT_TRUE(questions.constLast().startsWith(QStringLiteral("信任 Phone？"))) << questions.constLast().toStdString();
    EXPECT_EQ(requests().constLast().toStdString(), trusting(phone));
    EXPECT_EQ(message->text(), QStringLiteral("已信任 Phone"));
    cores.remove("trusted.txt");

    // 托盘菜单里的入口同样先确认、带设备号。这回主机说现在没有用密码连进来的会话（假主机本来就没有会话）
    if (trayTrust != nullptr) {
        host->consume(session("Laptop", laptop));
        answer(QMessageBox::Yes);
        window.findChild<QAction *>("trayTrust")->trigger();
        ASSERT_TRUE(waitUntil([&] { return message->text() == QStringLiteral("现在没有用密码连进来的设备，没有可以信任的"); }));
        ASSERT_EQ(requests().size(), 4);
        EXPECT_EQ(requests().constLast().toStdString(), trusting(laptop));
        EXPECT_EQ(questions.size(), 5);
    }
    // 没成的三种原因各说一句；界面不认得的原因也有话说
    host->consume("@ui trusted ok=0 error=old-client\n");
    EXPECT_NE(message->text(), QStringLiteral("现在没有用密码连进来的设备，没有可以信任的"));
    host->consume("@ui trusted ok=0 error=none\n");
    EXPECT_EQ(message->text(), QStringLiteral("现在没有用密码连进来的设备，没有可以信任的"));
    host->consume("@ui trusted ok=0 error=old-client\n");
    EXPECT_EQ(message->text(), QStringLiteral("对方的程序太旧，记不住密钥，没法信任它"));
    host->consume("@ui trusted ok=0 error=store\n");
    EXPECT_EQ(message->text(), QStringLiteral("设备表没能保存，没有信任成功"));
    host->consume("@ui trusted ok=0 error=later\n");
    EXPECT_EQ(message->text(), QStringLiteral("没有信任成功"));

    // 会话结束、主机停了：入口收起
    host->consume(session("Laptop", laptop));
    EXPECT_TRUE(offered());
    host->consume("@ui phase=listening\n");
    EXPECT_TRUE(withdrawn());
    host->consume(session("Laptop", laptop));
    EXPECT_TRUE(offered());
    control<QPushButton>(window, "stopHost")->click();
    ASSERT_TRUE(waitUntil([&] { return !host->busy(); }));
    EXPECT_TRUE(withdrawn());
    window.close();
}

// N4（号码连接设计 §4.3）：设置 → 安全里的固定密码。设没设由在跑的主机说了算；界面只把新密码交给它，自己不存
TEST(DesktopFixedPassword, SecurityTabSetsAndClearsItThroughTheRunningHostAndExplainsFailures) {
    const FakeCores cores;
    const QString path = cores.directory.filePath("desktop.ini");
    QSettings settings(path, QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostLog = control<QPlainTextEdit>(window, "hostLog");
    auto *message = control<QLabel>(window, "message");
    auto *state = control<QLabel>(window, "fixedState");
    auto *input = control<QLineEdit>(window, "fixedPassword");
    auto *reveal = control<QPushButton>(window, "showFixed");
    auto *set = control<QPushButton>(window, "setFixed");
    auto *clear = control<QPushButton>(window, "clearFixed");
    auto *numberHost = control<QCheckBox>(window, "numberHost");
    const auto start = [&] {
        control<QPushButton>(window, "startHost")->click();
        return waitUntil([&] { return control<QLabel>(window, "hostStatus")->text() == QStringLiteral("等待连接"); });
    };
    const auto stop = [&] {
        control<QPushButton>(window, "stopHost")->click();
        return waitUntil([&] { return !host->busy(); });
    };
    const auto sent = [&](const char *command) { return commandsSeen(*hostLog).contains(QLatin1String(command)); };
    const QString unset = QStringLiteral("未设置");
    const QString isSet = QStringLiteral("已设置");
    control<QPushButton>(window, "nav4")->click();
    control<QTabWidget>(window, "settingsTabs")->setCurrentIndex(3);

    // 每个控件读屏都叫得出名字；显示“设没设”的标签不另设名字（设了会盖掉它的文字）
    EXPECT_FALSE(input->accessibleName().isEmpty());
    EXPECT_FALSE(reveal->accessibleName().isEmpty());
    EXPECT_FALSE(set->accessibleName().isEmpty());
    EXPECT_FALSE(clear->accessibleName().isEmpty());
    EXPECT_TRUE(state->accessibleName().isEmpty());
    EXPECT_EQ(input->echoMode(), QLineEdit::Password);

    // 主机没在跑：没处设。几个控件不可用，提示里说为什么
    EXPECT_FALSE(input->isEnabled());
    EXPECT_FALSE(reveal->isEnabled());
    EXPECT_FALSE(set->isEnabled());
    EXPECT_FALSE(clear->isEnabled());
    EXPECT_TRUE(state->text().isEmpty());
    EXPECT_EQ(input->toolTip(), QStringLiteral("主机在运行时才能设置固定密码"));
    EXPECT_EQ(set->toolTip(), input->toolTip());
    EXPECT_EQ(clear->toolTip(), input->toolTip());
    // 主机在跑、可是没开号码连接（也就没开连接密码）：一样不可用，原因不同
    numberHost->setChecked(false);
    ASSERT_TRUE(start());
    EXPECT_FALSE(input->isEnabled());
    EXPECT_FALSE(set->isEnabled());
    EXPECT_TRUE(state->text().isEmpty());
    EXPECT_EQ(input->toolTip(), QStringLiteral("这个主机没开连接密码，用不了固定密码"));
    EXPECT_EQ(set->toolTip(), input->toolTip());
    ASSERT_TRUE(stop());
    numberHost->setChecked(true);

    // 主机报还没设：可以输了；不合规则的设不了——太短、8 位数字（那是临时密码的格式）、只有数字和连字符
    ASSERT_TRUE(start());
    ASSERT_TRUE(waitUntil([&] { return state->text() == unset; }));
    EXPECT_TRUE(input->isEnabled());
    EXPECT_TRUE(reveal->isEnabled());
    EXPECT_TRUE(input->toolTip().isEmpty());
    EXPECT_FALSE(set->isEnabled());
    EXPECT_FALSE(clear->isEnabled());
    for (const char *weak : {"short-1", "12345678", "1234-5678-90"}) {
        input->setText(weak);
        EXPECT_FALSE(set->isEnabled()) << weak;
    }
    input->setText(" Summer-2026 ");
    EXPECT_TRUE(set->isEnabled());
    // 默认遮着，点眼睛才显示
    reveal->click();
    EXPECT_EQ(input->echoMode(), QLineEdit::Normal);
    snapshot(window, "security-fixed");

    // 设置：密码（去掉首尾空白）交给主机，输入框当场清空——正显示着的时候按撤销也找不回来；主机说设好了才显示“已设置”
    set->click();
    EXPECT_TRUE(input->text().isEmpty());
    input->undo();
    EXPECT_TRUE(input->text().isEmpty());
    reveal->click();
    EXPECT_EQ(input->echoMode(), QLineEdit::Password);
    ASSERT_TRUE(waitUntil([&] { return state->text() == isSet; }));
    EXPECT_TRUE(sent("password fixed Summer-2026"));
    EXPECT_EQ(message->text(), QStringLiteral("固定密码已设置"));
    EXPECT_TRUE(clear->isEnabled());
    EXPECT_FALSE(set->isEnabled());
    // 清除
    clear->click();
    ASSERT_TRUE(waitUntil([&] { return state->text() == unset; }));
    EXPECT_TRUE(sent("password fixed-clear"));
    EXPECT_EQ(message->text(), QStringLiteral("固定密码已清除"));
    EXPECT_FALSE(clear->isEnabled());

    // 主机说没成的四种原因各说一句，设没设照它报的来；界面认不得的原因也不吞掉。
    // 没开连接密码（off）的主机报的 set 说明不了什么：这一行收起来，和没报过一样
    const QList<QPair<QByteArray, QString>> failures{
        {"set=0 error=weak", QStringLiteral("固定密码要 8–64 个英文字母、数字或符号，不能只有数字、空格和连字符")},
        {"set=1 error=store", QStringLiteral("固定密码没能存取，原因见被控端的日志")},
        {"set=0 error=off", QStringLiteral("这个主机没开连接密码，用不了固定密码")},
        {"set=0 error=renumbered", QStringLiteral("本机换了号码，原来的固定密码作废了，请重新设置")},
        {"set=1 error=locked", QStringLiteral("固定密码没有改成（locked）")}};
    for (const auto &[report, text] : failures) {
        host->consume("@ui fixed " + report + "\n");
        const bool off = report.endsWith("error=off");
        EXPECT_EQ(message->text(), text) << report.toStdString();
        EXPECT_EQ(state->text(), off ? QString() : report.startsWith("set=1") ? isSet : unset) << report.toStdString();
        EXPECT_EQ(input->isEnabled(), !off) << report.toStdString();
        EXPECT_EQ(input->toolTip(), off ? text : QString()) << report.toStdString();
    }

    // 新密码只交给了主机：界面的设置文件里没有它
    settings.sync();
    QFile stored(path);
    ASSERT_TRUE(stored.open(QIODevice::ReadOnly));
    EXPECT_FALSE(stored.readAll().contains("Summer"));

    // 主机停了：设没设不知道了，控件收回去
    ASSERT_TRUE(stop());
    EXPECT_FALSE(input->isEnabled());
    EXPECT_FALSE(clear->isEnabled());
    EXPECT_TRUE(state->text().isEmpty());
    // 以前设过的（盘上有派生值）：主机一启动就报设着，界面不把它当成自己刚设的去说“已设置”
    cores.write("fixed.txt", "1");
    ASSERT_TRUE(start());
    ASSERT_TRUE(waitUntil([&] { return state->text() == isSet; }));
    EXPECT_TRUE(clear->isEnabled());
    EXPECT_TRUE(message->text().isEmpty()) << message->text().toStdString();
    // 输完按回车等于点“设置”
    input->setText("Winter-2027");
    QTest::keyClick(input, Qt::Key_Return);
    EXPECT_TRUE(input->text().isEmpty());
    ASSERT_TRUE(waitUntil([&] { return sent("password fixed Winter-2027"); }));
    ASSERT_TRUE(stop());
    window.close();
}

// N4（号码连接设计 §4.3）：设置 → 安全里临时密码的两个开关。两项都存在主机上：界面把改动发给在跑的主机，开关照主机报回来的摆。
// 临时密码关着时主机不出码（@ui password code= 空着），连接页收起密码，复制出去的只有号码
TEST(DesktopTempPassword, SwitchesFollowTheRunningHostAndTheHomePageDropsThePasswordWhileItIsOff) {
    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostLog = control<QPlainTextEdit>(window, "hostLog");
    auto *message = control<QLabel>(window, "message");
    auto *temp = control<QCheckBox>(window, "tempPassword");
    auto *keep = control<QCheckBox>(window, "tempKeep");
    auto *number = control<QPushButton>(window, "copyNumber");
    auto *password = control<QLabel>(window, "localPassword");
    auto *reveal = control<QPushButton>(window, "showPassword");
    auto *renew = control<QPushButton>(window, "newPassword");
    auto *numberHost = control<QCheckBox>(window, "numberHost");
    const auto start = [&] {
        control<QPushButton>(window, "startHost")->click();
        return waitUntil([&] { return control<QLabel>(window, "hostStatus")->text() == QStringLiteral("等待连接"); });
    };
    const auto stop = [&] {
        control<QPushButton>(window, "stopHost")->click();
        return waitUntil([&] { return !host->busy(); });
    };
    const auto sent = [&](const char *command) { return commandsSeen(*hostLog).count(QLatin1String(command)); };
    // 点一个开关，等主机的答复说出一句话（先把状态栏清掉：存截图时切主题也会说“设置已保存”）
    const auto change = [&](QCheckBox *toggle) {
        window.message({});
        toggle->click();
        return waitUntil([&] { return !message->text().isEmpty(); });
    };
    const QString saved = QStringLiteral("设置已保存");
    const QString bothOff = QStringLiteral("临时密码关着，也没设固定密码：现在只有信任过的设备能用号码连进来");
    control<QPushButton>(window, "nav4")->click();
    control<QTabWidget>(window, "settingsTabs")->setCurrentIndex(3);

    // 两个开关读屏都叫得出名字
    for (QCheckBox *toggle : {temp, keep}) {
        auto *spoken = QAccessible::queryAccessibleInterface(toggle);
        ASSERT_NE(spoken, nullptr);
        EXPECT_FALSE(spoken->text(QAccessible::Name).isEmpty());
    }
    // 主机没在跑：没处改。开关不可用，提示里说为什么
    EXPECT_FALSE(temp->isEnabled());
    EXPECT_FALSE(keep->isEnabled());
    EXPECT_EQ(temp->toolTip(), QStringLiteral("主机在运行时才能设置临时密码"));
    EXPECT_EQ(keep->toolTip(), temp->toolTip());
    // 主机在跑、可是没开号码连接（也就没开连接密码，不报 @ui temp）：一样不可用，原因不同
    numberHost->setChecked(false);
    ASSERT_TRUE(start());
    EXPECT_FALSE(temp->isEnabled());
    EXPECT_FALSE(keep->isEnabled());
    EXPECT_EQ(temp->toolTip(), QStringLiteral("这个主机没开连接密码，用不了临时密码"));
    EXPECT_EQ(keep->toolTip(), temp->toolTip());
    ASSERT_TRUE(stop());
    numberHost->setChecked(true);

    // 主机报临时密码开着、用过就换：开关照着摆，可以改了。主机启动时报的那一条不说“设置已保存”
    ASSERT_TRUE(start());
    ASSERT_TRUE(waitUntil([&] { return temp->isEnabled(); }));
    EXPECT_TRUE(temp->isChecked());
    EXPECT_TRUE(keep->isEnabled());
    EXPECT_FALSE(keep->isChecked());
    EXPECT_TRUE(temp->toolTip().isEmpty());
    EXPECT_TRUE(message->text().isEmpty()) << message->text().toStdString();

    // 用过以后不换：发 password temp keep，主机答了才算
    ASSERT_TRUE(change(keep));
    EXPECT_EQ(message->text(), saved);
    EXPECT_EQ(sent("password temp keep"), 1);
    EXPECT_TRUE(keep->isChecked());
    snapshot(window, "security-temp");

    // 关掉临时密码：主机不出码了。连接页收起密码那几样，复制出去的只有号码；“用过以后不换”还是原来的值，只是关着时不可改。
    // 固定密码也没设：状态栏提醒现在只有信任过的设备能连进来
    ASSERT_TRUE(change(temp));
    EXPECT_EQ(message->text(), bothOff);
    EXPECT_EQ(sent("password temp off"), 1);
    EXPECT_FALSE(temp->isChecked());
    EXPECT_TRUE(keep->isChecked());
    EXPECT_FALSE(keep->isEnabled());
    EXPECT_TRUE(password->isHidden());
    EXPECT_TRUE(reveal->isHidden());
    EXPECT_TRUE(renew->isHidden());
    EXPECT_EQ(number->text(), "123 456 789");
    EXPECT_EQ(number->toolTip(), QStringLiteral("复制号码"));
    EXPECT_EQ(number->property("share").toString(), "123456789");
    snapshot(window, "security-temp-off");
    control<QPushButton>(window, "nav0")->click();
    snapshot(window, "home-temp-off");
    control<QPushButton>(window, "nav4")->click();

    // 开回来：主机出一个新码，连接页又有密码，复制的又是号码加密码
    ASSERT_TRUE(change(temp));
    EXPECT_EQ(message->text(), saved);
    EXPECT_EQ(sent("password temp on"), 1);
    EXPECT_TRUE(temp->isChecked());
    EXPECT_TRUE(keep->isEnabled());
    EXPECT_TRUE(keep->isChecked());
    EXPECT_FALSE(password->isHidden());
    EXPECT_FALSE(renew->isHidden());
    EXPECT_EQ(number->property("share").toString(), QStringLiteral("号码 123 456 789，密码 1357 2468"));
    // 改回用过就换
    ASSERT_TRUE(change(keep));
    EXPECT_EQ(message->text(), saved);
    EXPECT_EQ(sent("password temp rotate"), 1);
    EXPECT_FALSE(keep->isChecked());

    // 主机存不下来（error=store）：设置没改，开关照主机报的原值摆回去，状态栏说明
    cores.write("temp-store.txt", "1");
    ASSERT_TRUE(change(temp));
    EXPECT_EQ(message->text(), QStringLiteral("临时密码的设置没能保存，原因见被控端的日志"));
    EXPECT_EQ(sent("password temp off"), 2);
    EXPECT_TRUE(temp->isChecked());
    EXPECT_FALSE(password->isHidden());
    cores.remove("temp-store.txt");
    // 界面认不得的原因也不吞掉
    host->consume("@ui temp on=1 keep=0 error=locked\n");
    EXPECT_EQ(message->text(), QStringLiteral("临时密码的设置没有改成（locked）"));
    EXPECT_TRUE(temp->isEnabled());
    // 主机说没开连接密码（error=off，行里是默认值，不是它的设置）：开关收起来，和没报过一样
    host->consume("@ui temp on=1 keep=0 error=off\n");
    EXPECT_EQ(message->text(), QStringLiteral("这个主机没开连接密码，用不了临时密码"));
    EXPECT_FALSE(temp->isEnabled());
    EXPECT_FALSE(keep->isEnabled());
    EXPECT_EQ(temp->toolTip(), message->text());

    // 主机停了：不知道了，开关收回去。重启时主机报它存着的设置（关着、用过以后不换）：开关照摆，不说“设置已保存”，
    // 连接页没有密码
    ASSERT_TRUE(stop());
    EXPECT_FALSE(temp->isEnabled());
    EXPECT_FALSE(temp->isChecked());
    cores.write("temp.txt", "off keep");
    cores.write("fixed.txt", "1");
    ASSERT_TRUE(start());
    ASSERT_TRUE(waitUntil([&] { return temp->isEnabled(); }));
    EXPECT_FALSE(temp->isChecked());
    EXPECT_TRUE(keep->isChecked());
    EXPECT_FALSE(keep->isEnabled());
    EXPECT_TRUE(message->text().isEmpty()) << message->text().toStdString();
    EXPECT_TRUE(password->isHidden());
    EXPECT_EQ(number->property("share").toString(), "123456789");
    // 临时密码关着时清掉固定密码，同样提醒；设上了就不提醒
    ASSERT_TRUE(waitUntil([&] { return control<QPushButton>(window, "clearFixed")->isEnabled(); }));
    window.message({});
    control<QPushButton>(window, "clearFixed")->click();
    ASSERT_TRUE(waitUntil([&] { return !message->text().isEmpty(); }));
    EXPECT_EQ(message->text(), bothOff);
    window.message({});
    control<QLineEdit>(window, "fixedPassword")->setText("Summer-2026");
    control<QPushButton>(window, "setFixed")->click();
    ASSERT_TRUE(waitUntil([&] { return !message->text().isEmpty(); }));
    EXPECT_EQ(message->text(), QStringLiteral("固定密码已设置"));
    ASSERT_TRUE(stop());
    window.close();
}

TEST(DesktopNetdiag, ItemsFillInAsTheCoreReportsAndTheRendezvousServerIsRemembered) {
    const FakeCores cores;
    cores.write("netdiag.txt", "@ui diag item=nat ok=1 value=port-restricted\n@ui diag item=udp ok=1 value=reachable\n"
                               "@ui diag item=tcp ok=0 value=blocked\n@ui diag item=portmap ok=0 value=no\\sgateway\n"
                               "@ui diag item=verdict value=relay text=Relay\\sneeded\n");
    const QString path = cores.directory.filePath("desktop.ini");
    {
        QSettings settings(path, QSettings::IniFormat);
        ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
        window.show();
        control<QPushButton>(window, "nav4")->click();
        auto *tabs = control<QTabWidget>(window, "settingsTabs");
        tabs->setCurrentIndex(tabs->count() - 1);
        auto *results = control<QWidget>(window, "netdiagResults");
        auto *start = control<QPushButton>(window, "startNetdiag");
        auto *verdict = control<QLabel>(window, "netdiagVerdict");
        const auto value = [&](const char *item) { return control<QLabel>(window, item)->text(); };
        EXPECT_TRUE(results->isHidden());
        control<QLineEdit>(window, "rendezvous")->setText(" rdv.example.net:44300 ");
        EXPECT_EQ(settings.value("network/rendezvous").toString(), "rdv.example.net:44300");
        start->click();
        EXPECT_FALSE(start->isEnabled());
        ASSERT_TRUE(waitUntil([&] { return start->isEnabled(); }));
        EXPECT_TRUE(results->isVisible());
        EXPECT_EQ(verdict->text(), "Relay needed");
        EXPECT_EQ(value("diag-nat"), QStringLiteral("✓ port-restricted"));
        EXPECT_EQ(value("diag-udp"), QStringLiteral("✓ reachable"));
        EXPECT_EQ(value("diag-tcp"), QStringLiteral("✗ blocked"));
        EXPECT_EQ(value("diag-portmap"), QStringLiteral("✗ no gateway"));
        EXPECT_EQ(value("diag-relay_rtt"), QStringLiteral("—"));
        // 防火墙那一项是界面自己查系统规则得来的，这台机器装没装都要有个明确结果
        EXPECT_TRUE(value("diag-firewall").startsWith(QStringLiteral("✓")) || value("diag-firewall").startsWith(QStringLiteral("✗")));
        EXPECT_EQ(cores.calls().constLast(), "--netdiag --port 44100 --ui-control --rendezvous rdv.example.net:44300");
        snapshot(window, "netdiag");

        // 核心没给结论就退出了（比如旧核心不认 --netdiag）
        cores.write("netdiag.txt", "usage: adrd_host ...\n");
        control<QLineEdit>(window, "rendezvous")->clear();
        start->click();
        ASSERT_TRUE(waitUntil([&] { return start->isEnabled(); }));
        EXPECT_EQ(verdict->text(), QStringLiteral("诊断没有完成"));
        EXPECT_EQ(value("diag-nat"), QStringLiteral("—"));
        // 没填信令服务器：号码连接开着时测的是内置的那一台；关掉号码连接就不测中转
        EXPECT_EQ(cores.calls().constLast(), "--netdiag --port 44100 --ui-control --rendezvous " + ashdesk::signalServer({}));
        control<QCheckBox>(window, "numberHost")->setChecked(false);
        start->click();
        ASSERT_TRUE(waitUntil([&] { return start->isEnabled(); }));
        EXPECT_EQ(cores.calls().constLast(), "--netdiag --port 44100 --ui-control");
        control<QLineEdit>(window, "rendezvous")->setText("rdv.example.net:44300");
        window.close();
    }
    QSettings settings(path, QSettings::IniFormat);
    ashdesk::MainWindow reopened(settings, cores.directory.path(), cores.directory.filePath("data"));
    EXPECT_EQ(control<QLineEdit>(reopened, "rendezvous")->text(), "rdv.example.net:44300");
}

TEST(DesktopNetdiag, FirewallQueryAnswersForAProgramWithoutARule) {
    QElapsedTimer timer;
    timer.start();
    const ashdesk::FirewallRule rule = ashdesk::hostFirewallRule("C:/no/such/folder/adrd_host.exe");
    std::printf("[ perf ] firewall rule query %.1f ms\n", double(timer.nsecsElapsed()) / 1e6);
#ifdef Q_OS_WIN
    // 规则读得出来就是“没有”；防火墙服务被停掉的机器上读不出来，是“查不了”
    EXPECT_NE(rule, ashdesk::FirewallRule::Allowed);
#else
    EXPECT_EQ(rule, ashdesk::FirewallRule::Unknown);
#endif
}

TEST(DesktopDrivers, VirtualDisplaySwitchPersistsAndAddsCaptureIddToTheHostArguments) {
    ashdesk::DesktopOptions options;
    EXPECT_FALSE(ashdesk::hostArguments(options).contains("--capture"));
    options.virtualDisplay = true;
    const QStringList host = ashdesk::hostArguments(options);
    ASSERT_TRUE(host.contains("--capture"));
    EXPECT_EQ(host[host.indexOf("--capture") + 1], "idd");
    EXPECT_FALSE(ashdesk::clientArguments({"192.0.2.1", 44100}, options).contains("--capture"));

    QTemporaryDir directory;
    const QString path = directory.filePath("desktop.ini");
    {
        QSettings settings(path, QSettings::IniFormat);
        ashdesk::MainWindow window(settings, directory.path());
        auto *toggle = control<QCheckBox>(window, "virtualDisplay");
        EXPECT_FALSE(toggle->isChecked());
        toggle->click();
        EXPECT_TRUE(ashdesk::DesktopOptions::load(settings).virtualDisplay);
        window.close();
    }
    QSettings restored(path, QSettings::IniFormat);
    ashdesk::MainWindow reopened(restored, directory.path());
    auto *toggle = control<QCheckBox>(reopened, "virtualDisplay");
    EXPECT_TRUE(toggle->isChecked());
    toggle->click();
    EXPECT_FALSE(ashdesk::DesktopOptions::load(restored).virtualDisplay);
}

TEST(DesktopDrivers, MissingDriverIsOfferedOncePerHostRunAndWithoutAnInstallScriptTheCommandIsShown) {
    const FakeCores cores;
    QSettings settings(cores.directory.filePath("desktop.ini"), QSettings::IniFormat);
    ashdesk::MainWindow window(settings, cores.directory.path(), cores.directory.filePath("data"));
    window.show();
    auto *host = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *start = control<QPushButton>(window, "startHost");
    auto *message = control<QLabel>(window, "message");
    control<QCheckBox>(window, "virtualDisplay")->click();
    control<QPushButton>(window, "nav2")->click();
    DriverPrompts prompts(window, {QMessageBox::Cancel, QMessageBox::Yes}, "driver-prompt");

    // 假主机带着 --capture idd 起来，报缺虚拟屏驱动后退出。第一次问：取消，界面什么都不做
    start->click();
    ASSERT_TRUE(waitUntil([&] { return prompts.seen.size() == 1 && !host->busy(); }));
    ASSERT_EQ(cores.calls().size(), 1);
    EXPECT_TRUE(cores.calls()[0].contains("--capture idd")) << cores.calls()[0].toStdString();
    EXPECT_TRUE(prompts.seen[0].contains(QStringLiteral("虚拟屏驱动还没有安装，现在安装？")));
    EXPECT_TRUE(prompts.seen[0].contains(QStringLiteral("安装")));
    EXPECT_FALSE(message->text().contains("install.ps1")) << message->text().toStdString();
    // 同一次主机运行里再报一遍（真主机重试打开采集时会）不再问；界面不认识的驱动不问
    host->consume("@ui need driver=vdisplay\n@ui need driver=usb\n");
    QTest::qWait(150);
    EXPECT_EQ(prompts.seen.size(), 1);
    // 驱动在、只是主机没有管理员权限：不问装不装，只说明原因
    host->consume("@ui need admin=vdisplay\n");
    QTest::qWait(150);
    EXPECT_EQ(prompts.seen.size(), 1);
    EXPECT_TRUE(message->text().contains(QStringLiteral("管理员"))) << message->text().toStdString();

    // 再启动主机是新的一次运行，又问。点安装：测试环境里没有 install.ps1，界面不提权，只说要跑哪条命令
    start->click();
    ASSERT_TRUE(waitUntil([&] { return prompts.seen.size() == 2 && !host->busy(); }));
    EXPECT_TRUE(message->text().contains("install.ps1 -Drivers vdisplay")) << message->text().toStdString();
    EXPECT_EQ(cores.calls().size(), 2);
    // 不用 snapshot()：它切主题会存一次设置，状态栏的话就被“设置已保存”换掉了
    const QString snapshots = qEnvironmentVariable("ASHDESK_UI_SNAPSHOT_DIR");
    if (!snapshots.isEmpty()) {
        EXPECT_TRUE(window.grab().save(QDir(snapshots).filePath("driver-no-script.png")));
    }

    // 虚拟声卡是另一个驱动，各问各的
    host->consume("@ui need driver=vaudio\n");
    ASSERT_TRUE(waitUntil([&] { return prompts.seen.size() == 3; }));
    EXPECT_TRUE(prompts.seen[2].contains(QStringLiteral("虚拟声卡驱动还没有安装，放不出对方的麦克风。现在安装？")));
    window.close();
}

TEST(DesktopDrivers, InstallCommandRunsTheScriptOfTheInstallDirectoryWithQuotedPaths) {
    QTemporaryDir directory;
    const QDir root(directory.path());
    ASSERT_TRUE(root.mkpath("Ash Desk/bin"));
    // 上一级没有 install.ps1 的不是安装出来的布局
    EXPECT_TRUE(ashdesk::installDirectory(root.filePath("Ash Desk/bin")).isEmpty());
    QFile script(root.filePath("Ash Desk/install.ps1"));
    ASSERT_TRUE(script.open(QIODevice::WriteOnly));
    script.close();
    EXPECT_EQ(ashdesk::installDirectory(root.filePath("Ash Desk/bin")), root.filePath("Ash Desk"));

    const ashdesk::ElevatedCommand command = ashdesk::driverInstallCommand("C:/Program Files/AshDesk", "vdisplay");
    EXPECT_EQ(command.program, "powershell.exe");
    // 期望值不能直接写进 EXPECT_EQ：宏把参数转成字符串时，MSVC 会把原始字符串里的反斜杠当转义（C4129）
    const QString parameters = R"(-NoProfile -ExecutionPolicy Bypass -File "C:\Program Files\AshDesk\install.ps1" )"
                               R"(-InPlace -Drivers vdisplay -SkipOpenH264)";
    EXPECT_EQ(command.parameters, parameters);
    EXPECT_TRUE(ashdesk::driverInstallCommand("D:/AshDesk", "vaudio").parameters.endsWith(" -Drivers vaudio -SkipOpenH264"));
}

TEST(DesktopDrivers, CertificateIsTheSingleCerFileInTheDriverFolder) {
    const QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QDir root(directory.path());
    ASSERT_TRUE(root.mkpath("drivers/vdisplay"));
    // 正式签名的驱动包里没有证书：装驱动的命令不带它
    EXPECT_TRUE(ashdesk::driverCertificate(root.path(), "vdisplay").isEmpty());
    EXPECT_FALSE(ashdesk::driverInstallCommand(root.path(), "vdisplay").parameters.contains("-TestCertificate"));
    QFile first(root.filePath("drivers/vdisplay/AshVD.cer"));
    ASSERT_TRUE(first.open(QIODevice::WriteOnly));
    first.close();
    const QString certificate = root.filePath("drivers/vdisplay/AshVD.cer");
    EXPECT_EQ(ashdesk::driverCertificate(root.path(), "vdisplay"), certificate);
    EXPECT_TRUE(ashdesk::driverInstallCommand(root.path(), "vdisplay")
                    .parameters.endsWith(" -TestCertificate " + ashdesk::quotedArgument(QDir::toNativeSeparators(certificate))));
    EXPECT_TRUE(ashdesk::driverCertificate(root.path(), "vaudio").isEmpty());
    // 认不出是哪一张就不带，不替用户挑
    QFile second(root.filePath("drivers/vdisplay/other.cer"));
    ASSERT_TRUE(second.open(QIODevice::WriteOnly));
    second.close();
    EXPECT_TRUE(ashdesk::driverCertificate(root.path(), "vdisplay").isEmpty());
    EXPECT_FALSE(ashdesk::driverInstallCommand(root.path(), "vdisplay").parameters.contains("-TestCertificate"));
}

TEST(DesktopService, KnowsWhichCopyTheServiceRunsFrom) {
    const QString quoted = R"("E:\AshDesk-portable-20261126-0875f46\bin\adrd_svc.exe" service)";
    const QString program = R"(E:\AshDesk-portable-20261126-0875f46\bin\adrd_svc.exe)";
    EXPECT_EQ(ashdesk::serviceProgram(quoted), program);
    // 没加引号的 binPath：到第一个空格为止
    const QString bareProgram = R"(C:\AshDesk\bin\adrd_svc.exe)";
    EXPECT_EQ(ashdesk::serviceProgram(bareProgram + " service"), bareProgram);
    // 和界面程序（这里是测试程序）所在的目录比：大小写、斜杠方向都不算不同
    const QString here = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/adrd_svc.exe");
    const auto line = [](const QString &program) { return QStringLiteral("\"") + program + QStringLiteral("\" service"); };
    EXPECT_TRUE(ashdesk::serviceRunsHere(line(here)));
    EXPECT_TRUE(ashdesk::serviceRunsHere(line(here.toUpper())));
    EXPECT_TRUE(ashdesk::serviceRunsHere(line(QDir::fromNativeSeparators(here))));
    EXPECT_FALSE(ashdesk::serviceRunsHere(quoted));
    // 没装
    EXPECT_FALSE(ashdesk::serviceRunsHere({}));
}

TEST(DesktopService, QuotesOnlyWhatTheWindowsCommandLineWouldSplit) {
    // 期望值放进表里再比：原始字符串直接写进 EXPECT_EQ，MSVC 会把里面的反斜杠当转义（C4129）。
    // 反斜杠只在引号前要成对：结尾的、引号前的翻倍，路径中间的不动
    const std::array<std::pair<QString, QString>, 7> cases{{
        {"--port", "--port"},
        {R"(C:\ProgramData\AshDesk\number.txt)", R"(C:\ProgramData\AshDesk\number.txt)"},
        {"", R"("")"},
        {R"(C:\My Keys\k.txt)", R"("C:\My Keys\k.txt")"},
        {R"(C:\My Keys\)", R"("C:\My Keys\\")"},
        {R"(a"b)", R"("a\"b")"},
        {R"(a\"b)", R"("a\\\"b")"},
    }};
    for (const auto &[argument, quoted] : cases) {
        EXPECT_EQ(ashdesk::quotedArgument(argument), quoted) << argument.toStdString();
    }
}

TEST(DesktopService, HostArgumentsKeepTheSettingsButLiveInTheServiceDirectory) {
    ashdesk::DesktopOptions options;
    options.hostControl = true;
    const QString directory = QDir::fromNativeSeparators(R"(C:\ProgramData\AshDesk)");
    const QStringList service = ashdesk::serviceHostArguments(options, directory).value_or(QStringList{});
    ASSERT_FALSE(service.isEmpty());
    // 和界面自己起的主机一样，只是 --ui-control 由服务加、号码和日志在服务的目录里
    QStringList expected = ashdesk::hostArguments(options, ashdesk::Pairing::None, {false, R"(C:\ProgramData\AshDesk\number.txt)"});
    expected.removeAll("--ui-control");
    expected << "--log" << R"(C:\ProgramData\AshDesk\host.log)";
    EXPECT_EQ(service, expected);
    EXPECT_TRUE(service.contains("--inject"));
    EXPECT_TRUE(service.contains("--password"));
    EXPECT_FALSE(service.contains("--psk-stored"));
    // 密钥文件 SYSTEM 也读得到
    options.authentication = 1;
    options.keyFile = QDir::fromNativeSeparators(R"(C:\My Keys\k.txt)");
    const QStringList keyed = ashdesk::serviceHostArguments(options, directory).value_or(QStringList{});
    ASSERT_FALSE(keyed.isEmpty());
    EXPECT_EQ(keyed.value(keyed.indexOf("--psk-file") + 1), QFileInfo(options.keyFile).absoluteFilePath());
}

TEST(DesktopService, SettingsTheServiceCannotReadAreRefused) {
    ashdesk::DesktopOptions options;
    const QString directory = QDir::fromNativeSeparators(R"(C:\ProgramData\AshDesk)");
    // 不开号码连接、用存在这个账户里的共享密钥
    options.numberHost = false;
    options.authentication = 0;
    EXPECT_FALSE(ashdesk::serviceHostArguments(options, directory));
    // 二次验证密钥也存在这个账户里
    options.numberHost = true;
    options.totp = 1;
    EXPECT_FALSE(ashdesk::serviceHostArguments(options, directory));
    options.totp = 0;
    EXPECT_TRUE(ashdesk::serviceHostArguments(options, directory));
    // 找不到 ProgramData
    EXPECT_FALSE(ashdesk::serviceHostArguments(options, {}));
}

#ifdef Q_OS_WIN
namespace {

QStringList splitCommandLine(const QString &line) {
    int count = 0;
    const std::wstring text = line.toStdWString();
    LPWSTR *const parts = CommandLineToArgvW(text.c_str(), &count);
    QStringList out;
    for (int i = 0; i < count; ++i) {
        out << QString::fromWCharArray(*std::next(parts, i));
    }
    LocalFree(static_cast<HLOCAL>(parts));
    return out;
}

} // namespace

// 引号套两层：Windows 拆一次得到 -ServiceHostArgs 的值（install.ps1 收到的那一行），服务把这一行接在主机命令行后面再拆一次
TEST(DesktopService, InstallCommandCarriesEveryHostArgumentThroughBothSplits) {
    const QStringList host{"--port", "44100", "--psk-file", R"(C:\My Keys\k.txt)", "--rendezvous", "5.253.17.21:44200",
                           "--forward-allow", "", "--log", R"(C:\ProgramData\AshDesk\host.log)"};
    const ashdesk::ElevatedCommand command = ashdesk::serviceInstallCommand("D:/Ash Desk", host);
    EXPECT_EQ(command.program, "powershell.exe");
    const QStringList outer = splitCommandLine("powershell.exe " + command.parameters);
    const QStringList head{"powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", R"(D:\Ash Desk\install.ps1)",
                           "-InPlace", "-Service", "-AllowSas", "-SkipOpenH264", "-ServiceHostArgs"};
    ASSERT_EQ(outer.size(), head.size() + 1);
    EXPECT_EQ(outer.mid(0, head.size()), head);
    EXPECT_EQ(splitCommandLine("adrd_host.exe " + outer.last()).mid(1), host);
    const ashdesk::ElevatedCommand remove = ashdesk::serviceUninstallCommand("D:/Ash Desk");
    const QString svc = R"(D:\Ash Desk\bin\adrd_svc.exe)";
    EXPECT_EQ(remove.program, svc);
    EXPECT_EQ(remove.parameters, "uninstall");
}

// 第一层是 Windows PowerShell 自己拆的：用同样的命令行跑一个只回显参数的脚本，它收到的必须正好是那一行
TEST(DesktopService, WindowsPowerShellReceivesTheHostArgumentLineIntact) {
    const QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QFile probe(QDir(directory.path()).filePath("install.ps1"));
    ASSERT_TRUE(probe.open(QIODevice::WriteOnly));
    probe.write("param([switch]$InPlace, [switch]$Service, [switch]$AllowSas, [switch]$SkipOpenH264, [string]$ServiceHostArgs)\r\n"
                "[Console]::OutputEncoding = [Text.Encoding]::UTF8\r\n"
                "[Console]::Out.Write($ServiceHostArgs)\r\n");
    probe.close();
    const QStringList host{"--psk-file", R"(C:\My Keys\k.txt)", "--number-file", R"(C:\ProgramData\AshDesk\number.txt)", "--forward-allow", ""};
    const ashdesk::ElevatedCommand command = ashdesk::serviceInstallCommand(directory.path(), host);
    std::array<wchar_t, MAX_PATH> system{};
    ASSERT_NE(GetSystemDirectoryW(system.data(), MAX_PATH), 0U);
    QProcess shell;
    shell.setProgram(QString::fromWCharArray(system.data()) + R"(\WindowsPowerShell\v1.0\powershell.exe)");
    shell.setNativeArguments(command.parameters);
    // 开发机上从 PowerShell 7 继承来的 PSModulePath 会让 5.1 找不到自带的模块
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove("PSModulePath");
    shell.setProcessEnvironment(environment);
    shell.start();
    ASSERT_TRUE(shell.waitForFinished(30000));
    EXPECT_EQ(shell.exitCode(), 0) << shell.readAllStandardError().toStdString();
    const QString received = QString::fromUtf8(shell.readAllStandardOutput());
    EXPECT_EQ(splitCommandLine("adrd_host.exe " + received).mid(1), host) << received.toStdString();
}
#endif

#ifdef Q_OS_WIN
TEST(DesktopCore, ClientClosesItsViewerThroughTheControlPipe) {
    const QString program = QDir(QCoreApplication::applicationDirPath()).filePath("adrd_client.exe");
    if (!QFileInfo::exists(program)) GTEST_SKIP() << "Standalone UI build has no Windows client";
    QUdpSocket reservation;
    ASSERT_TRUE(reservation.bind(QHostAddress::LocalHost, 0));
    const QString port = QString::number(reservation.localPort());
    ashdesk::CoreProcess client;
    QString output;
    QStringList phases;
    bool launched = false;
    QObject::connect(&client, &ashdesk::CoreProcess::output, &client, [&](const QString &text) { output += text; });
    QObject::connect(&client, &ashdesk::CoreProcess::launched, &client, [&] { launched = true; });
    QObject::connect(&client, &ashdesk::CoreProcess::message, &client, [&](const ashdesk::CoreMessage &message) {
        if (message.topic == "phase") phases << message.value("phase");
    });
    ASSERT_TRUE(client.start(program, {"--host", "127.0.0.1", "--port", port, "--ui-control",
                                      "--no-input", "--no-audio", "--clip", "off", "--files", "off"}));
    // 没有主机在听：真客户端报的是连接前的某个阶段，不会是 stream
    ASSERT_TRUE(waitUntil([&] { return launched && !phases.isEmpty(); })) << output.toStdString();
    for (const QString &phase : std::as_const(phases))
        EXPECT_TRUE(QStringList({"lookup", "handshake", "negotiate", "configure"}).contains(phase)) << phase.toStdString();
    EXPECT_FALSE(output.contains("@ui "));
    client.stop();
    ASSERT_TRUE(waitUntil([&] { return !client.busy(); })) << output.toStdString();
    EXPECT_EQ(client.status(), QStringLiteral("已停止")) << output.toStdString();
    EXPECT_TRUE(output.contains("SUMMARY")) << output.toStdString();
}

// N4：真客户端按号码连、对方没回话就有结论的两种情况，界面那边的流程就是照这两条写的（假核心也是照它们抄的）。
// 记住设备的文件给一个空的临时位置，不读用户自己的那份；信令服务是本机一个只收不答的端口，不连公网
TEST(DesktopCore, ClientByNumberNeedsAPasswordUnlessRememberedAndReadsItFromStdin) {
    const QString program = QDir(QCoreApplication::applicationDirPath()).filePath("adrd_client.exe");
    if (!QFileInfo::exists(program)) {
        GTEST_SKIP() << "Standalone UI build has no Windows client";
    }
    const QTemporaryDir directory;
    const QStringList common{"--ui-control",   "--known-file", directory.filePath("known.dpapi"), "--device-file", directory.filePath("device.dpapi"),
                             "--no-input",     "--no-audio",   "--clip",                          "off",           "--files",
                             "off"};
    QUdpSocket silent;
    ASSERT_TRUE(silent.bind(QHostAddress::LocalHost, 0));
    const QStringList target{"--connect", QStringLiteral("123456789@127.0.0.1:%1").arg(silent.localPort())};
    const auto run = [&](const QStringList &arguments, const QString &typed, QList<ashdesk::CoreMessage> &messages) {
        ashdesk::CoreProcess client;
        int code = -2;
        QObject::connect(&client, &ashdesk::CoreProcess::finished, &client, [&](int exitCode) { code = exitCode; });
        QObject::connect(&client, &ashdesk::CoreProcess::message, &client,
                         [&](const ashdesk::CoreMessage &message) { messages << message; });
        QObject::connect(&client, &ashdesk::CoreProcess::launched, &client, [&] {
            if (!typed.isEmpty()) {
                client.send(typed);
            }
        });
        EXPECT_TRUE(client.start(program, arguments + common));
        EXPECT_TRUE(waitUntil([&] { return code != -2; }));
        return code;
    };
    // 不带密码、对方也没记住这台设备：先问信令服务对方在不在、收不收密码（不叫醒对方）。信令服务一直不答：
    // 问几次、一秒多以后照样报 error=30，退出码 3；在这之前不报任何阶段（界面靠这一点留在连接页上）
    constexpr qint64 kRetriedMs = 1000;              // 不答的话客户端要重问，比一次往返长得多
    constexpr qint64 kGaveUpMs = 8000;               // 一直等下去就是坏了
    QList<ashdesk::CoreMessage> needed;
    QElapsedTimer timer;
    timer.start();
    EXPECT_EQ(run(target, {}, needed), 3);
    const qint64 waited = timer.elapsed();
    std::cout << "[ perf ] client without a password gives up on a silent rendezvous server after " << waited << " ms\n";
    EXPECT_GE(waited, kRetriedMs);
    EXPECT_LT(waited, kGaveUpMs);
    ASSERT_EQ(needed.size(), 1);
    EXPECT_EQ(needed[0].topic, "error");
    EXPECT_EQ(needed[0].value("error"), "30");
    EXPECT_FALSE(needed[0].value("text").isEmpty());
    // 问的确实是 --connect 里的那台信令服务，问的是那个号码
    int queries = 0;
    while (silent.hasPendingDatagrams()) {
        const QByteArray query = silent.receiveDatagram().data();
        EXPECT_TRUE(query.contains("123456789")) << query.toHex().toStdString();
        ++queries;
    }
    EXPECT_GE(queries, 1);
    std::cout << "[ info ] " << queries << " lookups sent\n";
    // --password -：密码是界面写进 stdin 的那一行。格式不对（7 位数字）在本地就拒绝：error=31，退出码 3，不去问信令服务
    QList<ashdesk::CoreMessage> malformed;
    EXPECT_EQ(run(target + QStringList{"--password", "-"}, "1234567", malformed), 3);
    ASSERT_EQ(malformed.size(), 1);
    EXPECT_EQ(malformed[0].value("error"), "31");
    EXPECT_FALSE(malformed[0].value("text").isEmpty());
    EXPECT_FALSE(silent.hasPendingDatagrams());
}

TEST(DesktopCore, HostReportsListeningAndATemporaryCodeWritesItsLogAndListsDevices) {
    const QString program = QDir(QCoreApplication::applicationDirPath()).filePath("adrd_host.exe");
    if (!QFileInfo::exists(program)) GTEST_SKIP() << "Standalone UI build has no Windows host";
    QTemporaryDir directory;
    QUdpSocket reservation;
    ASSERT_TRUE(reservation.bind(QHostAddress::LocalHost, 0));
    const QString port = QString::number(reservation.localPort());
    reservation.close();
    const QStringList store{"--device-store", directory.filePath("devices.dpapi")};
    const QString log = ashdesk::coreLogFile(directory.filePath("logs"), "host");
    ashdesk::CoreProcess host;
    QString output;
    QList<ashdesk::CoreMessage> messages;
    QObject::connect(&host, &ashdesk::CoreProcess::output, &host, [&](const QString &text) { output += text; });
    QObject::connect(&host, &ashdesk::CoreProcess::message, &host,
                     [&](const ashdesk::CoreMessage &message) { messages << message; });
    const auto reported = [&](const QString &topic, const QString &key, const QString &value) {
        return std::any_of(messages.cbegin(), messages.cend(), [&](const ashdesk::CoreMessage &message) {
            return message.topic == topic && message.value(key) == value;
        });
    };
    ASSERT_TRUE(host.start(program, QStringList{"--ui-control", "--bind", "127.0.0.1", "--port", port, "--test-pattern",
                                                "160x96", "--tp-cpu", "--encoder", "ae264", "--keep-desktop", "--cleartype",
                                                "--audio", "off", "--clip", "off", "--files", "off", "--takeover-confirm",
                                                "never", "--audit-log", directory.filePath("audit.log"), "--pair-temp",
                                                "--log", log} +
                                        store));
    ASSERT_TRUE(waitUntil([&] { return reported("phase", "phase", "listening") && reported("pair", "temp", "1"); }))
        << output.toStdString();
    const auto pairing = std::find_if(messages.cbegin(), messages.cend(),
                                      [](const ashdesk::CoreMessage &message) { return message.topic == "pair"; });
    EXPECT_TRUE(QRegularExpression("^\\d{6}$").match(pairing->value("code")).hasMatch()) << pairing->value("code").toStdString();
    EXPECT_EQ(pairing->value("ttl"), "600");
    EXPECT_FALSE(output.contains("@ui "));
    host.stop();
    ASSERT_TRUE(waitUntil([&] { return !host.busy(); })) << output.toStdString();
    EXPECT_EQ(host.status(), QStringLiteral("已停止")) << output.toStdString();
    EXPECT_GT(QFileInfo(log).size(), 0);

    // 还没有设备配上：列表是空的，退出码 0，除状态行外不往界面的管道写别的
    ashdesk::CoreProcess list;
    int code = -2;
    int devices = 0;
    QObject::connect(&list, &ashdesk::CoreProcess::finished, &list, [&](int exitCode) { code = exitCode; });
    QObject::connect(&list, &ashdesk::CoreProcess::message, &list, [&](const ashdesk::CoreMessage &message) {
        if (message.topic == "device") ++devices;
    });
    ASSERT_TRUE(list.start(program, QStringList{"--devices", "--ui-control"} + store));
    ASSERT_TRUE(waitUntil([&] { return code != -2; }));
    EXPECT_EQ(code, 0);
    EXPECT_EQ(devices, 0);
}

// 号码连接设计 N3：真的信令服务 + 真的主机。主机登记上以后报本机号码和连接密码，界面要它换一个密码就换。
// N4：两种密码的设置也照界面发的命令改、照格式报回来；真客户端不带密码试连时，信令服务按主机登记的状态给它结论
TEST(DesktopCore, HostReportsItsNumberAndPasswordAndIssuesANewPasswordOnRequest) {
    const QDir programs(QCoreApplication::applicationDirPath());
    const QString program = programs.filePath("adrd_host.exe");
    const QString server = programs.filePath("adrd_rdv.exe");
    const QString client = programs.filePath("adrd_client.exe");
    if (!QFileInfo::exists(program) || !QFileInfo::exists(server) || !QFileInfo::exists(client)) {
        GTEST_SKIP() << "Standalone UI build has no Windows host";
    }
    const QTemporaryDir directory;
    QUdpSocket reservation;
    QUdpSocket hostReservation;
    ASSERT_TRUE(reservation.bind(QHostAddress::LocalHost, 0));
    ASSERT_TRUE(hostReservation.bind(QHostAddress::LocalHost, 0));
    const QString serverPort = QString::number(reservation.localPort());
    const QString hostPort = QString::number(hostReservation.localPort());
    reservation.close();
    hostReservation.close();
    QProcess rendezvous;
    rendezvous.setProcessChannelMode(QProcess::MergedChannels);
    rendezvous.start(server, {"--port", serverPort, "--tcp-port", "0"});
    ASSERT_TRUE(rendezvous.waitForStarted());

    ashdesk::CoreProcess host;
    QString output;
    QStringList numbers;
    QStringList passwords;
    QStringList registered;
    QStringList fixed;                               // N4：每条 @ui fixed 记成 "set值:error值"
    QStringList temps;                               // 每条 @ui temp 记成 "on值:keep值:error值"
    QStringList trusts;                              // 每条 @ui trusted 记成 "ok值:error值"
    QStringList listed;                              // 每条 @ui devices 的 n
    QObject::connect(&host, &ashdesk::CoreProcess::output, &host, [&](const QString &text) { output += text; });
    QObject::connect(&host, &ashdesk::CoreProcess::message, &host, [&](const ashdesk::CoreMessage &message) {
        if (message.topic == "fixed") {
            fixed << message.value("set") + ':' + message.value("error");
        }
        if (message.topic == "trusted") {
            trusts << message.value("ok") + ':' + message.value("error");
        }
        if (message.topic == "temp") {
            temps << message.value("on") + ':' + message.value("keep") + ':' + message.value("error");
        }
        if (message.topic == "devices") {
            listed << message.value("n");
        }
        if (message.topic == "number") {
            numbers << message.value("id");
        }
        if (message.topic == "number" && message.values.contains("online")) {
            registered << message.value("online");
        }
        if (message.topic == "password") {
            passwords << message.value("code");
        }
    });
    const QString numberFile = directory.filePath("number.txt");
    ASSERT_TRUE(host.start(program, {"--ui-control", "--bind", "127.0.0.1", "--port", hostPort, "--test-pattern", "160x96", "--tp-cpu",
                                     "--encoder", "ae264", "--keep-desktop", "--cleartype", "--audio", "off", "--clip", "off",
                                     "--files", "off", "--takeover-confirm", "never", "--rendezvous", "127.0.0.1:" + serverPort,
                                     "--number", "--number-file", numberFile, "--password", "--device-store",
                                     directory.filePath("devices.dpapi"), "--log", ashdesk::coreLogFile(directory.filePath("logs"), "host")}));
    ASSERT_TRUE(waitUntil([&] { return !numbers.isEmpty() && !passwords.isEmpty(); })) << output.toStdString();
    EXPECT_TRUE(QRegularExpression("^[1-9]\\d{8}$").match(numbers.constLast()).hasMatch()) << numbers.constLast().toStdString();
    EXPECT_TRUE(QRegularExpression("^\\d{8}$").match(passwords.constLast()).hasMatch());
    // 号码和证明它归本机的密钥落在交给主机的文件里，不碰用户的 %LOCALAPPDATA%
    QFile stored(numberFile);
    ASSERT_TRUE(stored.open(QIODevice::ReadOnly));
    EXPECT_EQ(QString::fromLatin1(stored.readAll()).trimmed(), numbers.constLast());
    ASSERT_TRUE(waitUntil([&] { return QFileInfo::exists(numberFile + ".key"); }));
    // 登记上了要说一声：号码显示得出来不等于别人找得到
    ASSERT_TRUE(waitUntil([&] { return !registered.isEmpty(); })) << output.toStdString();
    EXPECT_EQ(registered, QStringList{"1"});
    const QString first = passwords.constLast();
    host.send("password new");
    ASSERT_TRUE(waitUntil([&] { return passwords.constLast() != first; })) << output.toStdString();
    EXPECT_TRUE(QRegularExpression("^\\d{8}$").match(passwords.constLast()).hasMatch());
    // N4：界面这一节靠假核心测的那几条命令，在真主机上的答复就是假核心照抄的格式。
    // 启动时报固定密码还没设；8 位数字不算固定密码（weak）；设上、清掉各报一条；设备表是空的
    ASSERT_FALSE(fixed.isEmpty());
    EXPECT_EQ(fixed[0], "0:");
    host.send("password fixed 12345678");
    ASSERT_TRUE(waitUntil([&] { return fixed.size() == 2; })) << output.toStdString();
    EXPECT_EQ(fixed[1], "0:weak");
    host.send("password fixed Summer-2026");
    ASSERT_TRUE(waitUntil([&] { return fixed.size() == 3; })) << output.toStdString();
    EXPECT_EQ(fixed[2], "1:");
    EXPECT_TRUE(QFileInfo::exists(numberFile + ".pw"));
    host.send("password fixed-clear");
    ASSERT_TRUE(waitUntil([&] { return fixed.size() == 4; })) << output.toStdString();
    EXPECT_EQ(fixed[3], "0:");
    host.send("devices");
    ASSERT_TRUE(waitUntil([&] { return !listed.isEmpty(); })) << output.toStdString();
    EXPECT_EQ(listed, QStringList{"0"});
    // 界面信任设备时发的是 trust 设备号；不带的命令行、旧界面照旧能发。现在没有用密码连进来的会话：两种都答 none
    host.send("trust 00112233445566778899aabbccddeeff");
    ASSERT_TRUE(waitUntil([&] { return trusts.size() == 1; })) << output.toStdString();
    EXPECT_EQ(trusts[0], "0:none");
    host.send("trust");
    ASSERT_TRUE(waitUntil([&] { return trusts.size() == 2; })) << output.toStdString();
    EXPECT_EQ(trusts[1], "0:none");

    // 真客户端不带密码、也没被记住时按号码连：先问信令服务（不叫醒主机），在报任何阶段之前以退出码 3 结束。
    // 界面的试连照这几条走：对方收密码 30（去问密码），不在线 26、两种密码都没开 21（这两种不问）
    const auto trial = [&](const QString &target) {
        ashdesk::CoreProcess process;
        int code = -2;
        int phases = 0;
        QString error;
        QObject::connect(&process, &ashdesk::CoreProcess::finished, &process, [&](int exitCode) { code = exitCode; });
        QObject::connect(&process, &ashdesk::CoreProcess::message, &process, [&](const ashdesk::CoreMessage &message) {
            if (message.topic == "phase") {
                ++phases;
            }
            if (message.topic == "error") {
                error = message.value("error");
            }
        });
        EXPECT_TRUE(process.start(client, {"--connect", target + "@127.0.0.1:" + serverPort, "--ui-control", "--known-file",
                                           directory.filePath("known.dpapi"), "--device-file", directory.filePath("device.dpapi"),
                                           "--no-input", "--no-audio", "--clip", "off", "--files", "off"}));
        EXPECT_TRUE(waitUntil([&] { return code != -2; }));
        EXPECT_EQ(code, 3) << target.toStdString();
        EXPECT_EQ(phases, 0) << target.toStdString();
        return error;
    };
    const QString number = numbers.constLast();
    const QString nobody = number == "111111111" ? QStringLiteral("222222222") : QStringLiteral("111111111");
    constexpr qint64 kAnsweredMs = 1000;            // 信令服务答了就用不着等它重问（不答时要一秒多）
    QElapsedTimer timer;
    timer.start();
    EXPECT_EQ(trial(number), "30");
    std::cout << "[ perf ] client without a password learns that the host takes passwords in " << timer.elapsed() << " ms\n";
    EXPECT_LT(timer.elapsed(), kAnsweredMs);
    EXPECT_EQ(trial(nobody), "26");

    // 临时密码的两项设置：启动时报开着、用过就换；改一项报一条。关了以后主机报的密码是空的，开回来出一个新码
    ASSERT_FALSE(temps.isEmpty());
    EXPECT_EQ(temps[0], "1:0:");
    host.send("password temp keep");
    ASSERT_TRUE(waitUntil([&] { return temps.size() == 2; })) << output.toStdString();
    EXPECT_EQ(temps[1], "1:1:");
    host.send("password temp off");
    ASSERT_TRUE(waitUntil([&] { return temps.size() == 3 && passwords.constLast().isEmpty(); })) << output.toStdString();
    EXPECT_EQ(temps[2], "0:1:");
    // 临时密码关了、固定密码也没设：主机在信令服务上不再说收密码，试连报 21。主机改登记要一会儿，先报 30 的再试几次
    constexpr int kRegisterTries = 30;
    constexpr int kRegisterWaitMs = 100;
    QString closed = trial(number);
    for (int i = 0; i < kRegisterTries && closed == "30"; ++i) {
        QTest::qWait(kRegisterWaitMs);
        closed = trial(number);
    }
    EXPECT_EQ(closed, "21");
    host.send("password temp on");
    ASSERT_TRUE(waitUntil([&] { return temps.size() == 4 && !passwords.constLast().isEmpty(); })) << output.toStdString();
    EXPECT_EQ(temps[3], "1:1:");
    EXPECT_TRUE(QRegularExpression("^\\d{8}$").match(passwords.constLast()).hasMatch());
    host.send("password temp rotate");
    ASSERT_TRUE(waitUntil([&] { return temps.size() == 5; })) << output.toStdString();
    EXPECT_EQ(temps[4], "1:0:");
    EXPECT_TRUE(QFileInfo::exists(numberFile + ".pwopt"));
    // 明文的固定密码不进主机的输出
    EXPECT_FALSE(output.contains("Summer-2026")) << output.toStdString();
    EXPECT_FALSE(output.contains("@ui "));
    host.stop();
    ASSERT_TRUE(waitUntil([&] { return !host.busy(); })) << output.toStdString();
    rendezvous.kill();
    rendezvous.waitForFinished();
}

// 借鉴设计 A2：真的服务（前台调试）拉起真的主机。界面要按服务管道的 DACL（只给 FILE_GENERIC_READ | FILE_WRITE_DATA）打开，
// QLocalServer 建的管道权限是全的，测不出这一点
TEST(DesktopCore, WindowAttachesToTheRealServiceThroughItsRestrictedPipe) {
    const QString cores = QCoreApplication::applicationDirPath();
    const QString service = QDir(cores).filePath("adrd_svc.exe");
    const QString host = QDir(cores).filePath("adrd_host.exe");
    if (!QFileInfo::exists(service) || !QFileInfo::exists(host)) GTEST_SKIP() << "Standalone UI build has no Windows service";
    const QTemporaryDir directory;
    QUdpSocket reservation;
    ASSERT_TRUE(reservation.bind(QHostAddress::LocalHost, 0));
    const QString port = QString::number(reservation.localPort());
    reservation.close();
    const QString name = QStringLiteral("AshDeskSvcGtest%1").arg(QCoreApplication::applicationPid());
    const QString hostArguments = QStringLiteral("--bind 127.0.0.1 --port %1 --test-pattern 160x96 --tp-cpu --encoder ae264 --keep-desktop "
                                                 "--cleartype --audio off --clip off --files off --takeover-confirm never "
                                                 "--device-store %2 --audit-log %3")
                                      .arg(port, QDir::toNativeSeparators(directory.filePath("devices.dpapi")),
                                           QDir::toNativeSeparators(directory.filePath("audit.log")));
    QProcess supervisor;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("ProgramData", QDir::toNativeSeparators(directory.path()));   // 服务日志写到这里，不进本机的 ProgramData
    supervisor.setProcessEnvironment(environment);
    supervisor.setProcessChannelMode(QProcess::MergedChannels);
    supervisor.start(service, {"run", "--console", "--name", name, "--host-exe", QDir::toNativeSeparators(host), "--host-args", hostArguments});
    ASSERT_TRUE(supervisor.waitForStarted());

    QSettings settings(directory.filePath("desktop.ini"), QSettings::IniFormat);
    settings.setValue("serviceName", name);
    ashdesk::MainWindow window(settings, cores, directory.filePath("data"));
    window.show();
    auto *core = control<ashdesk::CoreProcess>(window, "hostCore");
    auto *hostStatus = control<QLabel>(window, "hostStatus");
    const bool listening = waitUntil([&] { return core->attached() && hostStatus->text() == QStringLiteral("等待连接"); });
    if (auto *quit = window.findChild<QAction *>("trayQuit")) {
        quit->trigger();
    } else {
        window.close();
    }
    supervisor.kill();
    supervisor.waitForFinished();
    EXPECT_TRUE(listening) << hostStatus->text().toStdString() << "\n" << supervisor.readAll().toStdString();
}

TEST(DesktopCore, NetworkDiagnosticsShowEveryItemAndTheVerdictOfTheRealHost) {
    const QString cores = QCoreApplication::applicationDirPath();
    if (!QFileInfo::exists(QDir(cores).filePath("adrd_host.exe"))) GTEST_SKIP() << "Standalone UI build has no Windows host";
    QTemporaryDir directory;
    QSettings settings(directory.filePath("desktop.ini"), QSettings::IniFormat);
    // 号码连接开着时没填信令服务器会去测内置的那一台：测试不往公网上发包，先关掉
    settings.setValue("host/number", false);
    ashdesk::MainWindow window(settings, cores, directory.filePath("data"));
    window.show();
    auto *start = control<QPushButton>(window, "startNetdiag");
    ASSERT_TRUE(start->isEnabled());
    start->click();
    ASSERT_TRUE(waitUntil([&] { return start->isEnabled(); }));
    // 没填信令服务器：核心每一项都明说没测，结论是判断不了
    for (const char *item : {"diag-nat", "diag-udp", "diag-tcp", "diag-relay_rtt", "diag-portmap", "diag-firewall"}) {
        const QString value = control<QLabel>(window, item)->text();
        EXPECT_TRUE(value.startsWith(QStringLiteral("✓ ")) || value.startsWith(QStringLiteral("✗ "))) << item << " " << value.toStdString();
    }
    const QString verdict = control<QLabel>(window, "netdiagVerdict")->text();
    EXPECT_FALSE(verdict.isEmpty());
    EXPECT_NE(verdict, QStringLiteral("诊断没有完成"));
    window.close();
}

TEST(DesktopCore, HostPerformsNormalCleanupForSplitStopAndClosedParentPipe) {
    const QString program = QDir(QCoreApplication::applicationDirPath()).filePath("adrd_host.exe");
    if (!QFileInfo::exists(program)) GTEST_SKIP() << "Standalone UI build has no Windows host";
    QTemporaryDir directory;
    for (const bool closeOnly : {false, true}) {
        QUdpSocket reservation;
        ASSERT_TRUE(reservation.bind(QHostAddress::LocalHost, 0));
        const QString port = QString::number(reservation.localPort());
        reservation.close();
        QProcess host;
        QString output;
        QStringDecoder decoder(QStringDecoder::Utf8);
        host.setProcessChannelMode(QProcess::MergedChannels);
        host.setWorkingDirectory(QFileInfo(program).absolutePath());
        QObject::connect(&host, &QProcess::readyReadStandardOutput, &host, [&] {
            output += decoder(host.readAllStandardOutput());
        });
        host.start(program, {"--ui-control", "--bind", "127.0.0.1", "--port", port,
                            "--test-pattern", "160x96", "--tp-cpu", "--encoder", "ae264",
                            "--keep-desktop", "--cleartype", "--audio", "off", "--clip", "off",
                            "--files", "off", "--takeover-confirm", "never",
                            "--device-store", directory.filePath("devices.dpapi"),
                            "--audit-log", directory.filePath("audit.log")});
        ASSERT_TRUE(waitUntil([&] { return output.contains(QStringLiteral("等待客户端")); })) << output.toStdString();
        if (!closeOnly) {
            host.write("st");
            host.waitForBytesWritten(1000);
            QTest::qWait(150);
            EXPECT_EQ(host.state(), QProcess::Running);
            host.write("op\n");
        }
        host.closeWriteChannel();
        ASSERT_TRUE(waitUntil([&] { return host.state() == QProcess::NotRunning; })) << output.toStdString();
        EXPECT_EQ(host.exitStatus(), QProcess::NormalExit);
        EXPECT_EQ(host.exitCode(), 0) << output.toStdString();
        EXPECT_TRUE(output.contains(QStringLiteral("主机退出"))) << output.toStdString();
    }
}
#endif

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--process-fixture") == 0) {
        std::puts("ready");
        std::fflush(stdout);
        char command[16]{};
        if (!std::fgets(command, sizeof command, stdin) || std::strcmp(command, "stop\n") != 0) return 2;
        std::puts("clean exit");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--process-failure") == 0) {
        std::fputs("fixture failed\n", stderr);
        return 23;
    }
    ::testing::InitGoogleTest(&argc, argv);
    if (::testing::GTEST_FLAG(list_tests)) return RUN_ALL_TESTS();
    // 开发机上装着真的系统服务 AshDeskSvc：不改名的话，测试里的窗口一转事件循环就接到它的界面管道上，
    // 之后点的吊销、信任、设固定密码都会发给真的主机。设置里自己写了 serviceName 的用例（扮服务的那两个）不受影响
    qputenv("ASHDESK_SERVICE_NAME", "AshDeskSvcUiTests");
    QApplication application(argc, argv);
    QApplication::setStyle("Fusion");
    return RUN_ALL_TESTS();
}
