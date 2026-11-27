#include "diagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace ashdesk {
namespace {

// dump 是二进制，原样带上；文本日志只取末尾，再脱敏
QByteArray readEntry(const QFileInfo &info) {
    QFile file(info.filePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    if (info.suffix() == QLatin1String("dmp")) return file.readAll();
    if (file.size() > kDiagnosticLogTail) {
        file.seek(file.size() - kDiagnosticLogTail);
        file.readLine(); // 从下一整行开始，不切开 UTF-8 字符
    }
    return redactSecrets(QString::fromUtf8(file.readAll())).toUtf8();
}

bool writeEntry(const QDir &stage, const QString &name, const QByteArray &data) {
    QFile file(stage.filePath(name));
    return stage.mkpath(QFileInfo(name).path()) && file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// candidates 只按字形挑出可能是地址的片段（时间、MAC 这类也会被挑中），真是地址的才换成占位
QString redactAddresses(const QString &text, const QRegularExpression &candidates) {
    QString result;
    qsizetype copied = 0;
    for (auto match = candidates.globalMatch(text); match.hasNext();) {
        const QRegularExpressionMatch candidate = match.next();
        const QHostAddress address(candidate.captured());
        if (address.isNull() || address == QHostAddress(QHostAddress::AnyIPv4) || address == QHostAddress(QHostAddress::AnyIPv6))
            continue;
        // QHostAddress::isPrivateUse 要 Qt 6.6，这里按 RFC 1918 的三个网段自己判；IPv6 的私网是 ULA 和旧的 site-local
        bool mapsToIpv4 = false;
        const quint32 ipv4 = address.toIPv4Address(&mapsToIpv4);
        const bool privateUse = mapsToIpv4 ? (ipv4 >> 24) == 10 || (ipv4 >> 20) == 0xAC1 || (ipv4 >> 16) == 0xC0A8
                                           : address.isUniqueLocalUnicast() || address.isSiteLocal();
        const QLatin1StringView family(address.protocol() == QAbstractSocket::IPv4Protocol ? "ipv4" : "ipv6");
        const QLatin1StringView scope(address.isLoopback()    ? "loopback"
                                      : address.isLinkLocal() ? "link-local"
                                      : privateUse            ? "private"
                                                              : "public");
        result += QStringView(text).mid(copied, candidate.capturedStart() - copied);
        result += QStringLiteral("<%1-%2>").arg(family, scope);
        copied = candidate.capturedEnd();
    }
    result += QStringView(text).mid(copied);
    return result;
}

} // namespace

QString redactSecrets(QString text) {
    static const QRegularExpression option(QStringLiteral(R"((--(?:psk|pair)\s+)(?!-)\S+)"));
    static const QRegularExpression code(QStringLiteral(R"((配对码[：:]\s*)\d{3}\s*\d{3})"));
    static const QRegularExpression hex(QStringLiteral("[0-9A-Fa-f]{32,}"));
    // IPv6 末尾可以嵌一段 IPv4（::ffff:192.0.2.1），所以先换 IPv6
    static const QRegularExpression ipv6(QStringLiteral(
        R"((?<![0-9A-Za-z:.])(?:[0-9A-Fa-f]{0,4}:){2,7}(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{0,4})(?![0-9A-Za-z:.]))"));
    static const QRegularExpression ipv4(QStringLiteral(R"((?<![0-9A-Za-z.])\d{1,3}(?:\.\d{1,3}){3}(?![0-9A-Za-z]|\.\d))"));
    const QString hidden = QStringLiteral("<已隐藏>");
    text.replace(option, QStringLiteral("\\1") + hidden);
    text.replace(code, QStringLiteral("\\1") + hidden);
    text.replace(hex, hidden);
    return redactAddresses(redactAddresses(text, ipv6), ipv4);
}

QString coreLogFile(const QString &directory, const QString &name) {
    const QDir folder(directory);
    folder.mkpath(QStringLiteral("."));
    const QString current = folder.filePath(name + QStringLiteral(".log"));
    if (QFileInfo(current).size() <= kCoreLogLimit) return current;
    const auto archived = [&](int index) { return folder.filePath(QStringLiteral("%1.%2.log").arg(name).arg(index)); };
    QFile::remove(archived(kCoreLogKeep));
    for (int index = kCoreLogKeep; index > 1; --index) QFile::rename(archived(index - 1), archived(index));
    QFile::rename(current, archived(1));
    return current;
}

QString zipTool() {
#ifdef Q_OS_WIN
    return QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/tar.exe"));
#elif defined(Q_OS_MACOS)
    return QStringLiteral("/usr/bin/tar");
#else
    return QStringLiteral("bsdtar");   // Linux 发行版的 tar 多是 GNU tar；bsdtar 在 libarchive-tools 里
#endif
}

QString packDiagnostics(const QString &target, const DiagnosticEntries &folders, const DiagnosticEntries &texts) {
    const QTemporaryDir stage;
    if (!stage.isValid())
        return QCoreApplication::translate("ashdesk", "无法创建临时目录：%1").arg(stage.errorString());
    const QDir root(stage.path());
    QStringList entries;
    bool written = true;
    for (const auto &[name, text] : texts) {
        written = written && writeEntry(root, name, redactSecrets(text).toUtf8());
        entries << name;
    }
    const QStringList patterns{QStringLiteral("*-crash-*.dmp"), QStringLiteral("*.log"), QStringLiteral("caps.json")};
    for (const auto &[prefix, path] : folders) {
        const QDir folder(path);
        for (const QString &pattern : patterns) {
            const QFileInfoList newest = folder.entryInfoList({pattern}, QDir::Files, QDir::Time);
            for (const QFileInfo &file : newest.mid(0, kDiagnosticFiles)) {
                const QString name = prefix + QLatin1Char('/') + file.fileName();
                written = written && writeEntry(root, name, readEntry(file));
                entries << name;
            }
        }
    }
    if (!written)
        return QCoreApplication::translate("ashdesk", "无法写入临时目录 %1").arg(QDir::toNativeSeparators(root.path()));
    // 不指定 hdrcharset 时中文文件名会被转成 CP437 问号
    QProcess tar;
    tar.setProcessChannelMode(QProcess::MergedChannels);
    tar.setWorkingDirectory(root.path());
    tar.start(zipTool(),
              QStringList{QStringLiteral("--options"), QStringLiteral("hdrcharset=UTF-8"), QStringLiteral("-a"),
                          QStringLiteral("-cf"), QStringLiteral("pack.zip")} +
                  entries);
    if (!tar.waitForFinished(60000) || tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0) {
        const QString reason = tar.error() == QProcess::UnknownError ? QString::fromLocal8Bit(tar.readAll()).trimmed()
                                                                      : tar.errorString();
        return QCoreApplication::translate("ashdesk", "打包失败：%1").arg(reason);
    }
    QFile::remove(target);
    if (!QFile::copy(root.filePath(QStringLiteral("pack.zip")), target))
        return QCoreApplication::translate("ashdesk", "无法写入 %1").arg(QDir::toNativeSeparators(target));
    return {};
}

} // namespace ashdesk
