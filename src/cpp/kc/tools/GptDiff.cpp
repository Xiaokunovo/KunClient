#include "kc/tools/GptDiff.h"

#include <QStringList>

namespace kc::tools {
namespace {

/// 在 new 表里为 old 里的一个分区找对手：先按 (name, lun) 精确找，再退到同名
/// 任意 LUN。返回下标，找不到 -1。
///
/// 为什么要退一步：LUN 变更是**真实存在**的改动（有人把分区从 LUN0 挪到 LUN4），
/// 如果只认 (name, lun)，这种改动会显示成「删一个 + 加一个」，用户看不出是同一
/// 个分区挪了位置。退一步之后它落进 changed，并由 Lun 标志说明。
int findCounterpart(const QVector<swt::edl::PartitionInfo>& pool,
                    const QVector<bool>& used, const QString& name, quint32 lun)
{
    int sameNameAnyLun = -1;
    for (int i = 0; i < pool.size(); ++i) {
        if (used.at(i)) continue;
        if (pool.at(i).name != name) continue;
        if (pool.at(i).lun == lun) return i;      // 最优：名字与 LUN 都对上
        if (sameNameAnyLun < 0) sameNameAnyLun = i;   // 备选：同名不同 LUN
    }
    return sameNameAnyLun;
}

} // namespace

QString GptFieldChange::describe() const
{
    QStringList parts;
    if (has(StartSector)) {
        parts.push_back(QStringLiteral("起始扇区 %1 → %2").arg(oldStartSector).arg(newStartSector));
    }
    if (has(SectorCount)) {
        parts.push_back(QStringLiteral("大小 %1 → %2 扇区").arg(oldSectorCount).arg(newSectorCount));
    }
    if (has(Lun)) {
        parts.push_back(QStringLiteral("LUN %1 → %2").arg(oldLun).arg(newLun));
    }
    if (has(TypeGuid)) {
        parts.push_back(QStringLiteral("类型 GUID 变了"));
    }
    return parts.join(QStringLiteral("；"));
}

DiffResult diffGpt(const QVector<swt::edl::PartitionInfo>& oldPartitions,
                   const QVector<swt::edl::PartitionInfo>& newPartitions)
{
    DiffResult result;

    QVector<bool> oldMatched(oldPartitions.size(), false);
    QVector<bool> newMatched(newPartitions.size(), false);

    // 以 new 为驱动：每个新分区去旧表里找对手 —— 找到就比字段，找不到就是新增。
    for (int i = 0; i < newPartitions.size(); ++i) {
        const swt::edl::PartitionInfo& fresh = newPartitions.at(i);
        const int oldIndex = findCounterpart(oldPartitions, oldMatched, fresh.name, fresh.lun);
        if (oldIndex < 0) {
            result.added.push_back(fresh);
            newMatched[i] = true;
            continue;
        }

        newMatched[i] = true;
        oldMatched[oldIndex] = true;
        const swt::edl::PartitionInfo& original = oldPartitions.at(oldIndex);

        // 逐字段比。注意大小用 sectorCount()（= lastLba - firstLba + 1），
        // 而不是直接比 lastLba —— 起始扇区一变，lastLba 往往跟着变，但那属于
        // 「起始变了」连带的结果，分开报更清楚。
        GptFieldChange change;
        if (original.firstLba != fresh.firstLba) {
            change.flags |= GptFieldChange::StartSector;
            change.oldStartSector = original.firstLba;
            change.newStartSector = fresh.firstLba;
        }
        if (original.sectorCount() != fresh.sectorCount()) {
            change.flags |= GptFieldChange::SectorCount;
            change.oldSectorCount = original.sectorCount();
            change.newSectorCount = fresh.sectorCount();
        }
        if (original.lun != fresh.lun) {
            change.flags |= GptFieldChange::Lun;
            change.oldLun = original.lun;
            change.newLun = fresh.lun;
        }
        if (!original.typeGuid.isEmpty() && !fresh.typeGuid.isEmpty()
            && original.typeGuid.compare(fresh.typeGuid, Qt::CaseInsensitive) != 0) {
            change.flags |= GptFieldChange::TypeGuid;
        }

        if (change.flags != 0) {
            change.name = fresh.name;
            result.changed.push_back(change);
        }
    }

    // 旧表里没被认领的 = 删除。
    for (int i = 0; i < oldPartitions.size(); ++i) {
        if (!oldMatched.at(i)) result.removed.push_back(oldPartitions.at(i));
    }

    result.identical = result.added.isEmpty() && result.removed.isEmpty()
                       && result.changed.isEmpty();
    return result;
}

QString describeDiff(const DiffResult& result)
{
    if (result.identical) {
        return QStringLiteral("两份分区表完全一致。");
    }

    QStringList lines;
    lines.push_back(QStringLiteral("分区表差异：新增 %1 · 删除 %2 · 改动 %3")
                        .arg(result.added.size())
                        .arg(result.removed.size())
                        .arg(result.changed.size()));

    if (!result.added.isEmpty()) {
        lines.push_back(QString());
        lines.push_back(QStringLiteral("新增分区："));
        for (const swt::edl::PartitionInfo& p : result.added) {
            lines.push_back(QStringLiteral("  + %1（LUN %2 · 起始 %3 · %4 扇区）")
                                .arg(p.name)
                                .arg(p.lun)
                                .arg(p.firstLba)
                                .arg(p.sectorCount()));
        }
    }

    if (!result.removed.isEmpty()) {
        lines.push_back(QString());
        lines.push_back(QStringLiteral("删除分区："));
        for (const swt::edl::PartitionInfo& p : result.removed) {
            lines.push_back(QStringLiteral("  - %1（LUN %2 · 起始 %3 · %4 扇区）")
                                .arg(p.name)
                                .arg(p.lun)
                                .arg(p.firstLba)
                                .arg(p.sectorCount()));
        }
    }

    if (!result.changed.isEmpty()) {
        lines.push_back(QString());
        lines.push_back(QStringLiteral("改动分区："));
        for (const GptFieldChange& c : result.changed) {
            lines.push_back(QStringLiteral("  ~ %1：%2").arg(c.name, c.describe()));
        }
    }

    return lines.join(QLatin1Char('\n'));
}

} // namespace kc::tools
