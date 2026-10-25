#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AesCfb — AES-128 单块加密 + CFB128 加/解密（自包含实现）
//
//  为什么不用 Windows CNG（bcrypt）：CNG 的 CFB 在 PaddingMode.None 下只吃
//  16 字节整倍数，而 OFP/OPS 里要解的长度并不总是整块（Sahara 段按原始长度）。
//  参考实现当年也踩过这个坑（先补零到整块再截断）。CFB 是流式模式，自己实现
//  反而更直接、可单测（NIST SP 800-38A 向量见 tests/OplusPackageTest.cpp）。
//
//  🔴 只做 AES-128（密钥 16 字节）。OFP/OPS 的密钥全部是 MD5 十六进制前 16 字符，
//     即 16 字节 ASCII，不存在 192/256 需求。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>

namespace swt::edl {

class AesCfb final {
public:
    static constexpr int BlockSize = 16;

    /// AES-128 单块加密（FIPS-197）。key 必须 16 字节，in/out 各 16 字节。
    static void encryptBlock(const quint8 in[BlockSize], quint8 out[BlockSize],
                             const quint8 key[BlockSize]);

    /// CFB128 解密（整块反馈）。任意长度；不足一块的尾巴按实际长度输出。
    /// key / iv 必须各 16 字节，否则返回空。
    static QByteArray decrypt(const QByteArray& data, const QByteArray& key,
                              const QByteArray& iv);

    /// CFB128 加密（与 decrypt 对称）。测试构造样本、将来重打包都用得上。
    static QByteArray encrypt(const QByteArray& data, const QByteArray& key,
                              const QByteArray& iv);
};

} // namespace swt::edl
