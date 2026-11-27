#include "core_process.h"

#include <QFileInfo>
#include <array>
#include <cstring>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#endif

namespace ashdesk {

static QString unescape(QStringView value);

bool parseCoreMessage(QStringView line, CoreMessage &message) {
    const QLatin1StringView prefix("@ui ");
    if (!line.startsWith(prefix)) return false;
    message = {};
    const QList<QStringView> tokens = line.mid(prefix.size()).trimmed().split(u' ', Qt::SkipEmptyParts);
    for (const QStringView token : tokens) {
        const qsizetype equals = token.indexOf(u'=');
        const QString word = (equals < 0 ? token : token.left(equals)).toString();
        if (message.topic.isEmpty()) message.topic = word;
        else if (equals < 0) message.action = word;
        if (equals >= 0) message.values.insert(word, unescape(token.mid(equals + 1)));
    }
    return true;
}

static QString unescape(QStringView value) {
    QString text;
    text.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        if (value[i] != u'\\' || i + 1 == value.size()) {
            text += value[i];
            continue;
        }
        const QChar escaped = value[++i];
        text += escaped == u's' ? QChar(u' ') : escaped == u'n' ? QChar(u'\n') : escaped;
    }
    return text;
}

CoreProcess::CoreProcess(QObject *parent) : QObject(parent), status_(tr("未启动")) {
    process_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process_, &QProcess::started, this, [this] {
        if (stopping_) stop();
        else {
            setStatus(tr("核心进程运行中"));
            emit launched();
        }
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { consume(process_.readAllStandardOutput()); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        setStatus(tr("无法启动：") + process_.errorString());
        emit finished(-1);
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exitStatus) {
        consume(process_.readAllStandardOutput());
        if (!pending_.isEmpty()) emit output(std::exchange(pending_, {}));
        const bool normal = exitStatus == QProcess::NormalExit;
        setStatus(normal && code == 0 ? tr("已停止") : tr("核心异常退出（%1），请查看诊断").arg(code));
        stopping_ = false;
        emit finished(normal ? code : -1);
    });
    connect(&service_, &QLocalSocket::readyRead, this, [this] { consume(service_.readAll()); });
    connect(&service_, &QLocalSocket::disconnected, this, [this] { detach(tr("系统服务断开了"), -1); });
}

bool CoreProcess::attach(const QString &serviceName) {
#ifdef Q_OS_WIN
    if (busy()) {
        return false;
    }
    const std::wstring path = (QStringLiteral("\\\\.\\pipe\\") + serviceName + QStringLiteral(".ui")).toStdWString();
    // 服务的管道只给 FILE_GENERIC_READ | FILE_WRITE_DATA：QLocalSocket::connectToServer 要的 GENERIC_WRITE 多了
    // FILE_APPEND_DATA，会被拒。按这个权限自己打开（Qt 的管道读写要重叠 I/O），再交给 QLocalSocket
    HANDLE pipe = CreateFileW(path.c_str(), GENERIC_READ | FILE_WRITE_DATA, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const bool trusted = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) == ERROR_SUCCESS &&
                         trustedServiceOwner(owner);
    LocalFree(sd);
    if (!trusted) {
        CloseHandle(pipe);
        setStatus(tr("有别的账户冒用系统服务的管道，没有接上"));
        return false;
    }
    decoder_.resetState();
    pending_.clear();
    // HANDLE 是指针、Qt 要整数描述符：按字节搬过去，不用 reinterpret_cast（AUTOSAR A5-2-4）
    qintptr descriptor = 0;
    static_assert(sizeof descriptor == sizeof pipe);
    std::memcpy(&descriptor, static_cast<const void *>(&pipe), sizeof descriptor);
    if (!service_.setSocketDescriptor(descriptor)) {
        CloseHandle(pipe);
        return false;
    }
    attached_ = true;
    setStatus(tr("由系统服务运行"));
    return true;
#else
    Q_UNUSED(serviceName);
    return false;
#endif
}

#ifdef Q_OS_WIN
// LocalSystem 建的对象，属主是 SYSTEM 或 Administrators（看令牌的默认属主），两个都认
bool trustedServiceOwner(void *ownerSid) {
    if (ownerSid == nullptr || IsValidSid(ownerSid) == FALSE) {
        return false;
    }
    if (IsWellKnownSid(ownerSid, WinLocalSystemSid) != FALSE || IsWellKnownSid(ownerSid, WinBuiltinAdministratorsSid) != FALSE) {
        return true;
    }
    HANDLE token = nullptr;
    union {
        TOKEN_USER user;
        std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> raw;
    } info{};
    DWORD size = 0;
    const bool mine = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE &&
                      GetTokenInformation(token, TokenUser, &info, sizeof info, &size) != FALSE &&
                      EqualSid(ownerSid, info.user.User.Sid) != FALSE;
    if (token != nullptr) {
        CloseHandle(token);
    }
    return mine;
}
#endif

bool CoreProcess::attached() const { return attached_; }

// 不接着服务了：界面自己断开（stop），或者服务那头断了（服务停了、换了控制台会话）
void CoreProcess::detach(const QString &status, int code) {
    if (!attached_) {
        return;
    }
    attached_ = false;
    service_.abort();
    setStatus(status);
    emit finished(code);
}

bool CoreProcess::start(const QString &program, const QStringList &arguments) {
    if (busy()) return false;
    if (!QFileInfo(program).isExecutable()) {
        setStatus(tr("未找到可运行的核心：%1").arg(program));
        return false;
    }
    decoder_.resetState();
    pending_.clear();
    stopping_ = false;
    process_.setProgram(program);
    process_.setArguments(arguments);
    process_.setWorkingDirectory(QFileInfo(program).absolutePath());
    process_.start();
    setStatus(tr("正在启动"));
    return true;
}

void CoreProcess::stop() {
    if (attached_) {
        detach(tr("未启动"), 0);
        return;
    }
    if (!busy()) return;
    stopping_ = true;
    setStatus(tr("正在停止"));
    if (process_.state() == QProcess::Running) {
        process_.write("stop\n");
        process_.closeWriteChannel();
    }
}

void CoreProcess::send(const QString &command) {
    if (attached_) {
        service_.write(command.toUtf8() + '\n');
    } else if (process_.state() == QProcess::Running && !stopping_) process_.write(command.toUtf8() + '\n');
}

void CoreProcess::consume(const QByteArray &bytes) {
    pending_ += decoder_(bytes);
    // 先把成行的部分取走再发信号：槽里可能重新启动核心，那会清掉 pending_
    const qsizetype complete = pending_.lastIndexOf(u'\n') + 1;
    const QString lines = pending_.left(complete);
    pending_.remove(0, complete);
    QString log;
    for (qsizetype start = 0; start < lines.size();) {
        const qsizetype end = lines.indexOf(u'\n', start);
        const QStringView line = QStringView(lines).mid(start, end - start);
        start = end + 1;
        CoreMessage parsed;
        if (!parseCoreMessage(line, parsed)) {
            log += line;
            log += u'\n';
            continue;
        }
        // 状态前面的日志先发出去，两者保持核心输出的先后顺序
        if (!log.isEmpty()) emit output(std::exchange(log, {}));
        emit message(parsed);
    }
    if (!log.isEmpty()) emit output(log);
}

bool CoreProcess::busy() const { return attached_ || process_.state() != QProcess::NotRunning; }
QString CoreProcess::status() const { return status_; }
void CoreProcess::setStatus(const QString &status) {
    status_ = status;
    emit changed();
}

} // namespace ashdesk
