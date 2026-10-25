#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  StrObf — 编译期字符串混淆（Obfuscar HideStrings 的 C++ 替代）
//
//  为什么需要它（.NET 时代的红线直接继承）：
//    敏感字面量（域名、端点路径、UA 前缀）若以明文写进代码，
//    `strings KunClient.exe` 一把就能捞出来 —— 逆向者据此直接定位服务端接口。
//    .NET 用 Obfuscar 的 HideStrings 做方法体 ldstr 加密；C++ 没有对应工具，
//    用「constexpr 逐字节 XOR + 运行期解密」达到同等效果：
//      · 二进制里只出现**密文**字节（constexpr 求值发生在编译期）；
//      · 明文只在运行期、在栈上的临时缓冲里拼出来，用完即弃。
//
//  用法：
//      #include "kc/security/StrObf.h"
//      const QString url = KC_STR("https://kunawa.cn", 0x5A);
//                                           └─ 每个调用点换一个 key（0~255）
//
//  红线：
//    · KC_STR 返回 QString（UTF-8 解码），中文/多字节字面量同样适用；
//    · key 是编译期常量，**必须每个敏感字面量一个独立 key**——
//      同 key 同模式会让批量 XOR 还原变得轻而易举；
//    · 这只是提高门槛，不是加密学意义上的安全（与 Obfuscar 同级）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <cstddef>
#include <cstdint>

namespace kc {

template <std::size_t N>
class ObfString {
public:
    // 编译期把 src 逐字节加密存进 m_data。GCC/Clang 在 constexpr 求值后
    // 落进 .rodata 的是密文数组 —— 明文不会出现在字符串表里。
    constexpr ObfString(const char (&src)[N], std::uint8_t key)
        : m_key(key)
    {
        for (std::size_t i = 0; i < N; ++i) {
            m_data[i] = static_cast<char>(src[i] ^ xorByte(i));
        }
    }

    // 运行期解密：栈上临时缓冲拼回明文，再转 QString（UTF-8）。
    QString str() const
    {
        char buf[N];
        for (std::size_t i = 0; i < N; ++i) {
            buf[i] = static_cast<char>(m_data[i] ^ xorByte(i));
        }
        // N 含结尾 '\0'（已被加密），fromUtf8 取 N-1 个有效字节；
        // buf 里最后一位解密回 '\0'，正好做终止符兜底。
        return QString::fromUtf8(buf, static_cast<qsizetype>(N - 1));
    }

private:
    // 位置相关的 XOR 流：key + 位置系数 31，避免同 key 下相同字符得到相同密文。
    constexpr std::uint8_t xorByte(std::size_t i) const
    {
        return static_cast<std::uint8_t>(m_key + static_cast<std::uint8_t>(i) * 31u);
    }

    std::uint8_t m_key = 0;
    char m_data[N] = {};
};

} // namespace kc

// sizeof(lit) 含结尾 '\0'，正好作为数组长度传给模板。
#define KC_STR(lit, key) (::kc::ObfString<sizeof(lit)>(lit, key).str())
