#include "kc/config/ClientConfig.h"

#include "kc/config/AppStatePaths.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcessEnvironment>
#include <QUrl>

namespace kc {
namespace {

// 最近一次被拒的地址（文件级静态，供 LastRejectedServerUrl() 读取）。
QString& lastRejected()
{
    static QString v;
    return v;
}

} // namespace

const QString ClientConfig::EnvVarName = QStringLiteral("KUNCLIENT_SERVER_URL");
const QString ClientConfig::AllowCustomServerVarName = QStringLiteral("KUNCLIENT_ALLOW_CUSTOM_SERVER");
const QString ClientConfig::ConfigFileName = QStringLiteral("KunClient.config.json");
const QString ClientConfig::LocalConfigFileName = QStringLiteral("KunClient.config.local.json");

QString ClientConfig::DefaultServerUrl()
{
    // 片段拼接：不做成单个字面量，避免成为「字符串加密碰不到」的元数据常量。
    return QStringLiteral("https://") + QStringLiteral("your-domain") + QStringLiteral(".example.com");
}

QString ClientConfig::ConfigFilePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(ConfigFileName);
}

QString ClientConfig::LocalConfigFilePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(LocalConfigFileName);
}

QString ClientConfig::UserConfigFilePath()
{
    return AppStatePaths::Combine(ConfigFileName);
}

QString ClientConfig::LegacyUserConfigFilePath()
{
    return AppStatePaths::Legacy(ConfigFileName);
}

QString ClientConfig::ServerUrl()
{
    const QString v = ResolveServerUrl();
    return v.isEmpty() ? DefaultServerUrl() : v;
}

QString ClientConfig::ResolveServerUrl()
{
    QString v = Accept(QProcessEnvironment::systemEnvironment().value(EnvVarName));
    if (!v.isEmpty()) return v;

    // UserConfigFilePath（新状态目录）→ LegacyUserConfigFilePath（旧的、会被清理的目录）
    // → 程序同目录 → 内嵌。旧目录那条只为老用户升级后不丢服务器地址。
    const QStringList files = {
        UserConfigFilePath(), LegacyUserConfigFilePath(), LocalConfigFilePath(), ConfigFilePath()
    };
    for (const QString& file : files) {
        v = Accept(ReadServerUrl(file));
        if (!v.isEmpty()) return v;
    }

    return Accept(ReadServerUrlFromEmbedded());
}

QString ClientConfig::Accept(const QString& url)
{
    const QString v = Normalize(url);
    if (v.isEmpty()) return {};

    if (IsTrustedHost(v)) return v;

    // 🔴 不写死「只认自家域名」以外的判断进常量：白名单靠 IsTrustedHost 一处维护。
    if (CustomServerAllowed()) return v;

    lastRejected() = v;
    return {};
}

bool ClientConfig::CustomServerAllowed()
{
    const QString v = QProcessEnvironment::systemEnvironment().value(AllowCustomServerVarName);
    return v == QStringLiteral("1") || v.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

bool ClientConfig::IsTrustedHost(const QString& url)
{
    const QUrl u(url, QUrl::StrictMode);
    if (!u.isValid()) return false;

    QString host = u.host().toLower();
    // IPv6 会带方括号，去掉后与 "::1" 比较。
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
    if (host.isEmpty()) return false;

    const QStringList domains = OfficialDomains();
    for (const QString& d : domains) {
        // 别只写 endsWith：那样后缀伪造（kunawa.cn.evil.com）能过。
        if (host == d || host.endsWith(QStringLiteral(".") + d)) return true;
    }

    return IsLocalHost(host);
}

QStringList ClientConfig::OfficialDomains()
{
    // 拼接而非字面量：与 DefaultServerUrl 同因（避免进常量表）。
    return { QStringLiteral("your-domain") + QStringLiteral(".example.com") };
}

bool ClientConfig::IsLocalHost(const QString& host)
{
    if (host == QStringLiteral("localhost") || host == QStringLiteral("127.0.0.1")
        || host == QStringLiteral("::1") || host == QStringLiteral("0.0.0.0"))
        return true;

    if (host.endsWith(QStringLiteral(".local")) || host.endsWith(QStringLiteral(".lan"))
        || host.endsWith(QStringLiteral(".internal")))
        return true;

    const QStringList parts = host.split(QLatin1Char('.'));
    if (parts.size() == 4) {
        bool okA = false, okB = false;
        const int a = parts[0].toInt(&okA);
        const int b = parts[1].toInt(&okB);
        if (okA && okB) {
            if (a == 10) return true;                              // 10.0.0.0/8
            if (a == 192 && b == 168) return true;                 // 192.168.0.0/16
            if (a == 172 && b >= 16 && b <= 31) return true;       // 172.16.0.0/12
            if (a == 169 && b == 254) return true;                 // 169.254.0.0/16
        }
    }
    return false;
}

QString ClientConfig::Normalize(const QString& url)
{
    QString v = url.trimmed();
    if (v.isEmpty()) return {};
    while (v.endsWith(QLatin1Char('/'))) v.chop(1);

    // 只写了域名时补 **https**，不是 http —— 明文会把凭据摆在共享网络里。
    if (!v.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
        !v.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
        v = QStringLiteral("https://") + v;

    if (IsInsecureRemote(v)) return {};
    return v;
}

bool ClientConfig::IsInsecureRemote(const QString& url)
{
    // https 一律放行
    if (!url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) return false;

    const QUrl u(url, QUrl::StrictMode);
    if (!u.isValid()) return true;   // 解析不出主机名时按不安全处理

    QString host = u.host().toLower();
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
    if (host.isEmpty()) return true;

    // 复用同一份「本地主机」判定，避免两处规则各自漂移
    return !IsLocalHost(host);
}

QString ClientConfig::LastRejectedServerUrl()
{
    return lastRejected();
}

QString ClientConfig::ReadServerUrlFromEmbedded()
{
    // App 项目内以 Qt 资源内嵌，路径 :/KunClient.config.json。
    QFile f(QStringLiteral(":/") + ConfigFileName);
    if (!f.open(QIODevice::ReadOnly)) return {};

    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return {};
    const QJsonValue v = doc.object().value(QStringLiteral("serverUrl"));
    if (v.isString()) return v.toString();
    return {};
}

QString ClientConfig::ReadServerUrl(const QString& file)
{
    if (!QFile::exists(file)) return {};
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return {};

    // 配置文件损坏时回退下一优先级（解析失败静默返回空）。
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};

    const QJsonValue v = doc.object().value(QStringLiteral("serverUrl"));
    if (v.isString()) return v.toString();
    return {};
}

} // namespace kc
