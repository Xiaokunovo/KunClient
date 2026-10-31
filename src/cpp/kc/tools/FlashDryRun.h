#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FlashDryRun —— 刷机计划预演（不碰设备）
//
//  为什么需要：现在点「开始刷写」就真的开始写了。用户看不到「这次会动哪些扇区、
//  哪些分区会被覆盖、有没有重叠」，只能事后从日志里回看。
//
//  预演把这层信息提前摊开：**只读输入、只算不改**。
//    · 每条 program 条目会写哪个 LUN、哪段扇区、多大；
//    · 哪些扇区被**多条**条目覆盖（重叠 = 后写的赢，是常见事故来源）；
//    · 总写入量、涉及分区数；
//    · 哪些分区是「保护分区」（写入需要额外确认）。
//
//  🔴 这一层**不读设备**：输入是已解析好的 RawProgramPlan（XML 侧的信息），
//     所以它可以在没连设备时跑 —— 这正是它的价值：先看清楚再插设备。
// ─────────────────────────────────────────────────────────────────────────────

#include "storage/RawProgramParser.h"

#include <QString>
#include <QVector>

namespace kc::tools {

/// 一条写入目标的扇区区间。
struct DryRunSegment {
    quint32 lun = 0;
    quint64 startSector = 0;
    quint64 sectorCount = 0;
    QString label;        ///< 分区名（XML 的 label）
    QString filename;     ///< 来源镜像文件名
    int entryIndex = -1;  ///< 对应 plan.entries 的下标
};

/// 两条条目重叠。
struct DryRunOverlap {
    int firstIndex = -1;
    int secondIndex = -1;
    quint32 lun = 0;
    quint64 startSector = 0;
    quint64 sectorCount = 0;
    QString firstLabel;
    QString secondLabel;
};

/// 预演结果。
struct DryRunResult {
    bool ok = false;
    QString error;

    QVector<DryRunSegment> segments;
    QVector<DryRunOverlap> overlaps;

    quint64 totalWriteBytes = 0;
    int entryCount = 0;
    int partitionCount = 0;   ///< 去重后的分区数
    int lunCount = 0;         ///< 涉及的 LUN 数
    QStringList protectedPartitions;   ///< 命中保护名单的分区

    QStringList warnings;     ///< 解析期警告 + 预演期警告
};

/// 生成预演。plan 为空 / 没有条目时 ok=false 并说明原因。
/// protectNames: 保护分区名单（不区分大小写）；命中会记进 protectedPartitions。
DryRunResult buildDryRun(const swt::edl::RawProgramPlan& plan,
                         const QStringList& protectNames);

/// 给人看的多行报告（确认弹窗 / 日志用）。
QString describeDryRun(const DryRunResult& result);

} // namespace kc::tools
