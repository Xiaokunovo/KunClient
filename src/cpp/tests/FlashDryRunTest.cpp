// 刷机计划预演（kc/tools/FlashDryRun）的回归测试。
//
// 预演的价值全在「算得准」：
//   · 重叠漏报 → 用户以为没事，实际后写的覆盖先写的；
//   · 字节数算错 → 磁盘空间判断跟着错；
//   · 保护分区漏报 → 该二次确认的没确认。
// 这些都不会崩，只会让人做错决定。

#include "kc/tools/FlashDryRun.h"

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL " << what << '\n';
    }
}

/// 造一条 program 条目。
swt::edl::RawProgramEntry makeEntry(const QString& label, quint32 lun, quint64 start,
                          quint64 count, quint32 sectorSize = 512)
{
    swt::edl::RawProgramEntry e;
    e.label = label;
    e.filename = label + QStringLiteral(".img");
    e.lun = lun;
    e.startSector = start;
    e.sectorCount = count;
    e.sectorSize = sectorSize;
    e.startSectorResolved = true;
    return e;
}

constexpr quint64 kMiB = 1024ULL * 1024ULL;

} // namespace

int main()
{
    using namespace kc::tools;

    // ── ① 空计划：要明确报错，不能静默返回「一切正常」 ────────────────────
    {
        swt::edl::RawProgramPlan empty;
        const DryRunResult r = buildDryRun(empty, {});
        check(!r.ok, "empty plan is rejected");
        check(!r.error.isEmpty(), "empty plan explains why");
    }

    // ── ② 基本统计：条目 / 分区 / LUN / 字节 ──────────────────────────────
    {
        swt::edl::RawProgramPlan plan;
        plan.entries.append(makeEntry(QStringLiteral("boot"), 0, 0, 2048));
        plan.entries.append(makeEntry(QStringLiteral("system"), 0, 2048, 4096));
        plan.entries.append(makeEntry(QStringLiteral("userdata"), 1, 0, 8192));

        const DryRunResult r = buildDryRun(plan, {});
        check(r.ok, "simple plan builds");
        check(r.entryCount == 3, "entry count is right");
        check(r.partitionCount == 3, "partition count is right");
        check(r.lunCount == 2, "lun count is right");
        // 总扇区 2048+4096+8192 = 14336，×512 = 7340032
        check(r.totalWriteBytes == 14336ULL * 512ULL, "total bytes is right");
        check(r.overlaps.isEmpty(), "no overlaps in a clean plan");
    }

    // ── ③ 重叠检测：同 LUN 相交要报，不同 LUN 不算 ────────────────────────
    {
        swt::edl::RawProgramPlan plan;
        // LUN0：boot 占 [0,2048)，system 占 [1024,2048) —— 相交
        plan.entries.append(makeEntry(QStringLiteral("boot"), 0, 0, 2048));
        plan.entries.append(makeEntry(QStringLiteral("system"), 0, 1024, 2048));
        // LUN1 上同样区间，但不与 LUN0 冲突
        plan.entries.append(makeEntry(QStringLiteral("other"), 1, 0, 2048));

        const DryRunResult r = buildDryRun(plan, {});
        check(r.ok, "overlapping plan still builds");
        check(r.overlaps.size() == 1, "exactly one overlap is found");
        if (!r.overlaps.isEmpty()) {
            const DryRunOverlap& o = r.overlaps.first();
            check(o.lun == 0, "overlap is on LUN 0");
            check(o.firstLabel == QStringLiteral("boot"), "overlap names the first entry");
            check(o.secondLabel == QStringLiteral("system"), "overlap names the second entry");
            // 相交段是 [1024, 2048) = 1024 扇区
            check(o.startSector == 1024, "overlap starts at the later start");
            check(o.sectorCount == 1024, "overlap length is the intersection");
        }
        check(!r.warnings.isEmpty(), "overlap produces a warning");
    }

    // ── ④ 相邻但不相交：不该误报 ──────────────────────────────────────────
    // [0,2048) 与 [2048,4096) 是紧挨着的，不是重叠 —— 这是最常见的正常情况，
    // 误报会让用户以为每次刷机都有问题。
    {
        swt::edl::RawProgramPlan plan;
        plan.entries.append(makeEntry(QStringLiteral("a"), 0, 0, 2048));
        plan.entries.append(makeEntry(QStringLiteral("b"), 0, 2048, 2048));

        const DryRunResult r = buildDryRun(plan, {});
        check(r.overlaps.isEmpty(), "adjacent but non-overlapping ranges are not reported");
    }

    // ── ⑤ 完全覆盖：一条包住另一条 ────────────────────────────────────────
    {
        swt::edl::RawProgramPlan plan;
        plan.entries.append(makeEntry(QStringLiteral("big"), 0, 0, 8192));
        plan.entries.append(makeEntry(QStringLiteral("small"), 0, 1024, 512));

        const DryRunResult r = buildDryRun(plan, {});
        check(r.overlaps.size() == 1, "containment is detected as overlap");
        if (!r.overlaps.isEmpty()) {
            check(r.overlaps.first().sectorCount == 512,
                  "containment overlap length equals the smaller range");
        }
    }

    // ── ⑥ 保护分区命中 ────────────────────────────────────────────────────
    {
        swt::edl::RawProgramPlan plan;
        plan.entries.append(makeEntry(QStringLiteral("boot"), 0, 0, 2048));
        plan.entries.append(makeEntry(QStringLiteral("persist"), 0, 2048, 1024));
        plan.entries.append(makeEntry(QStringLiteral("system"), 0, 4096, 2048));

        const QStringList guards = {QStringLiteral("persist"), QStringLiteral("modem")};
        const DryRunResult r = buildDryRun(plan, guards);
        check(r.protectedPartitions.size() == 1, "one protected partition is found");
        check(r.protectedPartitions.contains(QStringLiteral("persist")),
              "the right partition is flagged");
        check(!r.protectedPartitions.contains(QStringLiteral("boot")),
              "non-protected partitions are not flagged");
    }

    // ── ⑦ 保护名单大小写不敏感 ────────────────────────────────────────────
    {
        swt::edl::RawProgramPlan plan;
        plan.entries.append(makeEntry(QStringLiteral("PERSIST"), 0, 0, 1024));
        const DryRunResult r = buildDryRun(plan, {QStringLiteral("persist")});
        check(r.protectedPartitions.size() == 1, "protection match ignores case");
    }

    // ── ⑧ 异常条目要提醒 ──────────────────────────────────────────────────
    {
        swt::edl::RawProgramPlan plan;
        swt::edl::RawProgramEntry bad = makeEntry(QStringLiteral("weird"), 0, 0, 0);
        plan.entries.append(bad);
        const DryRunResult r = buildDryRun(plan, {});
        check(r.ok, "plan with a zero-sector entry still builds");
        check(!r.warnings.isEmpty(), "zero sector count produces a warning");
    }

    // ── ⑨ 报告文案：该提的都要提到 ────────────────────────────────────────
    {
        swt::edl::RawProgramPlan plan;
        plan.entries.append(makeEntry(QStringLiteral("boot"), 0, 0, 2048));
        plan.entries.append(makeEntry(QStringLiteral("persist"), 0, 1024, 2048));
        const DryRunResult r = buildDryRun(plan, {QStringLiteral("persist")});

        const QString text = describeDryRun(r);
        check(text.contains(QStringLiteral("2 个条目")), "report states the entry count");
        check(text.contains(QStringLiteral("保护分区")), "report mentions protected partitions");
        check(text.contains(QStringLiteral("重叠")), "report mentions overlaps");

        // 干净的计划要给出「可以开始」的结论
        swt::edl::RawProgramPlan clean;
        clean.entries.append(makeEntry(QStringLiteral("boot"), 0, 0, 2048));
        const QString cleanText = describeDryRun(buildDryRun(clean, {}));
        check(cleanText.contains(QStringLiteral("未发现重叠")),
              "clean plan says it is safe to start");

        // 失败的计划要把 error 原样带出来
        swt::edl::RawProgramPlan none;
        const QString failText = describeDryRun(buildDryRun(none, {}));
        check(failText.contains(QStringLiteral("没有")), "failed dry-run explains itself");
    }

    if (failures == 0)
        std::cout << "FlashDryRunTest: all checks passed\n";
    else
        std::cerr << "FlashDryRunTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
