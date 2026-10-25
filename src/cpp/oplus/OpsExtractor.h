#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  OpsExtractor — 一加 / OPPO OPS 固件解包（纯逻辑层，无 UI 依赖）
//
//  移植自 YukiClient `Core/Violet/OnePlusOpsExtractor.cs`（797 行）。
//
//  文件布局（由文件尾向前看）：
//      [ 头部/其它数据 ][ 分区镜像若干 ][ settings.xml 密文(补齐到 512 字节) ][ 512 字节页脚 ]
//    · 页脚固定 0x200 字节；
//    · 页脚偏移 0x18 处 4 字节小端 = settings.xml 的明文长度；
//    · 页脚之前是补齐到 512 字节边界的 settings.xml 密文。
//
//  解密方式（MBox 4 / 5 / 6 三套「邮箱常量」一一尝试）：
//    · 滚动密钥初值固定为 128 位常量；
//    · 每处理 16 字节，先把滚动密钥做一次类 AES 的密钥扩展（AES S 盒 + xtime 组成的
//      T 表，轮数由常量表第 0x3C 字节决定 → MBox 4/5/6 都是 8 轮）；
//    · 明文 4 个 32 位字 = 密钥 4 个 32 位字 逐字 XOR 密文对应字；
//    · 随后密钥 4 个字被当次密文的 4 个字替换（密文反馈，类似 CBC 链式）。
//    · 版本判定：依次用 5 / 6 / 4 三套常量解 settings.xml，结果必须同时满足
//      "含 XML 声明片段" 且 "能被 XML 解析器接受" 才算命中。
//
//  settings.xml 的镜像表：
//    · <SAHARA> 段下的 <File>        → 镜像体同样加密；
//    · <UFS_PROVISION> 段下的 <File> → 明文镜像；
//    · 段名含 "Program" 的段         → 段内所有带 Path / filename 的节点，明文镜像；
//    · FileOffsetInSrc 单位 = 512 字节扇区；SizeInByteInSrc 单位 = 字节。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace swt::edl {

struct OpsExtractResult {
    int mboxVersion = 0;        ///< 识别出的 MBox 版本（4 / 5 / 6）
    QString settingsPath;       ///< 解密后写出的 settings.xml 路径
    int fileCount = 0;
    qint64 totalBytes = 0;      ///< 抽出的镜像总字节数（不含 settings.xml）
    QStringList files;          ///< 相对路径（与 settings.xml 里的 Path 一致）
};

class OpsExtractor final {
public:
    /// OPS 内所有偏移/长度都以 512 字节扇区为基准。
    static constexpr int SectorSize = 0x200;

    using ProgressFn = std::function<void(int percent, const QString& message)>;
    using LogFn = std::function<void(const QString&)>;
    using CancelFn = std::function<bool()>;

    static bool extract(const QString& sourcePath, const QString& outputDirectory,
                        OpsExtractResult* result, QString* error,
                        const ProgressFn& progress = {}, const LogFn& log = {},
                        const CancelFn& cancel = {});

    // ── 自定义流式加/解密（纯函数）──────────────────────────────────────
    //  解密：明文 = 扩展密钥 XOR 密文，随后密钥 = 密文（密文反馈）。
    //  加密是同一套循环的反向写法（密钥流只由前一块密文决定），单测用它造样本。
    static QByteArray decryptCustom(const QByteArray& cipher, const QByteArray& mbox);
    static QByteArray encryptCustom(const QByteArray& plain, const QByteArray& mbox);

    /// 邮箱常量表（MBox 4 / 5 / 6 各 62 字节）。诊断与单测用；未知版本返回空。
    static QByteArray mboxConstants(int version);

private:
    OpsExtractor() = delete;
};

} // namespace swt::edl
