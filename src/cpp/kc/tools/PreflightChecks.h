#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PreflightChecks —— 刷机前的环境检查（电量 / 磁盘 / 容量）
//
//  这三项都是「不检查就会在刷到一半出事」的：
//    · 电量低 + 没充电 → 刷到一半掉电，是最常见的变砖原因；
//    · 磁盘剩余不足 → 备份/回读写到一半失败，比不备份更糟（用户以为备了）；
//    · 镜像比分区大 → 写不进去，报错在很靠后的位置，前面的分区已经被覆盖了。
//
//  🔴 这一层只做**判断**，不采集数据：电量怎么读、磁盘怎么查由调用方给。
//     这样它能脱离设备和文件系统单测。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QVector>

namespace kc::tools {

/// 检查项的严重程度。
enum class CheckLevel {
    Info,     ///< 正常
    Warning,  ///< 提醒，可以继续
    Error,    ///< 必须解决，否则不该开始
};

/// 一条检查结果。
struct CheckItem {
    CheckLevel level = CheckLevel::Info;
    QString title;    ///< 短标题（"电量" / "磁盘空间"）
    QString detail;   ///< 具体说明（含实际数值）
};

/// 电量检查。
///   percent  : 0~100；<0 表示读不到（不判错，给 Info 说明「无法读取」）
///   charging : 是否在充电
CheckItem checkBattery(int percent, bool charging);

/// 磁盘空间检查。
///   availableBytes : 目标目录所在盘剩余字节
///   requiredBytes  : 预计要写的字节
/// 口径：剩余 < 需求 × 1.5 → Error（写满盘会让程序在中途失败）；
///       剩余 < 需求 × 2   → Warning（留点余量更稳）。
CheckItem checkDiskSpace(qint64 availableBytes, qint64 requiredBytes);

/// 容量检查：镜像比分区大时，写到后面就失败了，而前面的分区已经被覆盖。
CheckItem checkImageSize(qint64 imageBytes, qint64 partitionBytes, const QString& partitionName);

/// 批量汇总：给一组检查项，算最严重的级别 + 一行摘要。
CheckLevel worstLevel(const QVector<CheckItem>& items);
QString summaryLine(const QVector<CheckItem>& items);

/// 级别的中文名。
QString checkLevelName(CheckLevel level);

} // namespace kc::tools
