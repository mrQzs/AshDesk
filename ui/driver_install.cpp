#include "driver_install.h"

#include <QDir>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#include <string>
#include <vector>
#include <windows.h>

#include <shellapi.h>
#include <winsvc.h>
#endif

namespace ashdesk {

QString installDirectory(const QString &applicationDirectory) {
    QDir directory(applicationDirectory);
    return directory.cdUp() && directory.exists(QStringLiteral("install.ps1")) ? directory.path() : QString();
}

namespace {

// 这几条命令只在 Windows 上跑：路径一律写成反斜杠，和构建界面的平台无关
QString windowsPath(QString path) {
    path.replace(u'/', u'\\');
    return path;
}

} // namespace

ElevatedCommand driverInstallCommand(const QString &directory, const QString &driver) {
    QString parameters = QStringLiteral("-NoProfile -ExecutionPolicy Bypass -File \"%1\\install.ps1\" -InPlace -Drivers %2 -SkipOpenH264")
                             .arg(windowsPath(directory), driver);
    const QString certificate = driverCertificate(directory, driver);
    if (!certificate.isEmpty()) {
        parameters += QStringLiteral(" -TestCertificate ") + quotedArgument(windowsPath(certificate));
    }
    return {QStringLiteral("powershell.exe"), parameters};
}

QString driverCertificate(const QString &directory, const QString &driver) {
    const QDir folder(QDir(directory).filePath(QStringLiteral("drivers/") + driver));
    const QStringList found = folder.entryList({QStringLiteral("*.cer")}, QDir::Files);
    return found.size() == 1 ? folder.filePath(found.front()) : QString();
}

ElevatedCommand serviceInstallCommand(const QString &directory, const QStringList &hostArguments) {
    QStringList line;
    for (const QString &argument : hostArguments) {
        line << quotedArgument(argument);
    }
    // 主机参数拼成一行，整行再作为 -ServiceHostArgs 的一个值：引号要套两层
    return {QStringLiteral("powershell.exe"),
            QStringLiteral("-NoProfile -ExecutionPolicy Bypass -File \"%1\\install.ps1\" -InPlace -Service -AllowSas -SkipOpenH264 "
                           "-ServiceHostArgs %2")
                .arg(windowsPath(directory), quotedArgument(line.join(u' ')))};
}

ElevatedCommand serviceUninstallCommand(const QString &directory) {
    return {windowsPath(directory) + QStringLiteral("\\bin\\adrd_svc.exe"), QStringLiteral("uninstall")};
}

bool serviceInstalled(const QString &name) {
#ifdef Q_OS_WIN
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, name.toStdWString().c_str(), SERVICE_QUERY_STATUS);
    if (service != nullptr) {
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return service != nullptr;
#else
    Q_UNUSED(name);
    return false;
#endif
}

QString serviceCommandLine(const QString &name) {
#ifdef Q_OS_WIN
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager == nullptr) {
        return {};
    }
    SC_HANDLE service = OpenServiceW(manager, name.toStdWString().c_str(), SERVICE_QUERY_CONFIG);
    QString line;
    DWORD need = 0;
    if (service != nullptr && QueryServiceConfigW(service, nullptr, 0, &need) == FALSE && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        // 按结构体的个数分配，对齐由 vector 保证；字符串紧跟在结构体后面
        std::vector<QUERY_SERVICE_CONFIGW> buffer(need / sizeof(QUERY_SERVICE_CONFIGW) + 1);
        QUERY_SERVICE_CONFIGW *const config = buffer.data();
        const auto size = static_cast<DWORD>(buffer.size() * sizeof(QUERY_SERVICE_CONFIGW));
        if (QueryServiceConfigW(service, config, size, &need) != FALSE && config->lpBinaryPathName != nullptr) {
            line = QString::fromWCharArray(config->lpBinaryPathName);
        }
    }
    if (service != nullptr) {
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return line;
#else
    Q_UNUSED(name);
    return {};
#endif
}

QString serviceProgram(const QString &commandLine) {
    const QString line = commandLine.trimmed();
    return line.startsWith(u'"') ? line.mid(1).section(u'"', 0, 0) : line.section(u' ', 0, 0);
}

bool serviceRunsHere(const QString &commandLine) {
    const QString program = serviceProgram(commandLine);
    if (program.isEmpty()) {
        return false;
    }
    const QString here = QDir::cleanPath(QCoreApplication::applicationDirPath());
    const QString there = QFileInfo(QDir::fromNativeSeparators(program)).path();
    return QDir::cleanPath(there).compare(here, Qt::CaseInsensitive) == 0;
}

QString quotedArgument(const QString &argument) {
    if (!argument.isEmpty() && !argument.contains(u' ') && !argument.contains(u'\t') && !argument.contains(u'"')) {
        return argument;
    }
    // 反斜杠只在引号前（含收尾那个）才是转义：那里成对写，后面跟着的引号再多一个
    QString quoted(u'"');
    qsizetype slashes = 0;
    for (const QChar c : argument) {
        if (c == u'\\') {
            ++slashes;
            continue;
        }
        quoted += QString(c == u'"' ? slashes * 2 + 1 : slashes, u'\\');
        quoted += c;
        slashes = 0;
    }
    return quoted + QString(slashes * 2, u'\\') + u'"';
}

bool runElevated(const ElevatedCommand &command, QObject *context, const std::function<void(int)> &done) {
#ifdef Q_OS_WIN
    // 提权去跑的程序不按搜索顺序找：当前目录或 PATH 里放一个同名程序就成了提权的口子。不带路径的名字一律指系统自带的 Windows PowerShell
    std::wstring program = command.program.toStdWString();
    wchar_t system[MAX_PATH];
    if (program.find(L'\\') == std::wstring::npos && GetSystemDirectoryW(system, MAX_PATH) != 0) {
        program = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\" + program;
    }
    const std::wstring parameters = command.parameters.toStdWString();
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof launch;
    launch.fMask = SEE_MASK_NOCLOSEPROCESS;
    launch.lpVerb = L"runas";
    launch.lpFile = program.c_str();
    launch.lpParameters = parameters.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch) || !launch.hProcess) return false;
    auto *exited = new QWinEventNotifier(launch.hProcess, context);
    QObject::connect(exited, &QWinEventNotifier::activated, context, [exited, done](Qt::HANDLE process) {
        // 进程句柄在进程退出后一直有信号：不先停掉通知器，它发完信号会对着已经关掉的句柄再等一次
        exited->setEnabled(false);
        exited->deleteLater();
        // 查不到退出码时按失败算
        DWORD code = DWORD(-1);
        GetExitCodeProcess(process, &code);
        CloseHandle(process);
        done(int(code));
    });
    return true;
#else
    Q_UNUSED(command);
    Q_UNUSED(context);
    Q_UNUSED(done);
    return false;
#endif
}

} // namespace ashdesk
