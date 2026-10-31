#include "kc/net/ApiClient.h"

#include "kc/security/Sha256.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QUrl>

#include <functional>

namespace kc::net {

namespace {

/// 生成一次性 nonce（服务端记 usedNonces 防重放，所以必须每次不同）。
/// 16 字节随机 → hex。QRandomGenerator::global() 是线程安全的。
QString makeNonce()
{
    QByteArray raw(16, Qt::Uninitialized);
    for (int i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<char>(QRandomGenerator::global()->bounded(256));
    }
    return QString::fromLatin1(raw.toHex());
}

QString sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(kc::crypto::sha256(data).toHex());
}

/// 把服务端返回的 JSON 错误体转成给人看的中文。
/// 服务端错误体形如 { success:false, code:"SIG_EXPIRED", message:"..." }。
void fillErrorFromBody(ApiResult& r)
{
    const QJsonDocument doc = QJsonDocument::fromJson(r.body);
    if (!doc.isObject()) return;
    const QJsonObject o = doc.object();
    const QString msg = o.value(QStringLiteral("message")).toString();
    const QString code = o.value(QStringLiteral("code")).toString();
    if (!code.isEmpty()) r.errorCode = code;
    if (!msg.isEmpty()) r.error = msg;
}

} // namespace

ApiClient::ApiClient(QObject* parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

ApiClient::~ApiClient()
{
    // secret 只存在内存里，析构时主动擦一遍（避免残留在堆页里被 dump 到）。
    m_secret.fill('\0');
}

void ApiClient::setBaseUrl(const QString& base)
{
    m_base = base;
    while (m_base.endsWith(QLatin1Char('/'))) m_base.chop(1);
}

qint64 ApiClient::correctedNowMs() const
{
    return QDateTime::currentMSecsSinceEpoch() + m_serverSkewMs;
}

void ApiClient::signRequest(QNetworkRequest& req, const QByteArray& method,
                            const QString& path, const QByteArray& body)
{
    if (!signedIn()) return;

    const QString ts = QString::number(correctedNowMs());
    const QString nonce = makeNonce();
    // 🔴 签名串的分隔符是 \n，顺序固定：ts / nonce / METHOD / path / bodyHash。
    //    与 server.js 的 sigGuard 逐字对应，改任何一处都要两边同时改。
    const QString toSign = ts + QLatin1Char('\n') + nonce + QLatin1Char('\n')
                           + QString::fromLatin1(method) + QLatin1Char('\n')
                           + path + QLatin1Char('\n') + sha256Hex(body);
    const QByteArray mac = kc::crypto::hmacSha256(m_secret, toSign.toUtf8());

    req.setRawHeader("X-KC-Sid", m_sid.toUtf8());
    req.setRawHeader("X-KC-Ts", ts.toUtf8());
    req.setRawHeader("X-KC-Nonce", nonce.toUtf8());
    req.setRawHeader("X-KC-Sig", mac.toBase64());
}

void ApiClient::handshake(std::function<void(bool, const QString&)> done)
{
    if (m_base.isEmpty()) {
        done(false, QStringLiteral("服务器地址为空"));
        return;
    }

    const qint64 localSentMs = QDateTime::currentMSecsSinceEpoch();
    QNetworkRequest req(QUrl(m_base + QStringLiteral("/api/v2/client/handshake")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    // 握手是公开端点，**不需要**签名（服务端把它放在 SIG_REQUIRED_PATHS 之外）。
    QNetworkReply* reply = m_nam->post(req, QByteArray("{}"));

    connect(reply, &QNetworkReply::finished, this, [this, reply, done, localSentMs] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();

        if (reply->error() != QNetworkReply::NoError) {
            // 429 = 握手限流（30 次/小时/IP），这条要单独说明，
            // 否则用户只会看到「连接失败」然后反复重试。
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString why = (status == 429)
                                    ? QStringLiteral("请求太频繁，请稍后再试")
                                    : reply->errorString();
            done(false, why);
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) {
            done(false, QStringLiteral("服务器返回格式异常"));
            return;
        }
        const QJsonObject root = doc.object();
        if (!root.value(QStringLiteral("success")).toBool()) {
            done(false, root.value(QStringLiteral("message"))
                            .toString(QStringLiteral("握手失败")));
            return;
        }
        const QJsonObject data = root.value(QStringLiteral("data")).toObject();
        m_sid = data.value(QStringLiteral("sid")).toString();
        m_secret = data.value(QStringLiteral("secret")).toString().toUtf8();
        const qint64 expiresIn = static_cast<qint64>(
            data.value(QStringLiteral("expiresIn")).toDouble());
        const qint64 serverTime = static_cast<qint64>(
            data.value(QStringLiteral("serverTime")).toDouble());

        // 🔴 时钟偏差校正：服务端只接受 ±窗口内的时间戳，本机时钟不准会直接
        //    401 SIG_EXPIRED。这里用「服务端时间 - 本机时间」的差值，之后每次
        //    签名都用校正后的时间。用「发送时刻」而不是「收到时刻」估本机时间，
        //    把往返延迟的一半误差也算进去了。
        if (serverTime > 0) m_serverSkewMs = serverTime - localSentMs;
        m_secretExpiresAtMs = correctedNowMs() + expiresIn * 1000;

        if (m_sid.isEmpty() || m_secret.isEmpty()) {
            done(false, QStringLiteral("服务器未下发签名会话"));
            return;
        }
        done(true, QString());
    });
}

void ApiClient::get(const QString& path, std::function<void(const ApiResult&)> done)
{
    QNetworkRequest req(QUrl(m_base + path));
    signRequest(req, "GET", path, QByteArray());

    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        ApiResult r;
        r.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        r.ok = (reply->error() == QNetworkReply::NoError) && r.httpStatus >= 200
               && r.httpStatus < 300;
        if (!r.ok) {
            fillErrorFromBody(r);
            if (r.error.isEmpty()) r.error = reply->errorString();
        }
        done(r);
    });
}

void ApiClient::post(const QString& path, const QByteArray& jsonBody,
                     std::function<void(const ApiResult&)> done)
{
    QNetworkRequest req(QUrl(m_base + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    signRequest(req, "POST", path, jsonBody);

    QNetworkReply* reply = m_nam->post(req, jsonBody);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        ApiResult r;
        r.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.body = reply->readAll();
        r.ok = (reply->error() == QNetworkReply::NoError) && r.httpStatus >= 200
               && r.httpStatus < 300;
        if (!r.ok) {
            fillErrorFromBody(r);
            if (r.error.isEmpty()) r.error = reply->errorString();
        }
        done(r);
    });
}

void ApiClient::download(const QString& url, const QString& localPath,
                         std::function<void(qint64, qint64)> progress,
                         std::function<void(const ApiResult&)> done)
{
    // 站内相对路径（/assets/x.exe）要补全成绝对地址。
    const QString abs = url.startsWith(QLatin1String("http"))
                            ? url
                            : m_base + (url.startsWith(QLatin1Char('/')) ? url : (QLatin1Char('/') + url));

    QNetworkRequest req{QUrl(abs)};
    // 下载**不签名**（票据在 query 里，是另一套鉴权）。
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* reply = m_nam->get(req);
    QFile* out = new QFile(localPath);
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString why = out->errorString();
        delete out;
        reply->abort();
        reply->deleteLater();
        ApiResult r;
        r.error = QStringLiteral("无法写入文件：%1").arg(why);
        done(r);
        return;
    }

    connect(reply, &QNetworkReply::readyRead, this, [reply, out] {
        out->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [progress](qint64 got, qint64 total) {
        if (progress) progress(got, total);
    });
    connect(reply, &QNetworkReply::finished, this, [reply, out, done] {
        out->write(reply->readAll());
        out->flush();
        out->close();
        const QString ioError = out->errorString();
        const bool ioOk = (out->error() == QFile::NoError);
        delete out;
        reply->deleteLater();

        ApiResult r;
        r.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        r.ok = (reply->error() == QNetworkReply::NoError) && r.httpStatus >= 200
               && r.httpStatus < 300 && ioOk;
        if (!r.ok) {
            r.error = ioOk ? reply->errorString() : QStringLiteral("写入失败：%1").arg(ioError);
        }
        done(r);
    });
}

} // namespace kc::net
