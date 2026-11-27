#pragma once

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <functional>

namespace ashdesk {

// 安装后的布局是 <安装目录>\bin\ashdesk.exe 和 <安装目录>\install.ps1：从界面程序所在的目录找安装目录。
// 上一级没有 install.ps1（从构建目录直接跑的界面）时返回空串
QString installDirectory(const QString &applicationDirectory = QCoreApplication::applicationDirPath());

// 一条要提权运行的命令，分成 ShellExecuteEx 要的两段；parameters 里的路径已经加了引号
struct ElevatedCommand {
    QString program;
    QString parameters;
};

// 核心报 "@ui need driver=X" 后补装驱动 X（vdisplay / vaudio）要跑的命令：安装目录里的 install.ps1
// （packaging/windows/install.ps1）。-InPlace 不能省：脚本默认装到 %ProgramFiles%\AshDesk，装在别处的不带它
// 会被整个再拷一份过去；绿色包也不该因此多出开始菜单和卸载登记
// 包里有这个驱动的签名证书（driverCertificate）时一并带上 -TestCertificate：开发期的驱动是自签的，装之前要把这张证书
// 加进本机的“受信任的根”和“受信任的发布者”（用户 11-26 同意了这个取舍）
ElevatedCommand driverInstallCommand(const QString &directory, const QString &driver);
// 包里驱动 driver 的签名证书（drivers\<driver>\ 下唯一的 .cer，驱动的构建脚本只在自签时放进去）；没有返回空串
QString driverCertificate(const QString &directory, const QString &driver);

// 把界面所在的这份装成系统服务：同一个 install.ps1 原地装（-InPlace），服务里的主机带 hostArguments（一项一个参数）
ElevatedCommand serviceInstallCommand(const QString &directory, const QStringList &hostArguments);
// 只卸服务（bin\adrd_svc.exe uninstall），程序、驱动、防火墙规则都留着：整套不要了跑安装目录里的 uninstall.ps1
ElevatedCommand serviceUninstallCommand(const QString &directory);
bool serviceInstalled(const QString &name);
// 服务登记的命令行（服务管理器里的 binPath）；没装或查不到返回空串
QString serviceCommandLine(const QString &name);
// binPath 里的程序路径：adrd_svc install 写的是 "<目录>\adrd_svc.exe" service；没加引号的就到第一个空格为止
QString serviceProgram(const QString &commandLine);
// 服务是不是从界面所在的这一份跑的：比程序所在的目录和界面程序的目录（不分大小写、不管斜杠方向）
bool serviceRunsHere(const QString &commandLine);

// 按 Windows 拆命令行的规则（CommandLineToArgvW、MSVC 运行库）把一个参数写成命令行里的一段：有空格、引号或为空才加引号
QString quotedArgument(const QString &argument);

// 经 UAC 提权启动 command，不等它结束；它退出时在 context 的线程上调 done（退出码）。
// 没启动起来（用户在 UAC 里点了“否”，或者不是 Windows）返回 false，done 不会被调
bool runElevated(const ElevatedCommand &command, QObject *context, const std::function<void(int)> &done);

} // namespace ashdesk
