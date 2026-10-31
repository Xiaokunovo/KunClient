#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// ─────────────────────────────────────────────────────────────────────────────
//  固件包完整性预检（选包即校）
//
//  为什么要有这一层：刷机失败最常见的原因不是命令敲错，而是**包本身不完整**
//  —— 下载中断留下半截 .img、搬运时被截断、或者包被人动过。这些在刷写命令
//  下发之前就能看出来，等设备已经进了 fastboot、刷到一半才报错就晚了。
//
//  能力边界（重要）：
//    本工程**没有解压库**（zip / 7z 内部一律不解），所以只做三层能离线做的检查：
//      1) 目录层：文件存在性 / 0 字节 / 关键镜像缺失；
//      2) 声明层：rawprogram XML 里写的 size 与磁盘实际字节数对比（截断检测）；
//      3) 旁挂层：同目录的 .md5 / .sha256 / MD5SUMS / SHA256SUMS 自动比对。
//    压缩包（.zip / .7z / .tgz）只能校验**整个包**的旁挂摘要，包内文件逐个
//    检查不了 —— 这一点必须如实告诉用户（结果是 Warning，不是「通过」）。
//
//  依赖口径：
//    · 校验文件解析复用 kc/tools/FirmwareChecksum（coreutils 文本/二进制两种格式）；
//    · 摘要计算复用 kc/security/Digest（Windows CNG 流式，1 MiB 一块），
//      不走 QCryptographicHash（会把整个 OpenSSL 4.4 MB 拖进单文件产物）。
//    本文件只依赖 QtCore，可以脱离界面直接单测（tests/FirmwarePrecheckTest.cpp）。
//
//  ⚠️ 线程：precheckFirmware 会做磁盘 IO 与摘要计算（大包可能几秒到几十秒），
//     界面必须放到线程池里跑，结果再回 UI 线程；本层不含任何 QtWidgets 依赖。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::tools {

/// 预检结论级别。值越大越严重；PrecheckResult::level 取所有明细里的最高档。
enum class PrecheckLevel {
    Info = 0,     ///< 正常，只是提示
    Warning = 1,  ///< 有风险但可以继续刷（例如没有校验文件）
    Error = 2,    ///< 包有问题，不能刷
};

/// 一条预检明细。
struct PrecheckDetail {
    PrecheckLevel level = PrecheckLevel::Info;
    QString path;     ///< 相关文件 / 目录（可为空，例如「包内缺关键镜像」这类整体结论）
    QString message;  ///< 中文说明（直接可展示，不含 emoji）
};

/// 预检开关。默认值就是界面要用的口径。
struct PrecheckOptions {
    /// 是否真的算摘要比对。默认 true。
    /// 关掉只做静态检查（存在性 / 0 字节 / XML 声明 size / 关键镜像），
    /// 用于超大包或单测提速；关掉时「找到校验文件」会降级成 Info 提示。
    bool verifyChecksums = true;

    /// 关键镜像基名（不带扩展名）。空 = 用内置默认表
    /// （boot / init_boot / vendor_boot / dtbo / recovery / system / super / vbmeta）。
    QStringList keyImages;
};

/// 预检结果。界面只读 summary + level + blocksFlashing 三个字段就够。
struct PrecheckResult {
    bool ok = false;                            ///< 没有任何 Error 级问题
    PrecheckLevel level = PrecheckLevel::Info;  ///< 明细里的最高级别
    QString summary;                            ///< 一行中文结论（状态文字直接用）
    QVector<PrecheckDetail> details;            ///< 全部明细，按发现顺序

    /// true = 禁止开始刷机。目前口径：任何 Error（含校验不一致）都锁。
    bool blocksFlashing = false;
    /// 旁挂校验文件明确对不上（区别于「没有校验文件」）。界面据此标红。
    bool checksumMismatch = false;

    int errorCount = 0;
    int warningCount = 0;
    int imageCount = 0;        ///< 目录里数到的镜像文件数（压缩包恒为 0）
    QString checksumFile;      ///< 实际参与比对的校验文件路径（没有则为空）
};

/// 预检主入口。path 可以是：
///   · 固件目录（含 images / rawprogram XML / 旁挂校验文件都行）；
///   · 压缩包（.zip / .7z / .tgz …，只校验整包旁挂摘要）；
///   · 单个 rawprogram*.xml（按它声明的 size 逐个核对引用的镜像）。
/// 路径不存在 / 为空也会返回结果（ok=false，明细里写明原因），不抛异常。
PrecheckResult precheckFirmware(const QString& path, const PrecheckOptions& options = {});

/// 级别的中文短名（"通过" / "提醒" / "失败"），供界面拼状态文字。
QString precheckLevelText(PrecheckLevel level);

} // namespace kc::tools
