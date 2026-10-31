#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  UpdateVerifier —— 更新包的**完整性 + 真实性**校验
//
//  🔴 这个文件的存在理由，写在 SECURITY.md 的「已知安全问题 › 严重」第一条：
//     「更新包无签名校验：UpdateService 只检查文件头是否为 MZ，未做 Authenticode
//       验证。v2.0.6 已移除自动更新功能，此风险随之消除。」
//     也就是说 —— **一旦把自动更新加回来，这个严重漏洞就会跟着回来**。
//     所以这次是「先补校验，再做更新」，不是反过来。
//
//  ── 两层校验，缺一不可 ──────────────────────────────────────────────────
//    ① SHA-256 完整性：确认下载的文件没被截断 / 损坏。
//       只能防「传输损坏」，**防不住恶意替换** —— 因为哈希值也是从同一个
//       服务器下发的，中间人能同时改包和哈希。
//    ② ECDSA P-256 真实性：服务器用**私钥**签「版本号 + 文件名 + SHA-256」，
//       客户端内置**公钥**验签。私钥不在服务器上（构建时离线签），
//       所以即使服务器被拿下，也签不出能通过校验的恶意包。
//
//  🔴 只做 ① 不做 ② 等于没做安全校验，只是把「明文的信任」换成了「哈希的信任」。
//
//  ── 为什么用 ECDSA P-256 而不是 Ed25519 ────────────────────────────────
//    Windows CNG（BCrypt）原生支持 ECDSA P-256/P-384/P-521，
//    而 Ed25519 要额外引入 libsodium 或 OpenSSL（项目刻意避免：
//    QCryptographicHash 拖进 OpenSSL 会让单文件 exe 多 4.4MB，见 Sha256.h 注释）。
//    P-256 的 128 位安全强度对「更新包签名」这个场景绰绰有余。
//
//  ── 签名清单格式（服务器下发，客户端验）────────────────────────────────
//    JSON：
//      {
//        "version": "2.0.8",
//        "file": "KunClient-2.0.8.exe",
//        "sha256": "<64 hex>",
//        "sig": "<base64(ECDSA-P256-SHA256 签名)>"
//      }
//    签名覆盖的明文是：version + "\n" + file + "\n" + sha256
//    （字段顺序固定，换行分隔 —— 与 ApiClient 的签名串同一个风格）
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QString>

namespace kc::update {

/// 校验结果。
struct VerifyResult {
    bool ok = false;
    bool hashOk = false;    ///< SHA-256 是否匹配
    bool sigOk = false;     ///< ECDSA 签名是否有效
    QString error;          ///< 失败原因（中文，可直接展示）
};

/// 内置的更新签名公钥（P-256，未压缩点格式 65 字节的 base64）。
///
/// 🔴 这是**公钥**，可以公开、也必须内置在客户端里。
///    对应私钥只存在于构建机的离线签名脚本里，绝不进仓库、绝不上服务器。
///    轮换密钥 = 发一个内置新公钥的版本，所以只在私钥泄露时才换。
///
/// 空串 = 尚未配置（此时 verifyPackage 会**拒绝**通过，而不是放行）。
/// 这是刻意的失败关闭（fail-closed）设计。
const char* updatePublicKeyBase64();

/// 计算文件的 SHA-256（hex 小写）。文件读不了返回空串。
QString fileSha256(const QString& path);

/// 校验一个已下载的更新包。
///   packagePath : 本地文件
///   expectedSha256 : 服务器下发的哈希（64 hex）
///   signatureBase64 : 服务器下发的 ECDSA 签名
///   signedPayload  : 被签名的明文（version\nfile\nsha256）
///
/// 🔴 严格程度：
///   · 公钥未配置 → 直接失败（不静默放行）
///   · 哈希不匹配 → 失败，且**不再**验签（省一次开销，反正已经不可信）
///   · 签名无效 → 失败
///   · 只有两者都通过才 ok=true
VerifyResult verifyPackage(const QString& packagePath,
                           const QString& expectedSha256,
                           const QString& signatureBase64,
                           const QByteArray& signedPayload);

/// 从服务器下发的 JSON 里解析出「待校验三件套」。
/// 缺字段 / 格式不对返回 false，error 说明缺什么。
struct PackageManifest {
    QString version;
    QString file;
    QString sha256;
    QString signatureBase64;
    QByteArray signedPayload() const;   ///< version\nfile\nsha256
};
bool parseManifest(const QByteArray& json, PackageManifest* out, QString* error);

/// 只验签（不比对文件哈希）。给测试与诊断用 —— 生产路径走 verifyPackage。
///
/// 存在的理由：整条链路里最容易悄悄坏的是**签名格式**（Node 的 crypto.sign
/// 产出 DER，Windows CNG 默认吃 P1363），一旦对不上就是「永远验不过」，
/// 而且报错只说「签名无效」，看不出是格式问题。单测必须能直接打这一层。
bool verifySignatureOnly(const QByteArray& signedPayload, const QString& signatureBase64);

} // namespace kc::update
