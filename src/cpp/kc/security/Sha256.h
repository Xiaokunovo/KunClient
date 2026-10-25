#pragma once

#include <QByteArray>

// ─────────────────────────────────────────────────────────────────────────────
//  kc::crypto — SHA-256 / HMAC-SHA256（Windows CNG 实现）
//
//  🔴 为什么不用 QCryptographicHash：
//     MSYS2 的 qt6-static 是**带 OpenSSL 编译**的（QtCore/qconfig.h 里
//     `QT_FEATURE_openssl 1`、`QT_FEATURE_openssl_linked 1`、`QT_LINKED_OPENSSL`），
//     因此 QCryptographicHash 的实现固定走 EVP_*（链接期就是 EVP_MD_CTX_new）。
//     实测（2026-10-15，ld -Map 全量解析）：仅这一条引用就会让链接器把
//     libcrypto.a 的 8425 个成员拖进来 —— **4.42 MB，占单文件 exe 的 11.3%**。
//     QCryptographicHash 没有运行时逃生口（openssl 后端是编译期定死的），
//     所以要甩掉这 4.4 MB，只能在调用侧换成系统实现。
//
//     CNG（BCrypt）是 Windows 自带实现（Vista+），硬件加速、零第三方依赖，
//     与非 Windows 平台无关（本工程只出 Windows 产物）。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::crypto {

/// SHA-256 摘要（32 字节原始字节，不是 hex）。失败返回空 QByteArray。
QByteArray sha256(const QByteArray& data);

/// HMAC-SHA256（RFC 2104，块长 64）。返回 32 字节原始摘要；失败返回空 QByteArray。
QByteArray hmacSha256(const QByteArray& key, const QByteArray& data);

} // namespace kc::crypto
