#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  kc::crypto — 通用摘要（MD5 / SHA-256，Windows CNG 实现）
//
//  为什么单独一份、不复用 Sha256.cpp：那边只暴露「一次性吃完整块数据」的
//  sha256(data)，而校验固件动辄 2~8 GB，必须**流式**喂（一次 1 MiB），
//  否则内存直接爆。这里补一个增量会话 + 文件入口，底层仍是同一个 CNG，
//  不引入 OpenSSL（原因见 Sha256.h 顶部那段：QCryptographicHash 会拖进 4.4 MB）。
//
//  MD5 只为「对齐 TMFTool 的 MD5 校验口径」而存在 —— 它早就不抗碰撞，
//  新代码请优先 SHA-256；这里保留是为了能核对第三方发布的 .md5 校验文件。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::crypto {

enum class HashAlgo {
    Md5,     ///< 16 字节摘要，仅用于核对既有 .md5 校验文件
    Sha256,  ///< 32 字节摘要，默认推荐
};

/// 算法名（小写，"md5" / "sha256"），用于日志与文件名后缀。
QString hashAlgoName(HashAlgo algo);

/// 摘要字节数（MD5=16，SHA-256=32）。
int hashAlgoLength(HashAlgo algo);

/// 一次性摘要（小数据用；固件请走 hashFile）。
QByteArray hashBytes(const QByteArray& data, HashAlgo algo);

/// 文件摘要（流式，1 MiB 一块）。返回**小写十六进制**字符串；
/// 失败返回空串并写 error。progress 可为空（每块回调一次，参数为已处理/总字节）。
QString hashFile(const QString& path, HashAlgo algo, QString* error,
                 const std::function<void(qint64, qint64)>& progress = {});

} // namespace kc::crypto
