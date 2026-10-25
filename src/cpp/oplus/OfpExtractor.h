#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  OfpExtractor — OPPO / realme OFP 固件解包（纯逻辑层，无 UI 依赖）
//
//  移植自 YukiClient `Core/Violet/OppoOfpExtractor.cs`（665 行）。三种形态靠文件头识别：
//
//   ① 加密 ZIP：开头是 'P''K' → 整个文件就是一个带口令的 zip。
//      参考实现用 SharpCompress + 固定口令解压；C++ 侧不内置 zip 解密，改调外部
//      7-Zip（程序目录 / Program Files / PATH 里找 7z.exe），找不到就明确报错。
//   ② 联发科 OFP：[镜像数据区][文件表 entryCount*0x60][文件头 0x6C] 三段贴在文件尾。
//      文件头/文件表都做过一层字节混淆：x → (高低半字节互换) 再 XOR "geyixue"。
//      🔴 该运算**不是自反的**（XOR 的键两个半字节都不为 0，与换半字节不交换），
//         所以这里只提供**解码方向** mtkShuffleDecode，别当对称加解密用。
//      识别：用候选密钥表逐条 AES-128-CFB 解开头 16 字节，解出以 "MMM" 开头的即命中。
//   ③ 高通 OFP：[镜像数据区][元数据页 pageSize][xml 数据]。
//      页大小 0x200 或 0x1000：从 `文件长度 - pageSize + 0x10` 处 u32 == 0x7CEF 判定。
//      末尾页 +0x14 处 u32 = xml 起始页号（× pageSize），u32 = xml 长度
//      （长度不合理时用 文件长度-pageSize-偏移-0x57 兜底）。
//      ProFile.xml 本身也按 AES-128-CFB 加密，解出来从 "<?xml" 到最后一个 '>' 是正文。
//      加密范围按 xml 里的类别决定：整段不加密（Firmware/DigestsToSign/ChainedTableOfDigests）、
//      整段加密（Sahara）、或只加密开头 0x40000 字节（其余类别）。
//
//  密钥：内置若干组三元组 mask / 混淆后的 key / 混淆后的 iv。
//    key = mask 与串逐字节 (XOR 后换半字节) → MD5 → 十六进制小写前 16 字符当 ASCII 密钥。
//    少数组只有 2 个串，那就是直接的 ASCII key/iv。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace swt::edl {

struct OfpExtractResult {
    enum class Format {
        Unknown,
        EncryptedZip,
        Mtk,
        Qualcomm,
    };

    Format format = Format::Unknown;
    int fileCount = 0;
    qint64 totalBytes = 0;
    QStringList files;
    QString profileXmlPath;   ///< 仅高通形态：解出来的 ProFile.xml 路径

    static QString formatName(Format format);
};

class OfpExtractor final {
public:
    /// 进度回调（percent 0~100 + 一句中文说明）。
    using ProgressFn = std::function<void(int percent, const QString& message)>;
    using LogFn = std::function<void(const QString&)>;
    /// 取消判定：返回 true 即中止（已完成的部分保留）。
    using CancelFn = std::function<bool()>;

    /// 解包到 outputDirectory（不存在会自动创建）。
    static bool extract(const QString& sourcePath, const QString& outputDirectory,
                        OfpExtractResult* result, QString* error,
                        const ProgressFn& progress = {}, const LogFn& log = {},
                        const CancelFn& cancel = {});

    // ── 纯函数（单测 / 排障用）────────────────────────────────────────────

    /// mask + 混淆串 → 16 字节 ASCII 密钥（MD5 十六进制前 16 字符）。
    static QByteArray deriveKeyMaterial(const QByteArray& mask, const QByteArray& encoded);

    /// 文件头/文件表的混淆**解码**（x = key[i%7] ^ swapNibbles(x)）。
    /// 🔴 不是自反的，不要拿它当加密用。
    static void mtkShuffleDecode(const QByteArray& key, QByteArray* data);

    /// 识别页大小（0x200 / 0x1000）；认不出返回 0。
    static int detectQualcommPageSize(const QByteArray& file);

private:
    OfpExtractor() = delete;
};

} // namespace swt::edl
