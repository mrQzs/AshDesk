#pragma once

#include <QList>
#include <QPair>
#include <QString>

namespace ashdesk {

// 每个目录每类只收最新的几个；文本日志只留末尾，够看最近几次会话
constexpr qsizetype kDiagnosticFiles = 5;
constexpr qint64 kDiagnosticLogTail = qint64{2} * 1024 * 1024;

// 界面让核心用 --log 落盘的日志：核心是追加写，超过这个大小就在下次拉起核心之前轮换，旧文件最多留 kCoreLogKeep 个
constexpr qint64 kCoreLogLimit = qint64{4} * 1024 * 1024;
constexpr int kCoreLogKeep = 3;

using DiagnosticEntries = QList<QPair<QString, QString>>;

// 隐藏 --psk / --pair 的值、配对码和 32 位以上的十六进制串（密钥）；IP 地址换成只留类别的占位，
// 如 <ipv4-private>、<ipv6-loopback>（类别：loopback、private、link-local、public；0.0.0.0 和 :: 原样保留）
QString redactSecrets(QString text);
// 返回 directory/<name>.log 给核心的 --log 用：目录不存在就建；文件超过 kCoreLogLimit 时先轮换成
// <name>.1.log（原来的 .1 变 .2，依此类推，超出 kCoreLogKeep 的删掉）
QString coreLogFile(const QString &directory, const QString &name);
// 写 zip 用的 bsdtar：Windows 10 1803 起自带的那个（PATH 里 Git 带的 GNU tar 写不了 zip，所以写全路径），macOS 自带的 tar 本身就是 bsdtar
QString zipTool();
// folders 是“条目前缀 → 目录”，从中收最新的崩溃 dump、*.log 和 caps.json；texts 是“条目名 → 文本”。
// 设备表、密钥等 *.dpapi 一律不收。成功返回空串，否则返回给用户看的原因。
QString packDiagnostics(const QString &target, const DiagnosticEntries &folders, const DiagnosticEntries &texts);

} // namespace ashdesk
