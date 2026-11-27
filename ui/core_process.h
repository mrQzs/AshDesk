#pragma once

#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QStringDecoder>

namespace ashdesk {

// 核心经控制管道报来的一行状态（格式见 docs/核心接口.md）。
// topic 是 "@ui " 后的第一项："@ui stats fps=60" 是 stats，"@ui phase=stream" 这种第一项就是键值的取它的键；
// action 是后面不带等号的词："@ui pair done name=…" 的 done
struct CoreMessage {
    QString topic;
    QString action;
    QHash<QString, QString> values;
    QString value(const QString &key) const { return values.value(key); }
};

// 不是 "@ui " 开头的行返回 false；值里的 \s、\n、\\ 还原成空格、换行、反斜杠
bool parseCoreMessage(QStringView line, CoreMessage &message);

#ifdef Q_OS_WIN
// 服务管道的属主（SID）信不信得过：LocalSystem / Administrators（真服务）或者当前用户（前台调试的服务、测试）。
// 服务没在跑时本机别的账户能抢先建同名管道，给界面看假状态、收界面的命令；那种属主是别的账户。attach 用，单独拿出来给测试
bool trustedServiceOwner(void *ownerSid);
#endif

class CoreProcess final : public QObject {
    Q_OBJECT
public:
    explicit CoreProcess(QObject *parent = nullptr);
    bool start(const QString &program, const QStringList &arguments);
    // 接到系统服务的界面管道（\\.\pipe\<服务名>.ui，格式见 docs/核心接口.md），代替自己起核心：
    // 服务拉起的主机报的状态照常发 message，send 的命令由服务转给它；stop 只是断开，停不了服务的主机。
    // 服务没装、没在跑、已经有别的界面连着，返回 false
    bool attach(const QString &serviceName);
    bool attached() const;
    void stop();
    // 往核心的 stdin（接着服务时是服务的管道）写一条命令（不带换行）；核心没在运行或正在停止时什么都不做
    void send(const QString &command);
    // 处理读到的一段核心输出：成行的 "@ui " 状态发 message，其余发 output；不成行的尾巴留到下一段
    void consume(const QByteArray &bytes);
    bool busy() const;
    QString status() const;

signals:
    void changed();
    void output(const QString &text);
    void message(const ashdesk::CoreMessage &message);
    void launched();
    // code 是正常退出时的退出码；崩溃或没能启动时为 -1
    void finished(int code);

private:
    void setStatus(const QString &status);
    void detach(const QString &status, int code);
    QProcess process_;
    QLocalSocket service_;
    bool attached_ = false;
    QStringDecoder decoder_{QStringDecoder::Utf8};
    QString pending_;
    QString status_;
    bool stopping_ = false;
};

} // namespace ashdesk
