// 分区表对比的纯逻辑回归（2026-10-31）。
//
// 手工比对几十行 GPT 不现实，所以「改了哪个字段」必须由代码说清楚。断言集中在：
//   · 只增不减 / 只减不增 / 完全一致（identical 不能误报）；
//   · changed 能区分「起始扇区变了 / 大小变了 / LUN 变了」，且能同时报多个；
//   · 大小用 sectorCount（lastLba-firstLba+1），不是直接比 lastLba；
//   · 同名不同 LUN 要报成「LUN 变了」而不是「删一个 + 加一个」。

#include "kc/tools/GptDiff.h"

#include <QString>
#include <QVector>

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool ok, const char* what, const std::string& detail = {})
{
    if (ok) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++g_failures;
        std::cerr << "FAIL " << what << (detail.empty() ? "" : ": " + detail) << '\n';
    }
}

swt::edl::PartitionInfo makePartition(const QString& name, quint64 firstLba, quint64 sectorCount,
                                      quint32 lun = 0)
{
    swt::edl::PartitionInfo p;
    p.name = name;
    p.firstLba = firstLba;
    p.lastLba = firstLba + sectorCount - 1;   // sectorCount() 就是 lastLba-firstLba+1
    p.lun = lun;
    p.typeGuid = QStringLiteral("11111111-2222-3333-4444-555555555555");
    return p;
}

} // namespace

int main()
{
    using namespace kc::tools;

    // ── ① 完全一致：identical=true 且三张清单都空 ────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> a{
            makePartition(QStringLiteral("boot"), 2048, 2048),
            makePartition(QStringLiteral("system"), 4096, 8192),
        };
        // 内容相同但**另一个对象**（不能靠指针判断）。
        const QVector<swt::edl::PartitionInfo> b{
            makePartition(QStringLiteral("boot"), 2048, 2048),
            makePartition(QStringLiteral("system"), 4096, 8192),
        };

        const DiffResult result = diffGpt(a, b);
        expect(result.identical, "identical-same-tables");
        expect(result.added.isEmpty(), "identical-no-added");
        expect(result.removed.isEmpty(), "identical-no-removed");
        expect(result.changed.isEmpty(), "identical-no-changed");
        expect(describeDiff(result).contains(QStringLiteral("完全一致")), "identical-describe");

        // 同一份表跟自己比也必须 identical。
        expect(diffGpt(a, a).identical, "identical-self");
    }

    // ── ② 空输入：两边都空 = 一致；一边空 = 全增 / 全删 ──────────────────
    {
        const DiffResult bothEmpty = diffGpt({}, {});
        expect(bothEmpty.identical, "empty-both-identical");
        expect(bothEmpty.added.isEmpty() && bothEmpty.removed.isEmpty()
                   && bothEmpty.changed.isEmpty(),
               "empty-both-all-lists-empty");

        const QVector<swt::edl::PartitionInfo> some{
            makePartition(QStringLiteral("boot"), 2048, 2048),
            makePartition(QStringLiteral("system"), 4096, 8192),
        };

        // old 空 → 全部新增。
        const DiffResult allAdded = diffGpt({}, some);
        expect(!allAdded.identical, "empty-old-not-identical");
        expect(allAdded.added.size() == 2, "empty-old-all-added",
               std::to_string(allAdded.added.size()));
        expect(allAdded.removed.isEmpty(), "empty-old-none-removed");
        expect(allAdded.changed.isEmpty(), "empty-old-none-changed");

        // new 空 → 全部删除。
        const DiffResult allRemoved = diffGpt(some, {});
        expect(!allRemoved.identical, "empty-new-not-identical");
        expect(allRemoved.removed.size() == 2, "empty-new-all-removed",
               std::to_string(allRemoved.removed.size()));
        expect(allRemoved.added.isEmpty(), "empty-new-none-added");
    }

    // ── ③ 只增不减 ──────────────────────────────────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),
            makePartition(QStringLiteral("vendor"), 4096, 4096),
            makePartition(QStringLiteral("product"), 8192, 4096),
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(!result.identical, "add-only-not-identical");
        expect(result.added.size() == 2, "add-only-count", std::to_string(result.added.size()));
        expect(result.removed.isEmpty(), "add-only-no-removed");
        expect(result.changed.isEmpty(), "add-only-no-changed");
        expect(result.added.at(0).name == QStringLiteral("vendor"), "add-only-first-name",
               result.added.at(0).name.toStdString());
        expect(result.added.at(1).name == QStringLiteral("product"), "add-only-second-name");

        const QString text = describeDiff(result);
        expect(text.contains(QStringLiteral("新增 2")), "add-only-describe", text.toStdString());
        expect(text.contains(QStringLiteral("vendor")), "add-only-describe-name");
    }

    // ── ④ 只减不增 ──────────────────────────────────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),
            makePartition(QStringLiteral("vendor"), 4096, 4096),
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(!result.identical, "remove-only-not-identical");
        expect(result.removed.size() == 1, "remove-only-count",
               std::to_string(result.removed.size()));
        expect(result.removed.at(0).name == QStringLiteral("vendor"), "remove-only-name");
        expect(result.added.isEmpty(), "remove-only-no-added");
        expect(result.changed.isEmpty(), "remove-only-no-changed");
    }

    // ── ⑤ 起始扇区变了（大小不变） ──────────────────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("boot"), 4096, 2048),   // 起点后移，长度不变
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(!result.identical, "start-changed-not-identical");
        expect(result.changed.size() == 1, "start-changed-count",
               std::to_string(result.changed.size()));
        if (result.changed.size() == 1) {
            const GptFieldChange& c = result.changed.at(0);
            expect(c.name == QStringLiteral("boot"), "start-changed-name");
            expect(c.has(GptFieldChange::StartSector), "start-changed-flag");
            expect(!c.has(GptFieldChange::SectorCount), "start-changed-not-size");
            expect(!c.has(GptFieldChange::Lun), "start-changed-not-lun");
            expect(c.oldStartSector == 2048 && c.newStartSector == 4096, "start-changed-values",
                   std::to_string(c.oldStartSector) + "->" + std::to_string(c.newStartSector));
            expect(c.describe().contains(QStringLiteral("起始扇区")), "start-changed-describe",
                   c.describe().toStdString());
        }
        // 只是起点挪了、长度没变 → 不能报成「一增一删」。
        expect(result.added.isEmpty() && result.removed.isEmpty(),
               "start-changed-not-add-remove");
    }

    // ── ⑥ 大小变了（起始不变） ──────────────────────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("system"), 4096, 8192),
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("system"), 4096, 16384),   // 扩容一倍
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(result.changed.size() == 1, "size-changed-count",
               std::to_string(result.changed.size()));
        if (result.changed.size() == 1) {
            const GptFieldChange& c = result.changed.at(0);
            expect(c.has(GptFieldChange::SectorCount), "size-changed-flag");
            expect(!c.has(GptFieldChange::StartSector), "size-changed-not-start");
            expect(c.oldSectorCount == 8192 && c.newSectorCount == 16384, "size-changed-values",
                   std::to_string(c.oldSectorCount) + "->" + std::to_string(c.newSectorCount));
            expect(c.describe().contains(QStringLiteral("大小")), "size-changed-describe");
        }
    }

    // ── ⑦ LUN 变了：要报成 changed，不是「删一个 + 加一个」 ──────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("userdata"), 2048, 65536, /*lun=*/0),
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("userdata"), 2048, 65536, /*lun=*/4),
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(result.added.isEmpty(), "lun-changed-no-added",
               std::to_string(result.added.size()));
        expect(result.removed.isEmpty(), "lun-changed-no-removed",
               std::to_string(result.removed.size()));
        expect(result.changed.size() == 1, "lun-changed-count",
               std::to_string(result.changed.size()));
        if (result.changed.size() == 1) {
            const GptFieldChange& c = result.changed.at(0);
            expect(c.has(GptFieldChange::Lun), "lun-changed-flag");
            expect(c.oldLun == 0 && c.newLun == 4, "lun-changed-values",
                   std::to_string(c.oldLun) + "->" + std::to_string(c.newLun));
            expect(c.describe().contains(QStringLiteral("LUN")), "lun-changed-describe");
        }
    }

    // ── ⑧ 多字段同时变：一个 change 报多个 flag ─────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("super"), 2048, 8192, /*lun=*/0),
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("super"), 4096, 16384, /*lun=*/2),
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(result.changed.size() == 1, "multi-changed-count",
               std::to_string(result.changed.size()));
        if (result.changed.size() == 1) {
            const GptFieldChange& c = result.changed.at(0);
            expect(c.has(GptFieldChange::StartSector), "multi-flag-start");
            expect(c.has(GptFieldChange::SectorCount), "multi-flag-size");
            expect(c.has(GptFieldChange::Lun), "multi-flag-lun");
            const QString text = c.describe();
            expect(text.contains(QStringLiteral("起始扇区")) && text.contains(QStringLiteral("大小"))
                       && text.contains(QStringLiteral("LUN")),
                   "multi-flag-describe", text.toStdString());
        }
    }

    // ── ⑨ 混合场景：增 + 删 + 改 同时存在 ───────────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),      // 不变
            makePartition(QStringLiteral("oldonly"), 4096, 1024),   // 删
            makePartition(QStringLiteral("system"), 8192, 8192),    // 改（大小）
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),      // 不变
            makePartition(QStringLiteral("system"), 8192, 16384),   // 改
            makePartition(QStringLiteral("newonly"), 24576, 1024),  // 增
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(result.added.size() == 1, "mixed-added", std::to_string(result.added.size()));
        expect(result.removed.size() == 1, "mixed-removed", std::to_string(result.removed.size()));
        expect(result.changed.size() == 1, "mixed-changed", std::to_string(result.changed.size()));
        expect(result.added.at(0).name == QStringLiteral("newonly"), "mixed-added-name");
        expect(result.removed.at(0).name == QStringLiteral("oldonly"), "mixed-removed-name");
        expect(result.changed.at(0).name == QStringLiteral("system"), "mixed-changed-name");

        const QString text = describeDiff(result);
        expect(text.contains(QStringLiteral("新增 1")) && text.contains(QStringLiteral("删除 1"))
                   && text.contains(QStringLiteral("改动 1")),
               "mixed-describe-counts", text.toStdString());
    }

    // ── ⑩ 分区名大小写敏感：boot 与 Boot 是两个名字 ─────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("boot"), 2048, 2048),
        };
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("Boot"), 2048, 2048),
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        // 大小写不同 = 改名 = 一增一删（不是「没变」）。
        expect(result.added.size() == 1, "case-sensitive-added",
               std::to_string(result.added.size()));
        expect(result.removed.size() == 1, "case-sensitive-removed",
               std::to_string(result.removed.size()));
        expect(result.changed.isEmpty(), "case-sensitive-no-changed");
    }

    // ── ⑪ 同名多 LUN：按 (name, lun) 精确配对 ───────────────────────────
    {
        const QVector<swt::edl::PartitionInfo> oldTable{
            makePartition(QStringLiteral("userdata"), 2048, 4096, /*lun=*/0),
            makePartition(QStringLiteral("userdata"), 2048, 8192, /*lun=*/1),
        };
        // 只把 LUN1 的那份扩容，LUN0 不动。
        const QVector<swt::edl::PartitionInfo> newTable{
            makePartition(QStringLiteral("userdata"), 2048, 4096, /*lun=*/0),
            makePartition(QStringLiteral("userdata"), 2048, 16384, /*lun=*/1),
        };

        const DiffResult result = diffGpt(oldTable, newTable);
        expect(result.added.isEmpty() && result.removed.isEmpty(),
               "multi-lun-no-add-remove");
        expect(result.changed.size() == 1, "multi-lun-changed-count",
               std::to_string(result.changed.size()));
        if (result.changed.size() == 1) {
            expect(result.changed.at(0).has(GptFieldChange::SectorCount),
                   "multi-lun-changed-size");
            expect(!result.changed.at(0).has(GptFieldChange::Lun), "multi-lun-lun-same");
        }
    }

    // ── ⑫ describeDiff 的拒绝/正常分支都要有内容 ────────────────────────
    {
        expect(!describeDiff(DiffResult()).isEmpty(), "describe-empty-result-not-blank");
        expect(describeDiff(DiffResult()).contains(QStringLiteral("完全一致")),
               "describe-default-identical");
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " FAILURE(S)\n";
    return 1;
}
