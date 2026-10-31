#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ImageProbe —— 镜像格式识别（按魔数，不看扩展名）
//
//  为什么需要：用户从各种来源拿到 .img，文件名和内容经常对不上 ——
//  有的是 Android sparse，有的是 raw ext4，有的其实是 boot image，
//  还有的干脆是全 0 的空文件（做擦除标记用的）。
//  选错类型刷进去就是砖。
//
//  🔴 **只读文件头几十字节**，不整块读进内存：
//    系统镜像动辄几个 GB，为了判断格式去读全文件是不可接受的。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>

namespace kc::tools {

/// 识别出的镜像类型。
enum class ImageKind {
    Unknown,         ///< 认不出
    AndroidSparse,   ///< Android sparse image（魔数 0x3AFF26ED）
    AndroidBoot,     ///< Android boot image（"ANDROID!"）
    Ext4,            ///< ext4 文件系统（偏移 0x438 处 0xEF53）
    AllZero,         ///< 全 0（可能是擦除标记）
    Plain,           ///< 普通数据（没有可识别魔数，但也不是全 0）
};

/// 识别结果。
struct ImageProbeResult {
    ImageKind kind = ImageKind::Unknown;
    QString kindName;      ///< 中文名（界面直接用）
    QString evidence;      ///< 依据说明（"文件头 3A FF 26 ED"），便于排查
    qint64 sizeBytes = 0;
    bool ok = false;       ///< 文件读得了
    QString error;         ///< ok=false 时的原因
};

/// 探测一个镜像文件。不存在的文件返回 ok=false 且 error 说明原因。
ImageProbeResult probeImage(const QString& path);

/// 类型 → 中文名。
QString imageKindName(ImageKind kind);

/// 按文件名 + 类型猜分区名（猜不出返回空串）。
/// 只做**提示**，不替用户决定 —— 猜错了刷进去是灾难，所以界面上要显示成
/// 「建议：boot」而不是直接填进去。
QString suggestPartitionName(ImageKind kind, const QString& fileName);

} // namespace kc::tools
