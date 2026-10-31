// 一键「恢复上次现场」的纯逻辑回归（2026-10-31）。
//
// 钉死的都是「错了会刷错机器 / 刷空包」的点：
//   · 白名单：只有 写入 / XML 刷机 / 擦除 / 读取 能重放，GPT 写入 / 整盘擦除 /
//     恢复出厂 / 基带备份 必须拒绝（误触代价太大）；
//   · 设备序列号不一致必须拒绝，且**不能**因为 COM 口号相同就放行；
//   · 来源包不存在必须拒绝；只有摘要没有路径的也要拒绝（不能拿去拼路径）；
//   · 分区摘要解析：枚举 / 「N 个」/「前几个等 N 个」/「N 个 program 条目」。
// 不碰真实磁盘：需要的「存在的包」用 QTemporaryDir 里的真实文件。

#include "kc/tools/FlashReplay.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

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

/// 造一条记录：默认「写入 + 设备一致 + 包存在」。
kc::diag::FlashRecord makeRecord(const QString& operation, const QString& package)
{
    kc::diag::FlashRecord r;
    r.time = QDateTime::currentDateTime();
    r.operation = operation;
    r.device = QStringLiteral("COM7 · 小米 23127PN0CC · 0x1234abcd");
    r.package = package;
    r.partitions = QStringLiteral("boot, system, vendor 等 3 个");
    r.success = true;
    r.message = QStringLiteral("完成");
    r.durationMs = 1000;
    return r;
}

const QString kCurrentDevice = QStringLiteral("COM7 · 小米 23127PN0CC · 0x1234abcd");

} // namespace

int main()
{
    using namespace kc::tools;

    // ── ① 白名单：哪些操作能重放 ────────────────────────────────────────
    {
        expect(isReplayableOperation(QStringLiteral("写入")), "replayable-write");
        expect(isReplayableOperation(QStringLiteral("XML 刷机")), "replayable-xml");
        expect(isReplayableOperation(QStringLiteral("擦除")), "replayable-erase");
        expect(isReplayableOperation(QStringLiteral("读取")), "replayable-read");
        expect(isReplayableOperation(QStringLiteral("回读")), "replayable-readback");

        // 危险操作一律不放行。
        expect(!isReplayableOperation(QStringLiteral("GPT 写入")), "reject-gpt-write");
        expect(!isReplayableOperation(QStringLiteral("整盘擦除")), "reject-full-erase");
        expect(!isReplayableOperation(QStringLiteral("恢复出厂")), "reject-factory-reset");
        expect(!isReplayableOperation(QStringLiteral("基带备份")), "reject-modem-backup");
        expect(!isReplayableOperation(QStringLiteral("GPT 导出")), "reject-gpt-export");
        expect(!isReplayableOperation(QString()), "reject-empty-operation");
    }

    // ── ② 设备身份提取：端口会变，identity 才作数 ────────────────────────
    {
        expect(deviceIdentity(QStringLiteral("COM7 · 小米 23127PN0CC · 0x1234abcd"))
                   == QStringLiteral("0x1234abcd"),
               "identity-from-full-summary",
               deviceIdentity(QStringLiteral("COM7 · 小米 23127PN0CC · 0x1234abcd")).toStdString());

        // 只有端口 → 拿不到 identity（端口随插拔变，不能当身份）。
        expect(deviceIdentity(QStringLiteral("COM7")).isEmpty(), "identity-port-only-rejected");
        expect(deviceIdentity(QStringLiteral("com12")).isEmpty(), "identity-port-lowercase-rejected");
        // 只有一段但不是端口形状（机型拿不到时摘要只剩序列号）→ 当 identity。
        expect(deviceIdentity(QStringLiteral("0x1234abcd")) == QStringLiteral("0x1234abcd"),
               "identity-single-serial");
        expect(deviceIdentity(QString()).isEmpty(), "identity-empty");

        // 端口不同但序列号相同 → 同一台（这正是要允许的情况）。
        expect(isSameDevice(QStringLiteral("COM7 · 小米 · 0x1234abcd"),
                            QStringLiteral("COM9 · 小米 · 0x1234abcd")),
               "same-device-port-changed");
        // 端口相同但序列号不同 → 不同机器（批量场景最容易踩的坑）。
        expect(!isSameDevice(QStringLiteral("COM7 · 小米 · 0xaaaa1111"),
                             QStringLiteral("COM7 · 小米 · 0xbbbb2222")),
               "different-device-same-port");
        // 任一方拿不到 identity → 一律当不同，宁可拒绝。
        expect(!isSameDevice(QStringLiteral("COM7"), QStringLiteral("COM7")),
               "same-device-unknown-identity-rejected");
        expect(!isSameDevice(QString(), kCurrentDevice), "same-device-empty-rejected");
        // 大小写不敏感（hwid 打印大小写不统一）。
        expect(isSameDevice(QStringLiteral("COM7 · 小米 · 0xABCD"), QStringLiteral("COM7 · 小米 · 0xabcd")),
               "same-device-case-insensitive");
    }

    // ── ③ 分区摘要解析 ──────────────────────────────────────────────────
    {
        int total = -999;
        const QStringList enumerated =
            parsePartitionSummary(QStringLiteral("boot, system, vendor"), &total);
        expect(enumerated.size() == 3 && total == 3, "parse-partitions-enumerated",
               std::to_string(enumerated.size()) + "/" + std::to_string(total));
        expect(enumerated.contains(QStringLiteral("system")), "parse-partitions-names");

        // 「前几个 + 总数」：名字只拿到列出的那几个，total 是真实总数。
        const QStringList truncated =
            parsePartitionSummary(QStringLiteral("boot, system, vendor 等 69 个"), &total);
        expect(truncated.size() == 3, "parse-partitions-truncated-count",
               std::to_string(truncated.size()));
        expect(total == 69, "parse-partitions-total-69", std::to_string(total));

        // 只有数量没有名字。
        const QStringList countOnly = parsePartitionSummary(QStringLiteral("69 个分区"), &total);
        expect(countOnly.isEmpty(), "parse-partitions-count-only-no-names");
        expect(total == 69, "parse-partitions-count-only-total", std::to_string(total));

        // 「N 个 program 条目」不是分区名，别硬拆成 "69 个 program 条目" 一个名字。
        const QStringList program = parsePartitionSummary(QStringLiteral("69 个 program 条目"), &total);
        expect(program.isEmpty(), "parse-partitions-program-entries-not-names",
               std::to_string(program.size()));

        expect(parsePartitionSummary(QString(), &total).isEmpty(), "parse-partitions-empty");
        expect(total == -1, "parse-partitions-empty-total-unknown", std::to_string(total));
    }

    // ── ④ 正常路径：能重放，计划字段正确 ────────────────────────────────
    {
        QTemporaryDir tmp;
        expect(tmp.isValid(), "replay-tmpdir");
        const QString packagePath = QDir(tmp.path()).filePath(QStringLiteral("umi.xml"));
        {
            QFile f(packagePath);
            expect(f.open(QIODevice::WriteOnly), "replay-create-package");
            f.write("dummy");
        }

        const kc::diag::FlashRecord record = makeRecord(QStringLiteral("XML 刷机"), packagePath);
        const ReplayPlan plan = buildReplayPlan(record, kCurrentDevice);

        expect(plan.ok, "replay-ok", plan.reason.toStdString());
        expect(plan.operation == QStringLiteral("XML 刷机"), "replay-operation");
        expect(plan.packagePath == packagePath, "replay-package-path",
               plan.packagePath.toStdString());
        expect(plan.partitions.size() == 3, "replay-partitions",
               std::to_string(plan.partitions.size()));
        expect(!plan.notes.isEmpty(), "replay-has-notes");

        // 文案：给人看的，必须带操作名与那句风险提示。
        const QString text = describeReplay(plan);
        expect(text.contains(QStringLiteral("XML 刷机")), "describe-contains-operation",
               text.toStdString());
        expect(text.contains(QStringLiteral("覆盖设备上的数据")), "describe-has-warning");
        expect(!text.contains(QStringLiteral("无法重放")), "describe-ok-no-refusal");
    }

    // ── ⑤ 设备不匹配 → 拒绝，且理由说清楚 ───────────────────────────────
    {
        QTemporaryDir tmp;
        const QString packagePath = QDir(tmp.path()).filePath(QStringLiteral("umi.xml"));
        { QFile f(packagePath); f.open(QIODevice::WriteOnly); f.write("x"); }

        const kc::diag::FlashRecord record = makeRecord(QStringLiteral("XML 刷机"), packagePath);

        // 5-1 另一台机器（端口相同、序列号不同）—— 批量场景最危险的情况。
        const ReplayPlan other = buildReplayPlan(
            record, QStringLiteral("COM7 · 小米 23127PN0CC · 0xdeadbeef"));
        expect(!other.ok, "reject-different-device");
        expect(other.reason.contains(QStringLiteral("设备不一致")), "reject-reason-device",
               other.reason.toStdString());

        // 5-2 端口变了但序列号相同 → 允许（这是正常的重新插拔）。
        const ReplayPlan portChanged = buildReplayPlan(
            record, QStringLiteral("COM9 · 小米 23127PN0CC · 0x1234abcd"));
        expect(portChanged.ok, "allow-same-device-port-changed",
               portChanged.reason.toStdString());
        expect(!portChanged.notes.isEmpty()
                   && portChanged.notes.join(QStringLiteral("\n"))
                          .contains(QStringLiteral("端口")),
               "note-mentions-port-change");

        // 5-3 没有连接设备 → 拒绝。
        const ReplayPlan noDevice = buildReplayPlan(record, QString());
        expect(!noDevice.ok, "reject-no-device");
        expect(noDevice.reason.contains(QStringLiteral("没有连接设备")), "reject-reason-no-device",
               noDevice.reason.toStdString());

        // 5-4 记录里没有设备信息 → 拒绝（无法确认，不能默认放行）。
        kc::diag::FlashRecord noDeviceRecord = record;
        noDeviceRecord.device.clear();
        const ReplayPlan unknown = buildReplayPlan(noDeviceRecord, kCurrentDevice);
        expect(!unknown.ok, "reject-record-without-device");
    }

    // ── ⑥ 来源包不存在 / 只是摘要 → 拒绝 ────────────────────────────────
    {
        QTemporaryDir tmp;
        // 6-1 路径指向不存在的文件。
        const QString missing =
            QDir(tmp.path()).filePath(QStringLiteral("已经删掉了.xml"));
        const ReplayPlan notFound =
            buildReplayPlan(makeRecord(QStringLiteral("XML 刷机"), missing), kCurrentDevice);
        expect(!notFound.ok, "reject-missing-package");
        expect(notFound.reason.contains(QStringLiteral("不存在")), "reject-reason-missing",
               notFound.reason.toStdString());

        // 6-2 包字段是「给人看的摘要」而不是路径（历史里就是这么存的）。
        const ReplayPlan summaryOnly = buildReplayPlan(
            makeRecord(QStringLiteral("写入"), QStringLiteral("boot.img 等 5 个镜像")),
            kCurrentDevice);
        expect(!summaryOnly.ok, "reject-package-summary-not-path");
        expect(summaryOnly.reason.contains(QStringLiteral("摘要")), "reject-reason-summary",
               summaryOnly.reason.toStdString());

        // 6-3 包字段为空。
        const ReplayPlan emptyPackage =
            buildReplayPlan(makeRecord(QStringLiteral("写入"), QString()), kCurrentDevice);
        expect(!emptyPackage.ok, "reject-empty-package");

        // 6-4 不可重放的操作：即使设备与包都没问题也要拒绝。
        const QString realPackage = QDir(tmp.path()).filePath(QStringLiteral("umi.xml"));
        { QFile f(realPackage); f.open(QIODevice::WriteOnly); f.write("x"); }
        const ReplayPlan gptWrite =
            buildReplayPlan(makeRecord(QStringLiteral("GPT 写入"), realPackage), kCurrentDevice);
        expect(!gptWrite.ok, "reject-non-replayable-operation");
        expect(gptWrite.reason.contains(QStringLiteral("GPT 写入")), "reject-reason-operation",
               gptWrite.reason.toStdString());
    }

    // ── ⑦ 回读：输出目录不存在不算错（执行时创建） ───────────────────────
    {
        QTemporaryDir tmp;
        const QString outDir = QDir(tmp.path()).filePath(QStringLiteral("还没建好的目录"));
        const ReplayPlan plan =
            buildReplayPlan(makeRecord(QStringLiteral("回读"), outDir), kCurrentDevice);
        expect(plan.ok, "readback-missing-outdir-ok", plan.reason.toStdString());
        expect(plan.notes.join(QStringLiteral("\n")).contains(QStringLiteral("执行时会创建")),
               "readback-note-will-create");
    }

    // ── ⑧ 失败记录的文案：允许重放但必须提醒 ─────────────────────────────
    {
        QTemporaryDir tmp;
        const QString packagePath = QDir(tmp.path()).filePath(QStringLiteral("umi.xml"));
        { QFile f(packagePath); f.open(QIODevice::WriteOnly); f.write("x"); }

        kc::diag::FlashRecord failed = makeRecord(QStringLiteral("XML 刷机"), packagePath);
        failed.success = false;
        failed.message = QStringLiteral("传输中断");
        const ReplayPlan plan = buildReplayPlan(failed, kCurrentDevice);
        expect(plan.ok, "failed-record-still-replayable", plan.reason.toStdString());
        expect(plan.notes.join(QStringLiteral("\n")).contains(QStringLiteral("失败")),
               "failed-record-note");
    }

    // ── ⑨ 拒绝时的 describeReplay：只有原因，不能出现「将重放」 ───────────
    {
        ReplayPlan bad;
        bad.ok = false;
        bad.reason = QStringLiteral("来源包已不存在：D:/gone.xml");
        const QString text = describeReplay(bad);
        expect(text.contains(QStringLiteral("无法重放")), "describe-refusal-prefix",
               text.toStdString());
        expect(text.contains(QStringLiteral("D:/gone.xml")), "describe-refusal-reason");
        expect(!text.contains(QStringLiteral("将重放")), "describe-refusal-no-plan");

        // 原因缺失也不能吐出空白弹窗。
        ReplayPlan bare;
        bare.ok = false;
        expect(describeReplay(bare).contains(QStringLiteral("没有给出原因")),
               "describe-refusal-empty-reason");
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " FAILURE(S)\n";
    return 1;
}
