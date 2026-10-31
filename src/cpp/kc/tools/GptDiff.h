#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  GptDiff —— 分区表对比（2026-10-31）
//
//  场景：用户手上有一份官方 GPT 和一份别人给的「扩容 / 改分区」GPT，想知道到底
//  动了什么；或者刷机前后各读一次 GPT，确认这次刷写有没有把分区表改坏。手工比对
//  几十上百行分区是不现实的 —— 这一层把两份表压成「新增 / 删除 / 改动」三张清单。
//
//  为什么直接用 core/Types.h 的 swt::edl::PartitionInfo 而不是自造结构：
//    · 那份结构就是 GptParser 的产物，也是 UI / worker / AdbGptReader 一路传下来的
//      类型；再定义一份 GptEntry 只会多一层转换，且两边的字段口径迟早漂移。
//    · 它带 typeGuid / uniqueGuid / attributes，将来要扩「GUID 变了」也不用改签名。
//    本文件因此**只依赖 core/Types.h + QtCore**，不引入任何 storage / protocol 实现
//    （GptParser 是纯解析类，不链进来也能用这个头）。
//
//  分区名的匹配口径：按 name **区分大小写**精确匹配。
//    · GPT 里分区名大小写敏感（"boot" 与 "Boot" 是两个名字），模糊匹配会把
//      真实的改名误报成「没变」；
//    · 同名多份（多 LUN 下偶尔有）时按 LUN 再分一次：先找 (name, lun) 精确的，
//      找不到再退到同名任意 LUN —— 这样「LUN 变了」才能被报成 changed 而不是
//      一增一删。
// ─────────────────────────────────────────────────────────────────────────────

#include "core/Types.h"

#include <QString>
#include <QVector>

namespace kc::tools {

/// 字段级差异：changed 里逐项告诉你「哪个字段变了」。
struct GptFieldChange {
    /// 变了什么。位标志，可以同时有多个（例如起始扇区 + 大小一起变）。
    enum Flag : quint8 {
        StartSector = 0x01,   ///< 起始扇区变了
        SectorCount = 0x02,   ///< 大小（扇区数）变了
        Lun = 0x04,           ///< LUN 变了
        TypeGuid = 0x08,      ///< 类型 GUID 变了（同样是「分区性质」变了，值得报）
    };

    QString name;
    quint8 flags = 0;
    quint64 oldStartSector = 0;
    quint64 newStartSector = 0;
    quint64 oldSectorCount = 0;
    quint64 newSectorCount = 0;
    quint32 oldLun = 0;
    quint32 newLun = 0;

    bool has(Flag flag) const { return (flags & flag) != 0; }
    /// 差异的中文描述，如「起始扇区 2048 → 4096；大小 2048 → 4096 扇区」。
    QString describe() const;
};

/// 对比结果。identical 为真时三张清单都为空。
struct DiffResult {
    QVector<swt::edl::PartitionInfo> added;   ///< 新表有、旧表没有
    QVector<swt::edl::PartitionInfo> removed; ///< 旧表有、新表没有
    QVector<GptFieldChange> changed;          ///< 同名但字段不同
    bool identical = true;
};

/// 对比两份分区表（old = 基线，new = 待比较）。
/// 入参顺序无所谓（不是「谁比谁好」），但 added/removed 的语义是相对 old 的。
/// 空表也合法：old 空 → 全部算 added；new 空 → 全部算 removed。
DiffResult diffGpt(const QVector<swt::edl::PartitionInfo>& oldPartitions,
                   const QVector<swt::edl::PartitionInfo>& newPartitions);

/// 给人看的摘要（每行一条）。identical 时返回「两份分区表完全一致。」。
QString describeDiff(const DiffResult& result);

} // namespace kc::tools
