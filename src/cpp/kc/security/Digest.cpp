#include "kc/security/Digest.h"

#include <QFile>
#include <QtGlobal>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <bcrypt.h>   // 需要链接 bcrypt（见 CMakeLists 的 WIN32 链接段）
#else
#  error "kc::crypto::Digest 目前只实现了 Windows CNG 后端（本工程只出 Windows 产物）"
#endif

namespace kc::crypto {
namespace {

#ifdef Q_OS_WIN

/// 算法提供者句柄的 RAII 包装。
struct AlgHandle
{
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgHandle()
    {
        if (h) BCryptCloseAlgorithmProvider(h, 0);
    }
    AlgHandle() = default;
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;
};

/// 一个增量摘要会话。构造失败时 m_ok 为 false，调用方按「摘要为空」处理。
class DigestSession
{
public:
    explicit DigestSession(HashAlgo algo)
    {
        LPCWSTR algId = (algo == HashAlgo::Md5) ? BCRYPT_MD5_ALGORITHM
                                                : BCRYPT_SHA256_ALGORITHM;
        if (BCryptOpenAlgorithmProvider(&m_alg.h, algId, nullptr, 0) != 0) return;

        ULONG objLen = 0;
        ULONG cb = 0;
        if (BCryptGetProperty(m_alg.h, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0) != 0)
            return;
        m_object.resize(static_cast<int>(objLen));

        // 非 HMAC：pbSecret 必须是 NULL + 0（传非空会被判 INVALID_PARAMETER）。
        if (BCryptCreateHash(m_alg.h, &m_hash, reinterpret_cast<PUCHAR>(m_object.data()),
                             objLen, nullptr, 0, 0) != 0) {
            m_hash = nullptr;
            return;
        }
        m_ok = true;
    }

    ~DigestSession()
    {
        if (m_hash) BCryptDestroyHash(m_hash);
    }
    DigestSession(const DigestSession&) = delete;
    DigestSession& operator=(const DigestSession&) = delete;

    [[nodiscard]] bool ok() const { return m_ok; }

    bool feed(const char* data, int len)
    {
        if (!m_ok) return false;
        if (len <= 0) return true;
        return BCryptHashData(m_hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data)),
                              static_cast<ULONG>(len), 0) == 0;
    }

    /// 结束并输出原始摘要字节；调用后会话作废（不可再 feed）。
    QByteArray finish(int digestLen)
    {
        if (!m_ok) return {};
        QByteArray out(digestLen, Qt::Uninitialized);
        if (BCryptFinishHash(m_hash, reinterpret_cast<PUCHAR>(out.data()),
                             static_cast<ULONG>(digestLen), 0) != 0)
            return {};
        return out;
    }

private:
    AlgHandle m_alg;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    QByteArray m_object;
    bool m_ok = false;
};

#endif // Q_OS_WIN

} // namespace

QString hashAlgoName(HashAlgo algo)
{
    return algo == HashAlgo::Md5 ? QStringLiteral("md5") : QStringLiteral("sha256");
}

int hashAlgoLength(HashAlgo algo)
{
    return algo == HashAlgo::Md5 ? 16 : 32;
}

QByteArray hashBytes(const QByteArray& data, HashAlgo algo)
{
#ifdef Q_OS_WIN
    DigestSession session(algo);
    if (!session.ok()) return {};
    if (!session.feed(data.constData(), data.size())) return {};
    return session.finish(hashAlgoLength(algo));
#else
    Q_UNUSED(data);
    Q_UNUSED(algo);
    return {};
#endif
}

QString hashFile(const QString& path, HashAlgo algo, QString* error,
                 const std::function<void(qint64, qint64)>& progress)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开文件：%1（%2）").arg(path, file.errorString());
        return {};
    }
    const qint64 total = file.size();

#ifdef Q_OS_WIN
    DigestSession session(algo);
    if (!session.ok()) {
        if (error) *error = QStringLiteral("摘要算法初始化失败（CNG）");
        return {};
    }

    constexpr qint64 kChunk = 1024 * 1024;   // 1 MiB
    QByteArray buffer(static_cast<int>(kChunk), Qt::Uninitialized);
    qint64 done = 0;
    while (true) {
        const qint64 read = file.read(buffer.data(), kChunk);
        if (read < 0) {
            if (error) *error = QStringLiteral("读取失败：%1（%2）").arg(path, file.errorString());
            return {};
        }
        if (read == 0) break;
        if (!session.feed(buffer.constData(), static_cast<int>(read))) {
            if (error) *error = QStringLiteral("摘要计算失败（CNG）");
            return {};
        }
        done += read;
        if (progress) progress(done, total);
    }

    const QByteArray digest = session.finish(hashAlgoLength(algo));
    if (digest.isEmpty()) {
        if (error) *error = QStringLiteral("摘要收尾失败（CNG）");
        return {};
    }
    return QString::fromLatin1(digest.toHex());
#else
    Q_UNUSED(error);
    Q_UNUSED(progress);
    Q_UNUSED(total);
    return {};
#endif
}

} // namespace kc::crypto
