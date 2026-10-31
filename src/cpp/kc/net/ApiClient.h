#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ApiClient —— v2 接口客户端（握手 + HMAC 签名 + 统一错误口径）
//
//  为什么要有这一层：自动更新 / 联网功能都需要「带签名的请求」，而签名协议
//  有 5 个容易写错的细节，散在各处一定会漂。集中在这里一次写对。
//
//  ── 协议（与服务端 _archive/server/server.js 的 sigGuard 逐字对应）──
//    ① POST /api/v2/client/handshake
//       → { sid, secret, expiresIn, serverTime }
//       secret 只在内存里，**不落盘**（它是本次会话的签名密钥）。
//    ② 每个请求带四个头：
//         X-KC-Sid   : sid
//         X-KC-Ts    : 毫秒时间戳
//         X-KC-Nonce : 一次性随机串（服务端记 usedNonces，重放直接 401）
//         X-KC-Sig   : base64(HMAC-SHA256(secret,
//                        ts + "\n" + nonce + "\n" + METHOD + "\n" + path + "\n" + bodyHash))
//       其中 bodyHash = sha256hex(rawBody)，无 body 时是空串的 sha256。
//    ③ 时间戳窗口：服务端允许 ±SIG_TS_WINDOW（默认 5 分钟）。本机时钟不准会
//       直接 401 SIG_EXPIRED —— 所以握手时记下 serverTime 与本机时间的差，
//       之后发请求用「校正后的时间戳」。这是很多人踩过的坑。
//
//  🔴 密钥不落盘、不写日志。secret 只存在对象内存里，析构即失效。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace kc::net {

/// 一次请求的结果。ok=false 时 error 是给人看的中文说明。
struct ApiResult {
    bool ok = false;
    int httpStatus = 0;
    QByteArray body;      ///< 原始响应体（JSON 由调用方解析）
    QString error;        ///< 失败原因（中文，可直接展示）
    QString errorCode;    ///< 服务端 code 字段（SIG_EXPIRED / BANNED 等），可能为空
};

/// v2 客户端。**非线程安全**：只在 UI 线程用（Qt 的网络本来就是异步的）。
class ApiClient : public QObject {
    Q_OBJECT

public:
    explicit ApiClient(QObject* parent = nullptr);
    ~ApiClient() override;

    /// 服务端根地址，如 "https://kunawa.cn"（不带尾斜杠）。
    void setBaseUrl(const QString& base);
    QString baseUrl() const { return m_base; }

    /// 是否已完成握手且 secret 未过期。
    bool signedIn() const { return !m_sid.isEmpty() && !m_secret.isEmpty(); }

    /// 握手（拿 sid + secret）。失败时 error 说明原因。
    /// 回调在 UI 线程执行。
    void handshake(std::function<void(bool ok, const QString& error)> done);

    /// 发一个 GET 请求（自动签名）。
    void get(const QString& path, std::function<void(const ApiResult&)> done);

    /// 发一个 POST 请求（自动签名；body 参与 bodyHash）。
    void post(const QString& path, const QByteArray& jsonBody,
              std::function<void(const ApiResult&)> done);

    /// 下载一个文件到本地（带票据时拼 ?t=...）。
    /// progress 回调：(已收字节, 总字节)；总字节未知时为 -1。
    /// 返回的 ApiResult.body 为空，成败看 ok / error。
    void download(const QString& url, const QString& localPath,
                  std::function<void(qint64 received, qint64 total)> progress,
                  std::function<void(const ApiResult&)> done);

private:
    /// 组装签名头。path 必须是「参与签名的那一段」（与服务器 req.path 同口径：
    /// 含 query 的完整 path）。
    void signRequest(QNetworkRequest& req, const QByteArray& method,
                     const QString& path, const QByteArray& body);

    /// 用握手得到的 serverTime 偏差校正本机时间戳。
    qint64 correctedNowMs() const;

    QNetworkAccessManager* m_nam = nullptr;
    QString m_base;
    QString m_sid;
    QByteArray m_secret;
    qint64 m_serverSkewMs = 0;   ///< 服务端时间 - 本机时间
    qint64 m_secretExpiresAtMs = 0;
};

} // namespace kc::net
