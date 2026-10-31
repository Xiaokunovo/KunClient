#include "kc/tools/FlashDryRun.h"

#include <QMap>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace kc::tools {

namespace {

QString humanBytes(quint64 bytes)
{
    const double kb = 1024.0;
    if (bytes < static_cast<quint64>(kb)) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < static_cast<quint64>(kb * kb))
        return QStringLiteral("%1 KB").arg(bytes / kb, 0, 'f', 1);
    if (bytes < static_cast<quint64>(kb * kb * kb))
        return QStringLiteral("%1 MB").arg(bytes / (kb * kb), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (kb * kb * kb), 0, 'f', 2);
}

/// 两个区间是否相交（半开区间 [start, start+count)）。
bool intersects(quint64 aStart, quint64 aCount, quint64 bStart, quint64 bCount)
{
    if (aCount == 0 || bCount == 0) return false;
    const quint64 aEnd = aStart + aCount;
    const quint64 bEnd = bStart + bCount;
    return aStart < bEnd && bStart < aEnd;
}

} // namespace

DryRunResult buildDryRun(const swt::edl::RawProgramPlan& plan,
                         const QStringList& protectNames)
{
    DryRunResult r;

    if (plan.entries.isEmpty()) {
        r.error = QStringLiteral("这份 XML 里没有任何 program 条目，没有可预演的内容");
        return r;
    }

    r.entryCount = static_cast<int>(plan.entries.size());
    r.warnings = plan.warnings;

    // ── ① 逐条摊成扇区区间 ────────────────────────────────────────────────
    QSet<QString> partitions;
    QSet<quint32> luns;
    quint64 totalBytes = 0;

    for (int i = 0; i < plan.entries.size(); ++i) {
        const swt::edl::RawProgramEntry& e = plan.entries.at(i);

        DryRunSegment seg;
        seg.lun = e.lun;
        seg.startSector = e.startSector;
        seg.sectorCount = e.sectorCount;
        seg.label = e.label.trimmed().isEmpty() ? e.filename : e.label;
        seg.filename = e.filename;
        seg.entryIndex = i;
        r.segments.append(seg);

        // 字节数：优先用 XML 声明的扇区数 × 扇区大小；
        // 两者都缺时退化为「文件大小」（sourceSizeAtPreflight 是 Worker 预检快照）。
        quint64 bytes = 0;
        if (e.sectorCount > 0 && e.sectorSize > 0) {
            bytes = e.sectorCount * e.sectorSize;
        } else if (e.sourceSizeAtPreflight > 0) {
            bytes = static_cast<quint64>(e.sourceSizeAtPreflight);
        }
        totalBytes += bytes;

        if (!seg.label.isEmpty()) partitions.insert(seg.label);
        luns.insert(e.lun);

        // 保护分区命中
        for (const QString& guard : protectNames) {
            if (seg.label.compare(guard, Qt::CaseInsensitive) == 0) {
                if (!r.protectedPartitions.contains(seg.label)) {
                    r.protectedPartitions.append(seg.label);
                }
                break;
            }
        }

        // 扇区数/起始扇区没解析出来的条目要提醒 —— 这种条目刷下去行为不确定。
        if (e.sectorCount == 0) {
            r.warnings.append(
                QStringLiteral("%1：扇区数为 0，可能写不到任何内容").arg(seg.label));
        }
        if (!e.startSectorResolved) {
            r.warnings.append(
                QStringLiteral("%1：起始扇区是表达式且未解析出来").arg(seg.label));
        }
    }

    r.totalWriteBytes = totalBytes;
    r.partitionCount = partitions.size();
    r.lunCount = luns.size();

    // ── ② 找重叠：同 LUN 上两两比较 ───────────────────────────────────────
    // 🔴 复杂度是 O(n²)，但条目数通常几十条（最多几百），可以接受；
    //    用排序 + 扫描线当然更快，但那要处理「一条覆盖多条」的合并，
    //    而这里要的是「哪两条撞了」的**明细**，两两比更直接。
    for (int i = 0; i < r.segments.size(); ++i) {
        for (int j = i + 1; j < r.segments.size(); ++j) {
            const DryRunSegment& a = r.segments.at(i);
            const DryRunSegment& b = r.segments.at(j);
            if (a.lun != b.lun) continue;
            if (!intersects(a.startSector, a.sectorCount, b.startSector, b.sectorCount)) {
                continue;
            }

            DryRunOverlap o;
            o.firstIndex = a.entryIndex;
            o.secondIndex = b.entryIndex;
            o.lun = a.lun;
            o.firstLabel = a.label;
            o.secondLabel = b.label;
            // 记录相交的那一段（从较晚的开始，到较早的结束）
            const quint64 from = std::max(a.startSector, b.startSector);
            const quint64 to = std::min(a.startSector + a.sectorCount,
                                        b.startSector + b.sectorCount);
            o.startSector = from;
            o.sectorCount = (to > from) ? (to - from) : 0;
            r.overlaps.append(o);
        }
    }

    if (!r.overlaps.isEmpty()) {
        r.warnings.append(
            QStringLiteral("检测到 %1 处扇区重叠：后写的会覆盖先写的，"
                           "请确认这是有意为之").arg(r.overlaps.size()));
    }

    r.ok = true;
    return r;
}

QString describeDryRun(const DryRunResult& result)
{
    if (!result.ok) {
        return result.error.isEmpty() ? QStringLiteral("预演失败") : result.error;
    }

    QStringList lines;
    lines << QStringLiteral("这次刷写会写入 %1 个条目，涉及 %2 个分区、%3 个 LUN，"
                            "合计约 %4。")
                 .arg(result.entryCount)
                 .arg(result.partitionCount)
                 .arg(result.lunCount)
                 .arg(humanBytes(result.totalWriteBytes));

    if (!result.protectedPartitions.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("【注意】包含 %1 个保护分区：%2")
                     .arg(result.protectedPartitions.size())
                     .arg(result.protectedPartitions.join(QStringLiteral("、")));
        lines << QStringLiteral("刷这些分区需要额外确认，写坏了设备可能无法启动。");
    }

    if (!result.overlaps.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("【警告】%1 处扇区重叠：").arg(result.overlaps.size());
        int shown = 0;
        for (const DryRunOverlap& o : result.overlaps) {
            if (shown++ >= 5) {
                lines << QStringLiteral("  … 另有 %1 处")
                             .arg(result.overlaps.size() - 5);
                break;
            }
            lines << QStringLiteral("  · LUN %1 扇区 %2（%3 条）：「%4」与「%5」")
                         .arg(o.lun).arg(o.startSector).arg(o.sectorCount)
                         .arg(o.firstLabel, o.secondLabel);
        }
    }

    if (!result.warnings.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("提醒（%1 条）：").arg(result.warnings.size());
        int shown = 0;
        for (const QString& w : result.warnings) {
            if (shown++ >= 5) {
                lines << QStringLiteral("  … 另有 %1 条").arg(result.warnings.size() - 5);
                break;
            }
            lines << QStringLiteral("  · %1").arg(w);
        }
    }

    if (result.overlaps.isEmpty() && result.warnings.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("未发现重叠或异常，可以开始刷写。");
    }

    return lines.join(QLatin1Char('\n'));
}

} // namespace kc::tools
