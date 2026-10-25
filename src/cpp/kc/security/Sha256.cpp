#include "kc/security/Sha256.h"

#include <QtGlobal>

#include <utility>

#ifdef Q_OS_WIN
#  include <windows.h>
#  include <bcrypt.h>          // 需要链接 bcrypt（见 CMakeLists 的 WIN32 链接段）
#else
#  error "kc::crypto::Sha256 目前只实现了 Windows CNG 后端（本工程只出 Windows 产物）"
#endif

namespace kc::crypto {
namespace {

#ifdef Q_OS_WIN

constexpr ULONG kSha256Len = 32;

/// BCryptAlgHandle 的 RAII 包装：算法提供者句柄。
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

/// 一个可增量喂数据的 CNG 哈希会话。
/// hmac=true 时算法句柄必须带 BCRYPT_ALG_HANDLE_HMAC_FLAG，且 key 即 HMAC 密钥。
class HashSession
{
public:
    HashSession(BCRYPT_ALG_HANDLE alg, bool hmac, const QByteArray& key)
    {
        ULONG objLen = 0;
        ULONG cb = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0) != 0)
            return;
        m_object.resize(static_cast<int>(objLen));

        // 🔴 pbSecret 的规则（CNG 文档）：
        //    非 HMAC → 必须传 NULL + 0。传非空指针会被判 STATUS_INVALID_PARAMETER，
        //    BCryptCreateHash 直接失败 → 摘要恒为空、比对恒不等。
        //    （2026-10-15 踩过：一开始「保险地」永远传非空指针，结果普通 sha256()
        //      全挂；而 RFC4231 向量只覆盖 HMAC 路径，把这个洞盖住了。）
        //    HMAC → 传密钥；空密钥等价于 64 字节全零密钥（RFC 2104 补零语义），
        //    这里显式补零，因为 cbSecret=0 在 HMAC 模式下也会被判参数非法。
        PUCHAR secret = nullptr;
        ULONG secretLen = 0;
        if (hmac) {
            if (key.isEmpty()) {
                m_paddedKey = QByteArray(64, '\0');
                secret = reinterpret_cast<PUCHAR>(m_paddedKey.data());
                secretLen = 64;
            } else {
                secret = reinterpret_cast<PUCHAR>(const_cast<char*>(key.constData()));
                secretLen = static_cast<ULONG>(key.size());
            }
        }

        if (BCryptCreateHash(alg, &m_handle,
                             reinterpret_cast<PUCHAR>(m_object.data()), objLen,
                             secret, secretLen, 0) != 0) {
            m_handle = nullptr;
        }
    }

    ~HashSession()
    {
        if (m_handle) BCryptDestroyHash(m_handle);
    }

    HashSession(const HashSession&) = delete;
    HashSession& operator=(const HashSession&) = delete;

    bool isValid() const { return m_handle != nullptr; }

    void add(const QByteArray& data)
    {
        if (!m_handle || data.isEmpty()) return;
        BCryptHashData(m_handle,
                       reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())),
                       static_cast<ULONG>(data.size()), 0);
    }

    QByteArray finish()
    {
        if (!m_handle) return {};
        QByteArray out(static_cast<int>(kSha256Len), '\0');
        if (BCryptFinishHash(m_handle, reinterpret_cast<PUCHAR>(out.data()), kSha256Len, 0) != 0)
            return {};
        return out;
    }

private:
    BCRYPT_HASH_HANDLE m_handle = nullptr;
    QByteArray m_object;      // 哈希上下文缓冲区，生命周期必须不短于 m_handle
    QByteArray m_paddedKey;   // 仅 HMAC + 空密钥时用到（显式补零），同上
};

/// 打开 SHA-256 算法提供者；hmac 为真时用 HMAC 模式。
bool openSha256(AlgHandle& alg, bool hmac)
{
    return BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA256_ALGORITHM, nullptr,
                                       hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) == 0;
}

#endif // Q_OS_WIN

} // namespace

QByteArray sha256(const QByteArray& data)
{
#ifdef Q_OS_WIN
    AlgHandle alg;
    if (!openSha256(alg, /*hmac=*/false)) return {};
    HashSession s(alg.h, /*hmac=*/false, QByteArray());
    if (!s.isValid()) return {};
    s.add(data);
    return s.finish();
#else
    Q_UNUSED(data);
    return {};
#endif
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data)
{
#ifdef Q_OS_WIN
    AlgHandle alg;
    if (!openSha256(alg, /*hmac=*/true)) return {};
    HashSession s(alg.h, /*hmac=*/true, key);
    if (!s.isValid()) return {};
    s.add(data);
    return s.finish();
#else
    Q_UNUSED(key);
    Q_UNUSED(data);
    return {};
#endif
}

} // namespace kc::crypto
