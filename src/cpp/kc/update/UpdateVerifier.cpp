#include "kc/update/UpdateVerifier.h"

#include "kc/security/Sha256.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <windows.h>
#include <bcrypt.h>

#ifndef NT_SUCCESS
#  define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace kc::update {

namespace {

/// 内置公钥。**占位**：真实部署时要换成构建机生成的公钥。
///
/// 格式：未压缩点（0x04 || X(32) || Y(32)）的 base64，共 65 字节。
/// 为什么留空而不是填个假值：假值会让「验签永远失败」，比留空更难排查。
/// 留空时 verifyPackage 会明确报「未配置签名公钥」，fail-closed。
const char* kUpdatePublicKeyB64 =
    "BOaoZdVSiImqT8ikviYQpyLzCpnqLKQu2sebEVSeOCsavyvgW4mJZ8DCy/eTrUZlt5+YIdPvHEBNeqlUO8FBYZE=";

/// BCrypt 句柄的 RAII 包装（BCryptOpenAlgorithmProvider 必须配对 Close）。
class AlgHandle {
public:
    AlgHandle() = default;
    ~AlgHandle() { if (m_h) BCryptCloseAlgorithmProvider(m_h, 0); }
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;

    bool open(LPCWSTR algId)
    {
        return NT_SUCCESS(BCryptOpenAlgorithmProvider(&m_h, algId, nullptr, 0));
    }
    BCRYPT_ALG_HANDLE get() const { return m_h; }
    bool valid() const { return m_h != nullptr; }

private:
    BCRYPT_ALG_HANDLE m_h = nullptr;
};

/// BCrypt 公钥句柄的 RAII 包装。
class KeyHandle {
public:
    KeyHandle() = default;
    ~KeyHandle() { if (m_h) BCryptDestroyKey(m_h); }
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    BCRYPT_KEY_HANDLE* addr() { return &m_h; }
    BCRYPT_KEY_HANDLE get() const { return m_h; }
    bool valid() const { return m_h != nullptr; }

private:
    BCRYPT_KEY_HANDLE m_h = nullptr;
};

/// 把「未压缩点」格式的公钥（65 字节）塞进 BCRYPT_ECCKEY_BLOB。
///
/// 🔴 这个格式很容易写错：BCrypt 要的是
///    { magic(4) | cbKey(4) | X(cbKey) | Y(cbKey) }，
///    magic 用 BCRYPT_ECDSA_PUBLIC_P256_MAGIC，cbKey = 32。
///    直接把 65 字节丢给 BCryptImportKeyPair 会失败（返回 STATUS_INVALID_PARAMETER）。
QByteArray makeP256PublicBlob(const QByteArray& uncompressedPoint)
{
    // 未压缩点：0x04 || X(32) || Y(32)
    if (uncompressedPoint.size() != 65 || static_cast<unsigned char>(uncompressedPoint[0]) != 0x04) {
        return {};
    }
    BCRYPT_ECCKEY_BLOB header{};
    header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header.cbKey = 32;   // P-256 的坐标长度

    QByteArray blob(reinterpret_cast<const char*>(&header), sizeof(header));
    blob.append(uncompressedPoint.mid(1, 32));    // X
    blob.append(uncompressedPoint.mid(33, 32));   // Y
    return blob;
}

/// DER 编码的 ECDSA 签名 → IEEE P1363（r||s 各 32 字节）。
///
/// 🔴 为什么必须有这一步：Node 的 crypto.sign() 产出 **DER**（SEQUENCE { INTEGER r, INTEGER s }），
///    而 Windows CNG 的 BCryptVerifySignature 只吃 **P1363**（定长 r||s）。
///    直接把 DER 丢进去，验签会**永远失败**，而且报错只是「签名无效」——
///    完全看不出是格式问题。实测（2026-10-31）：不转换时 CNG 验不通 Node 的签名。
///
/// DER 结构：30 <len> 02 <rLen> <r> 02 <sLen> <s>
///   r / s 可能是 33 字节（最高位为 1 时补 0x00），也可能是 31 字节（前导零省略）——
///   都要补齐 / 裁到 32 字节。
QByteArray derToP1363(const QByteArray& der)
{
    // P1363 原样（64 字节）直接返回：有些实现（如 OpenSSL 的某些封装）已经转过。
    if (der.size() == 64) return der;

    if (der.size() < 8 || static_cast<unsigned char>(der[0]) != 0x30) return {};

    int pos = 1;
    // 长度字段：短格式（<0x80）或长格式（0x81/0x82 + 长度）
    int seqLen = 0;
    const unsigned char lenByte = static_cast<unsigned char>(der[pos]);
    if (lenByte < 0x80) {
        seqLen = lenByte;
        pos += 1;
    } else if (lenByte == 0x81 && der.size() > pos + 1) {
        seqLen = static_cast<unsigned char>(der[pos + 1]);
        pos += 2;
    } else if (lenByte == 0x82 && der.size() > pos + 2) {
        seqLen = (static_cast<unsigned char>(der[pos + 1]) << 8)
                 | static_cast<unsigned char>(der[pos + 2]);
        pos += 3;
    } else {
        return {};
    }
    Q_UNUSED(seqLen);

    const auto readInt = [&der, &pos](QByteArray* out) -> bool {
        if (pos + 2 > der.size()) return false;
        if (static_cast<unsigned char>(der[pos]) != 0x02) return false;
        const int n = static_cast<unsigned char>(der[pos + 1]);
        pos += 2;
        if (pos + n > der.size()) return false;
        QByteArray v = der.mid(pos, n);
        pos += n;

        // 去掉 DER 为「正整数」加的前导 0x00
        while (v.size() > 32 && static_cast<unsigned char>(v[0]) == 0x00) v.remove(0, 1);
        // 不足 32 字节 → 左边补 0
        if (v.size() < 32) v = QByteArray(32 - v.size(), '\0') + v;
        if (v.size() != 32) return false;
        *out = v;
        return true;
    };

    QByteArray r, sVal;
    if (!readInt(&r)) return {};
    if (!readInt(&sVal)) return {};

    QByteArray out;
    out.reserve(64);
    out.append(r);
    out.append(sVal);
    return out;
}

/// ECDSA-P256-SHA256 验签。签名字节按 P1363（r||s 各 32 字节）；
/// 传 DER 进来也可以，内部会转（见 derToP1363 的注释）。
bool verifyEcdsaP256(const QByteArray& payload, const QByteArray& signature,
                     const QByteArray& publicKeyBlob)
{
    AlgHandle alg;
    if (!alg.open(BCRYPT_ECDSA_P256_ALGORITHM)) return false;

    KeyHandle key;
    if (!NT_SUCCESS(BCryptImportKeyPair(alg.get(), nullptr, BCRYPT_ECCPUBLIC_BLOB,
                                        key.addr(),
                                        reinterpret_cast<PUCHAR>(const_cast<char*>(publicKeyBlob.constData())),
                                        static_cast<ULONG>(publicKeyBlob.size()), 0))) {
        return false;
    }

    // 先算 payload 的 SHA-256（BCrypt 的 ECDSA 验签接口要的是「已算好的哈希」）。
    const QByteArray digest = kc::crypto::sha256(payload);
    if (digest.size() != 32) return false;

    // 🔴 DER → P1363 必须先转，否则 CNG 验不通 Node 的签名（实测）。
    const QByteArray p1363 = derToP1363(signature);
    if (p1363.size() != 64) return false;

    const NTSTATUS st = BCryptVerifySignature(
        key.get(), nullptr,
        reinterpret_cast<PUCHAR>(const_cast<char*>(digest.constData())),
        static_cast<ULONG>(digest.size()),
        reinterpret_cast<PUCHAR>(const_cast<char*>(p1363.constData())),
        static_cast<ULONG>(p1363.size()), 0);
    return NT_SUCCESS(st);
}

} // namespace

const char* updatePublicKeyBase64()
{
    return kUpdatePublicKeyB64;
}

QString fileSha256(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();

    // 分块读：更新包可能几十 MB，整块读进内存没必要。
    // 🔴 不能直接 f.readAll() —— 大文件会瞬间吃掉几十 MB 堆。
    constexpr qint64 kChunk = 1 << 20;   // 1 MiB
    QByteArray all;
    all.reserve(static_cast<int>(qMin<qint64>(f.size(), 64LL << 20)));
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(kChunk);
        if (chunk.isEmpty()) break;
        all.append(chunk);
    }
    f.close();
    if (all.isEmpty() && f.size() > 0) return QString();

    return QString::fromLatin1(kc::crypto::sha256(all).toHex());
}

VerifyResult verifyPackage(const QString& packagePath,
                           const QString& expectedSha256,
                           const QString& signatureBase64,
                           const QByteArray& signedPayload)
{
    VerifyResult r;

    // ① 公钥没配 → 直接失败。**不能**放行（fail-closed）。
    const QString pubB64 = QString::fromLatin1(updatePublicKeyBase64()).trimmed();
    if (pubB64.isEmpty()) {
        r.error = QStringLiteral("未配置更新签名公钥，无法校验更新包（请联系开发者）");
        return r;
    }

    if (expectedSha256.trimmed().isEmpty()) {
        r.error = QStringLiteral("服务器未下发文件哈希，无法校验");
        return r;
    }

    // ② SHA-256 完整性
    const QString actual = fileSha256(packagePath);
    if (actual.isEmpty()) {
        r.error = QStringLiteral("无法读取下载的文件");
        return r;
    }
    r.hashOk = (actual.compare(expectedSha256.trimmed(), Qt::CaseInsensitive) == 0);
    if (!r.hashOk) {
        // 哈希不对就**不再验签** —— 文件已经不可信，省一次开销。
        r.error = QStringLiteral("文件校验失败：哈希不匹配（可能下载不完整或被篡改）");
        return r;
    }

    // ③ ECDSA 真实性
    if (signatureBase64.trimmed().isEmpty()) {
        r.error = QStringLiteral("服务器未下发签名，无法确认更新包来源");
        return r;
    }
    const QByteArray point = QByteArray::fromBase64(pubB64.toLatin1());
    const QByteArray blob = makeP256PublicBlob(point);
    if (blob.isEmpty()) {
        r.error = QStringLiteral("内置签名公钥格式异常（应为 65 字节未压缩点）");
        return r;
    }
    const QByteArray sig = QByteArray::fromBase64(signatureBase64.trimmed().toLatin1());
    if (sig.isEmpty()) {
        r.error = QStringLiteral("签名数据格式异常");
        return r;
    }

    r.sigOk = verifyEcdsaP256(signedPayload, sig, blob);
    if (!r.sigOk) {
        r.error = QStringLiteral("签名校验失败：更新包来源不可信");
        return r;
    }

    r.ok = true;
    return r;
}

QByteArray PackageManifest::signedPayload() const
{
    // 字段顺序固定，换行分隔 —— 与服务器签名脚本必须逐字一致。
    return (version + QLatin1Char('\n') + file + QLatin1Char('\n') + sha256).toUtf8();
}

bool verifySignatureOnly(const QByteArray& signedPayload, const QString& signatureBase64)
{
    const QString pubB64 = QString::fromLatin1(updatePublicKeyBase64()).trimmed();
    if (pubB64.isEmpty() || signatureBase64.trimmed().isEmpty()) return false;

    const QByteArray point = QByteArray::fromBase64(pubB64.toLatin1());
    const QByteArray blob = makeP256PublicBlob(point);
    if (blob.isEmpty()) return false;

    const QByteArray sig = QByteArray::fromBase64(signatureBase64.trimmed().toLatin1());
    if (sig.isEmpty()) return false;

    return verifyEcdsaP256(signedPayload, sig, blob);
}

bool parseManifest(const QByteArray& json, PackageManifest* out, QString* error)
{
    if (!out) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("清单不是合法 JSON");
        return false;
    }
    const QJsonObject o = doc.object();
    PackageManifest m;
    m.version = o.value(QStringLiteral("version")).toString();
    m.file = o.value(QStringLiteral("file")).toString();
    m.sha256 = o.value(QStringLiteral("sha256")).toString();
    m.signatureBase64 = o.value(QStringLiteral("sig")).toString();

    // 四个字段缺一不可 —— 缺 sha256 或 sig 就意味着「服务器没给校验依据」，
    // 这种情况必须报错而不是带着不完整的信息往下走。
    if (m.version.isEmpty() || m.file.isEmpty() || m.sha256.isEmpty()
        || m.signatureBase64.isEmpty()) {
        if (error) *error = QStringLiteral("更新清单缺少必要字段");
        return false;
    }
    *out = m;
    return true;
}

} // namespace kc::update
