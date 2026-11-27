#include "ashdesk_version.h"
#include "main_window.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName("AshDesk");
    QCoreApplication::setApplicationName("AshDesk");
    QCoreApplication::setApplicationVersion(QStringLiteral(ASHDESK_VERSION_STRING));
    QApplication::setStyle("Fusion");
    QIcon applicationIcon;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 128, 256})
        applicationIcon.addFile(QStringLiteral(":/ashdesk/app/ashdesk-%1.png").arg(size), QSize(size, size));
    QApplication::setWindowIcon(applicationIcon);
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(directory);
    const QString file = QDir(directory).filePath("desktop.ini");
    // 先按系统语言装翻译，配置读不出来时的提示也能翻译；坏配置会恢复成“跟随系统”，读出来之后再按设置换
    ashdesk::useLanguage(0);
    const QString notice = ashdesk::prepareSettings(file);
    QSettings settings(file, QSettings::IniFormat);
    if (const int language = ashdesk::DesktopOptions::load(settings).language) ashdesk::useLanguage(language);
    ashdesk::MainWindow window(settings);
    window.show();
    window.goOnline();
    // 窗口先画出来再问
    QTimer::singleShot(0, &window, [&window] { window.offerService(); });
    // 放在上线之后：主机启动成功会清掉状态栏，配置被恢复过的提示不能跟着没了
    if (!notice.isEmpty()) window.message(notice);
    return application.exec();
}
