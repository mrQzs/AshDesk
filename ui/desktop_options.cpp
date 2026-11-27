#include "desktop_options.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QTranslator>
#include <QUrl>
#include <algorithm>

namespace ashdesk {

QString Endpoint::address() const {
    return (host.contains(':') ? '[' + host + ']' : host) + ':' + QString::number(port);
}

DesktopOptions DesktopOptions::load(const QSettings &settings) {
    DesktopOptions options;
    options.theme = std::clamp(settings.value("appearance/theme", 0).toInt(), 0, 2);
    options.material = std::clamp(settings.value("appearance/material", 2).toInt(), 0, 3);
    options.authentication = std::clamp(settings.value("security/authentication", 0).toInt(), 0, 1);
    options.keyFile = settings.value("security/keyFile").toString();
    options.port = std::clamp(settings.value("network/port", 44100).toInt(), 1, 65535);
    options.fps = std::clamp(settings.value("quality/fps", 60).toInt(), 30, 144);
    options.bitrate = std::clamp(settings.value("quality/bitrate", 30).toInt(), 5, 100);
    options.audio = settings.value("audio/enabled", true).toBool();
    options.microphone = settings.value("audio/microphone", false).toBool();
    options.clipboard = settings.value("security/clipboard", true).toBool();
    options.files = settings.value("security/files", true).toBool();
    options.viewOnly = settings.value("security/viewOnly", false).toBool();
    options.metered = settings.value("quality/metered", false).toBool();
    options.hostControl = settings.value("security/hostControl", false).toBool();
    options.virtualDisplay = settings.value("host/virtualDisplay", false).toBool();
    options.language = std::clamp(settings.value("appearance/language", 0).toInt(), 0, 2);
    options.totp = std::clamp(settings.value("security/totp", 0).toInt(), 0, 2);
    options.rendezvous = settings.value("network/rendezvous").toString();
    options.numberHost = settings.value("host/number", true).toBool();
    // 11-26 起三档，存在 security/confirmMode。以前的 security/confirm 只有两档（0 电脑前有人时询问、1 每次都询问）：
    // 选过“每次都询问”的照旧，其余按新的默认（不问）
    options.confirm = settings.contains("security/confirmMode") ? std::clamp(settings.value("security/confirmMode").toInt(), 0, 2)
                      : settings.value("security/confirm", 0).toInt() == 1 ? 2 : 0;
    options.forwardAllow = settings.value("network/forwardAllow").toString();
    options.forward = settings.value("network/forward").toString();
    options.socks5 = std::clamp(settings.value("network/socks5", 0).toInt(), 0, 65535);
    return options;
}

void DesktopOptions::save(QSettings &settings) const {
    settings.setValue("appearance/theme", theme);
    settings.setValue("appearance/material", material);
    settings.setValue("security/authentication", authentication);
    settings.setValue("security/keyFile", keyFile);
    settings.setValue("network/port", port);
    settings.setValue("quality/fps", fps);
    settings.setValue("quality/bitrate", bitrate);
    settings.setValue("audio/enabled", audio);
    settings.setValue("audio/microphone", microphone);
    settings.setValue("security/clipboard", clipboard);
    settings.setValue("security/files", files);
    settings.setValue("security/viewOnly", viewOnly);
    settings.setValue("quality/metered", metered);
    settings.setValue("security/hostControl", hostControl);
    settings.setValue("host/virtualDisplay", virtualDisplay);
    settings.setValue("appearance/language", language);
    settings.setValue("security/totp", totp);
    settings.setValue("network/rendezvous", rendezvous);
    settings.setValue("host/number", numberHost);
    settings.setValue("security/confirmMode", confirm);
    settings.remove("security/confirm");
    settings.setValue("network/forwardAllow", forwardAllow);
    settings.setValue("network/forward", forward);
    settings.setValue("network/socks5", socks5);
    settings.sync();
}

bool parseEndpoint(const QString &text, int defaultPort, Endpoint &endpoint, QString &error) {
    QString address = text.trimmed();
    QHostAddress ip;
    if (ip.setAddress(address) && ip.protocol() == QAbstractSocket::IPv6Protocol)
        address = '[' + address + ']';
    const QUrl url("tcp://" + address, QUrl::StrictMode);
    const int port = url.port(defaultPort);
    if (address.isEmpty() || !url.isValid() || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || !url.path().isEmpty() || url.hasQuery() ||
        url.hasFragment() || address.contains(QRegularExpression("\\s")) ||
        port < 1 || port > 65535) {
        error = QCoreApplication::translate("ashdesk", "请输入主机地址，例如 192.168.1.23:44100 或 [::1]:44100");
        return false;
    }
    endpoint = {url.host(), port};
    error.clear();
    return true;
}

static void addAuthentication(QStringList &arguments, const DesktopOptions &options) {
    if (options.authentication == 1) arguments << "--psk-file" << QFileInfo(options.keyFile).absoluteFilePath();
    else arguments << "--psk-stored";
}

static QLocale interfaceLocale(int language);

namespace {

constexpr qsizetype kNumberDigits = 9;
constexpr qsizetype kNumberGroup = 3;
constexpr qsizetype kPasswordDigits = 8;
constexpr qsizetype kPasswordGroup = 4;
// 固定密码的长度和字符范围，同 proto/common/adp_dev.h 的 adp_password_kind
constexpr qsizetype kFixedPasswordMin = 8;
constexpr qsizetype kFixedPasswordMax = 64;
constexpr char16_t kPrintableFirst = 0x20;
constexpr char16_t kPrintableLast = 0x7E;
constexpr QStringView kAddressMarks = u".:@/";   // 地址里才有的字符：有它们的文字不当成“号码 + 密码”
// 全角数字 ０–９
constexpr char16_t kFullWidthZero = 0xFF10;
constexpr char16_t kFullWidthNine = 0xFF19;

// 只留数字，全角数字换成半角；*plain = 除了数字只有空格和连字符
QString digitsOf(const QString &text, bool *plain) {
    QString digits;
    *plain = true;
    for (const QChar c : text) {
        if (c >= u'0' && c <= u'9') {
            digits += c;
        } else if (c >= QChar(kFullWidthZero) && c <= QChar(kFullWidthNine)) {
            digits += QChar(c.unicode() - kFullWidthZero + u'0');
        } else if (!c.isSpace() && c != u'-') {
            *plain = false;
        }
    }
    return digits;
}

// 客户端两种连法共用的选项
void addClientOptions(QStringList &arguments, const DesktopOptions &options) {
    if (!options.forward.isEmpty()) arguments << "--forward" << options.forward;
    if (options.socks5 != 0) arguments << "--socks5" << QString::number(options.socks5);
    arguments << "--clip" << (options.clipboard ? "text" : "off");
    arguments << "--files" << (options.files ? "both" : "off");
    arguments << "--metered" << (options.metered ? "on" : "auto");
    if (!options.audio) arguments << "--no-audio";
    if (options.microphone) arguments << "--mic";
    if (options.viewOnly) arguments << "--no-input";
    // 客户端只有中文和英文：界面不是中文就用英文，和 useLanguage 里没有译文退回英文是同一条规则
    if (interfaceLocale(options.language).language() != QLocale::Chinese) arguments << "--lang" << "en";
}
} // namespace

QStringList clientArguments(const Endpoint &endpoint, const DesktopOptions &options, const QString &totp) {
    QStringList arguments{"--host", endpoint.host, "--port", QString::number(endpoint.port), "--ui-control"};
    addAuthentication(arguments, options);
    if (!totp.isEmpty()) {
        arguments << "--totp" << totp;
    }
    addClientOptions(arguments, options);
    return arguments;
}

QString signalServer(const DesktopOptions &options) {
    const QString server = options.rendezvous.trimmed();
    // 内置的信令服务器（号码连接设计 §9 第 4 条）
    return server.isEmpty() ? QStringLiteral("5.253.17.21:44200") : server;
}

int parseNumber(const QString &text, QString &number) {
    bool plain = false;
    const QString digits = digitsOf(text, &plain);
    if (!plain || digits.isEmpty()) {
        return 0;
    }
    if (digits.size() != kNumberDigits || digits.front() == u'0') {
        return -1;
    }
    number = digits;
    return 1;
}

QString groupDigits(const QString &digits) {
    const qsizetype group = digits.size() == kPasswordDigits ? kPasswordGroup : kNumberGroup;
    QString grouped;
    for (qsizetype i = 0; i < digits.size(); i += group) {
        if (i != 0) {
            grouped += u' ';
        }
        grouped += digits.mid(i, group);
    }
    return grouped;
}

bool validFixedPassword(const QString &text) {
    if (text.size() < kFixedPasswordMin || text.size() > kFixedPasswordMax || text.front() == u' ' || text.back() == u' ') {
        return false;
    }
    bool mixed = false;                          // 有不是数字、空格、连字符的字符
    for (const QChar c : text) {
        if (c < QChar(kPrintableFirst) || c > QChar(kPrintableLast)) {
            return false;
        }
        mixed = mixed || ((c < u'0' || c > u'9') && c != u' ' && c != u'-');
    }
    return mixed;
}

QString parsePassword(const QString &text) {
    bool plain = false;
    const QString digits = digitsOf(text, &plain);
    if (plain && digits.size() == kPasswordDigits) {
        return digits;
    }
    const QString fixed = text.trimmed();
    return validFixedPassword(fixed) ? fixed : QString();
}

bool parseNumberTarget(const QString &text, NumberTarget &target) {
    if (std::any_of(text.cbegin(), text.cend(), [](QChar c) { return kAddressMarks.contains(c); })) {
        return false;
    }
    bool plain = false;
    const QString digits = digitsOf(text, &plain);
    if (digits.size() != kNumberDigits + kPasswordDigits || digits.front() == u'0') {
        return false;
    }
    target.number = digits.first(kNumberDigits);
    target.password = digits.sliced(kNumberDigits);
    return true;
}

QStringList numberArguments(const NumberTarget &target, const DesktopOptions &options) {
    QStringList arguments{"--connect", target.number + u'@' + signalServer(options)};
    if (!target.password.isEmpty()) {
        arguments << "--password" << "-";
    }
    arguments << "--ui-control";
    addClientOptions(arguments, options);
    return arguments;
}

QStringList directArguments(const Endpoint &endpoint, bool password, const DesktopOptions &options) {
    QStringList arguments{"--host", endpoint.host, "--port", QString::number(endpoint.port), "--by-number"};
    if (password) {
        arguments << "--password" << "-";
    }
    arguments << "--ui-control";
    addClientOptions(arguments, options);
    return arguments;
}

QStringList identityArguments(bool out, const QString &file, const QString &dataDirectory) {
    return {out ? "--identity-export" : "--identity-import", QDir::toNativeSeparators(file), "--identity-dir",
            QDir::toNativeSeparators(dataDirectory), "--ui-control"};
}

QStringList hostArguments(const DesktopOptions &options, Pairing pairing, const HostStore &store) {
    QStringList arguments{"--port", QString::number(options.port), "--ui-control",
                          "--keep-desktop", "--takeover-confirm",
                          options.confirm == 2 ? "always" : options.confirm == 1 ? "attended" : "never",
                          "--takeover-default", "deny", "--control-indicator"};
    // 号码连接开着、又没存过共享密钥：不带 --psk-stored（带了核心会因为没有密钥退出），对方靠连接密码或已配对的设备密钥进来
    if (!options.numberHost || options.authentication == 1 || store.storedKey) {
        addAuthentication(arguments, options);
    }
    if (options.numberHost) {
        arguments << "--rendezvous" << signalServer(options) << "--number";
        if (!store.numberFile.isEmpty()) {
            arguments << "--number-file" << store.numberFile;
        }
        if (pairing == Pairing::None) {
            arguments << "--password";
        }
    }
    if (options.totp != 0) arguments << "--totp" << (options.totp == 1 ? "pair" : "session");
    if (pairing != Pairing::None) arguments << (pairing == Pairing::Device ? "--pair" : "--pair-temp");
    if (!options.forwardAllow.isEmpty()) arguments << "--forward-allow" << options.forwardAllow;
    if (options.virtualDisplay) arguments << "--capture" << "idd";
    arguments << "--fps" << QString::number(options.fps);
    arguments << "--bitrate" << QString::number(options.bitrate);
    arguments << "--audio" << (options.audio ? "auto" : "off");
    arguments << "--clip" << (options.clipboard ? "text" : "off");
    arguments << "--files" << (options.files ? "both" : "off");
    if (options.hostControl) arguments << "--inject";
    return arguments;
}

std::optional<QStringList> serviceHostArguments(const DesktopOptions &options, const QString &directory) {
    const QDir data(directory);
    QStringList arguments = hostArguments(options, Pairing::None, HostStore{false, QDir::toNativeSeparators(data.filePath("number.txt"))});
    if (directory.isEmpty() || arguments.contains("--psk-stored") || arguments.contains("--totp")) {
        return std::nullopt;
    }
    arguments.removeAll("--ui-control");
    return arguments << "--log" << QDir::toNativeSeparators(data.filePath("host.log"));
}

static bool upgradeSettings(const QString &fileName);

QString prepareSettings(const QString &fileName) {
    if (upgradeSettings(fileName)) return {};
    const QString backup = fileName + ".bad-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");
    if (!QFile::rename(fileName, backup))
        return QCoreApplication::translate("ashdesk", "配置文件 %1 无法读取，也无法备份；修改设置会覆盖它")
            .arg(QDir::toNativeSeparators(fileName));
    return QCoreApplication::translate("ashdesk", "配置无法读取，已备份为 %1 并恢复默认设置")
        .arg(QDir::toNativeSeparators(backup));
}

// 能读懂就迁移到当前版本并写上版本号；没有版本号的是版本 1
static bool upgradeSettings(const QString &fileName) {
    QSettings settings(fileName, QSettings::IniFormat);
    bool numeric = true;
    const int version = settings.value("meta/version", 1).toInt(&numeric);
    if (settings.status() != QSettings::NoError || !numeric || version < 1 || version > kSettingsVersion) return false;
    if (version < 2) {
        QList<RecentConnection> recent;
        for (const QString &address : settings.value("connections/recent").toStringList()) recent.append({address, {}, {}});
        settings.remove("connections/recent");
        saveRecent(settings, recent);
    }
    settings.setValue("meta/version", kSettingsVersion);
    return true;
}

QList<RecentConnection> loadRecent(QSettings &settings) {
    QList<RecentConnection> recent;
    const int count = settings.beginReadArray("recent");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString address = settings.value("address").toString();
        if (!address.isEmpty())
            recent.append({address, settings.value("alias").toString(), settings.value("group").toString()});
    }
    settings.endArray();
    return recent;
}

void saveRecent(QSettings &settings, const QList<RecentConnection> &recent) {
    settings.remove("recent");
    settings.beginWriteArray("recent", int(recent.size()));
    for (int i = 0; i < recent.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("address", recent[i].address);
        settings.setValue("alias", recent[i].alias);
        settings.setValue("group", recent[i].group);
    }
    settings.endArray();
    settings.sync();
}

void rememberRecent(QList<RecentConnection> &recent, const QString &address) {
    RecentConnection entry{address, {}, {}};
    const auto known = std::find_if(recent.begin(), recent.end(),
                                    [&](const RecentConnection &item) { return item.address == address; });
    if (known != recent.end()) {
        entry = *known;
        recent.erase(known);
    }
    recent.prepend(entry);
    qsizetype unnamed = 0;
    for (auto it = recent.begin(); it != recent.end();)
        it = it->alias.isEmpty() && it->group.isEmpty() && ++unnamed > kRecentLimit ? recent.erase(it) : it + 1;
}

void useLanguage(int language) {
    static QTranslator texts;
    static QTranslator qtTexts;
    QCoreApplication::removeTranslator(&texts);
    QCoreApplication::removeTranslator(&qtTexts);
    const QLocale locale = interfaceLocale(language);
    // 源文字是中文；暂无译文的语言退回英文
    const QString resources = QStringLiteral(":/i18n");
    if (locale.language() != QLocale::Chinese &&
        (texts.load(locale, "ashdesk", "_", resources) || texts.load(QLocale(QLocale::English), "ashdesk", "_", resources)))
        QCoreApplication::installTranslator(&texts);
    if (qtTexts.load(locale, "qtbase", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(&qtTexts);
}

// 跟随系统看的是界面显示语言（uiLanguages 首项），不是区域格式；也不能把整个 uiLanguages 交给 load，
// 中文系统常把英文列在第二位，会因为没有 ashdesk_zh 而选中英文
static QLocale interfaceLocale(int language) {
    return language == 1   ? QLocale(QLocale::Chinese, QLocale::China)
           : language == 2 ? QLocale(QLocale::English, QLocale::UnitedStates)
                           : QLocale(QLocale::system().uiLanguages().value(0));
}

} // namespace ashdesk
