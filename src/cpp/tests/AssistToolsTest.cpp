// 镜像识别 / 设备报告 / 刷机前检查 三个模块的回归测试。
//
// 三者的共同点：**判断错了不会崩，只会让用户做错决定**。
//   · 镜像类型认错 → 刷错分区；
//   · 报告漏字段 → 求助时对方要反复问；
//   · 检查阈值错 → 要么白拦（用户烦），要么没拦住（变砖）。
// 所以测的是判断结果，不是「函数能跑」。

#include "kc/tools/DeviceReport.h"
#include "kc/tools/ImageProbe.h"
#include "kc/tools/PreflightChecks.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

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

void checkStr(const QString& got, const QString& want, const char* what)
{
    if (got == want) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL: " << what << "\n  got:  [" << got.toStdString()
                  << "]\n  want: [" << want.toStdString() << "]\n";
    }
}

QString writeBytes(const QTemporaryDir& dir, const QString& name, const QByteArray& data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(data);
    f.close();
    return path;
}

constexpr qint64 kMiB = 1024LL * 1024;
constexpr qint64 kGiB = 1024LL * kMiB;

} // namespace

int main()
{
    using namespace kc::tools;

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "无法创建临时目录\n";
        return 1;
    }

    // ══ ① ImageProbe：按魔数识别，不看扩展名 ══════════════════════════════
    {
        // Android sparse：3A FF 26 ED + 填充
        QByteArray sparse = QByteArray::fromHex("3AFF26ED") + QByteArray(200, '\0');
        const QString p1 = writeBytes(dir, QStringLiteral("a.bin"), sparse);
        const ImageProbeResult r1 = probeImage(p1);
        check(r1.ok, "sparse probe succeeds");
        check(r1.kind == ImageKind::AndroidSparse, "sparse magic is recognised");
        check(!r1.kindName.isEmpty(), "sparse has a display name");
        check(!r1.evidence.isEmpty(), "sparse carries evidence text");

        // Android boot："ANDROID!" + 填充
        QByteArray boot = QByteArray("ANDROID!") + QByteArray(200, '\x11');
        const QString p2 = writeBytes(dir, QStringLiteral("b.img"), boot);
        const ImageProbeResult r2 = probeImage(p2);
        check(r2.kind == ImageKind::AndroidBoot, "boot magic is recognised");

        // ext4：偏移 0x438 处 53 EF
        QByteArray ext4(0x600, '\0');
        ext4[0x438] = static_cast<char>(0x53);
        ext4[0x439] = static_cast<char>(0xEF);
        // 前面填点非零数据，避免被当成「全 0」
        ext4[0] = static_cast<char>(0x01);
        const QString p3 = writeBytes(dir, QStringLiteral("c.img"), ext4);
        const ImageProbeResult r3 = probeImage(p3);
        check(r3.kind == ImageKind::Ext4, "ext4 magic at 0x438 is recognised");

        // 全 0
        const QString p4 = writeBytes(dir, QStringLiteral("d.img"), QByteArray(512, '\0'));
        const ImageProbeResult r4 = probeImage(p4);
        check(r4.kind == ImageKind::AllZero, "all-zero image is recognised");

        // 0 字节：是有效结论，不是失败
        const QString p5 = writeBytes(dir, QStringLiteral("e.img"), QByteArray());
        const ImageProbeResult r5 = probeImage(p5);
        check(r5.ok, "empty file probes successfully");
        check(r5.kind == ImageKind::AllZero, "empty file is treated as all-zero");

        // 普通数据
        const QString p6 = writeBytes(dir, QStringLiteral("f.img"),
                                      QByteArray("random payload here"));
        const ImageProbeResult r6 = probeImage(p6);
        check(r6.kind == ImageKind::Plain, "unknown magic falls back to plain");

        // 不存在 / 空路径：都要有明确错误，不能崩
        const ImageProbeResult r7 = probeImage(dir.filePath(QStringLiteral("nope.img")));
        check(!r7.ok, "missing file is reported as failure");
        check(!r7.error.isEmpty(), "missing file explains why");

        const ImageProbeResult r8 = probeImage(QString());
        check(!r8.ok, "empty path is rejected");
    }

    // ══ ② 分区名建议：按文件名猜，猜不出就空 ══════════════════════════════
    {
        checkStr(suggestPartitionName(ImageKind::AndroidBoot, QStringLiteral("boot.img")),
                 QStringLiteral("boot"), "boot.img suggests boot");
        // init_boot 必须先于 boot 匹配，否则会被 boot 吃掉
        checkStr(suggestPartitionName(ImageKind::AndroidBoot, QStringLiteral("init_boot.img")),
                 QStringLiteral("init_boot"), "init_boot wins over boot");
        checkStr(suggestPartitionName(ImageKind::Ext4, QStringLiteral("system.img")),
                 QStringLiteral("system"), "system.img suggests system");
        checkStr(suggestPartitionName(ImageKind::Plain, QStringLiteral("whatever.bin")),
                 QString(), "unknown name yields no suggestion");
    }

    // ══ ③ DeviceReport：文本与 JSON 都要完整 ══════════════════════════════
    {
        DeviceReportInput in;
        in.deviceSummary = QStringLiteral("COM7 · 小米 23127PN0CC · 0xABC123");
        in.appVersion = QStringLiteral("2.0.8");
        in.generatedAt = QStringLiteral("2026-10-31T14:00:00");
        in.partitions = {{QStringLiteral("boot"), 64 * kMiB},
                         {QStringLiteral("system"), 3 * kGiB}};
        in.extraInfo = {{QStringLiteral("协议"), QStringLiteral("Firehose")},
                        {QStringLiteral("存储"), QStringLiteral("UFS")}};

        const QString text = buildTextReport(in);
        check(text.contains(QStringLiteral("COM7")), "text report contains the device");
        check(text.contains(QStringLiteral("2.0.8")), "text report contains the version");
        check(text.contains(QStringLiteral("boot")), "text report lists partitions");
        check(text.contains(QStringLiteral("Firehose")), "text report lists extra info");
        check(text.contains(QStringLiteral("分区表")), "text report has a partition section");

        const QByteArray json = buildJsonReport(in);
        const QJsonObject o = QJsonDocument::fromJson(json).object();
        checkStr(o.value(QStringLiteral("device")).toString(), in.deviceSummary,
                 "json keeps the device summary");
        checkStr(o.value(QStringLiteral("appVersion")).toString(), QStringLiteral("2.0.8"),
                 "json keeps the version");
        check(o.value(QStringLiteral("partitions")).toArray().size() == 2,
              "json lists both partitions");
        check(o.value(QStringLiteral("extra")).toObject().size() == 2,
              "json lists extra info");

        // 空输入也要能出报告（不崩、有基本框架）
        DeviceReportInput empty;
        const QString emptyText = buildTextReport(empty);
        check(!emptyText.isEmpty(), "empty input still yields a report");
        check(emptyText.contains(QStringLiteral("未连接")), "empty input says not connected");
        check(emptyText.contains(QStringLiteral("未读取分区表")),
              "empty input says no partition table");
    }

    // ══ ④ 电量检查：阈值边界 ══════════════════════════════════════════════
    {
        check(checkBattery(80, false).level == CheckLevel::Info, "80% is fine");
        check(checkBattery(15, false).level == CheckLevel::Error,
              "15% not charging is an error");
        check(checkBattery(15, true).level == CheckLevel::Info,
              "15% while charging is fine");
        check(checkBattery(30, false).level == CheckLevel::Warning,
              "30% not charging is a warning");
        // 读不到电量不该拦人
        check(checkBattery(-1, false).level == CheckLevel::Info,
              "unknown battery level does not block");
    }

    // ══ ⑤ 磁盘空间：1.5 倍 / 2 倍阈值 ═════════════════════════════════════
    {
        const qint64 need = 100 * kMiB;
        check(checkDiskSpace(500 * kMiB, need).level == CheckLevel::Info,
              "plenty of space is fine");
        check(checkDiskSpace(120 * kMiB, need).level == CheckLevel::Error,
              "less than 1.5x is an error");
        check(checkDiskSpace(180 * kMiB, need).level == CheckLevel::Warning,
              "between 1.5x and 2x is a warning");
        check(checkDiskSpace(0, 0).level == CheckLevel::Info,
              "zero requirement needs no space");
    }

    // ══ ⑥ 镜像大小检查 ════════════════════════════════════════════════════
    {
        check(checkImageSize(10 * kMiB, 20 * kMiB, QStringLiteral("boot")).level
                  == CheckLevel::Info,
              "image smaller than partition is fine");
        check(checkImageSize(30 * kMiB, 20 * kMiB, QStringLiteral("boot")).level
                  == CheckLevel::Error,
              "image larger than partition is an error");
        // 信息不全时不该乱判
        check(checkImageSize(0, 20 * kMiB, QStringLiteral("boot")).level == CheckLevel::Info,
              "missing size info skips the check");
    }

    // ══ ⑦ 汇总 ════════════════════════════════════════════════════════════
    {
        QVector<CheckItem> items;
        CheckItem a; a.level = CheckLevel::Info; items.append(a);
        CheckItem b; b.level = CheckLevel::Warning; items.append(b);
        check(worstLevel(items) == CheckLevel::Warning, "worst level picks the warning");
        check(summaryLine(items).contains(QStringLiteral("提醒")),
              "summary mentions the warning");

        CheckItem c; c.level = CheckLevel::Error; items.append(c);
        check(worstLevel(items) == CheckLevel::Error, "worst level picks the error");
        check(summaryLine(items).contains(QStringLiteral("必须处理")),
              "summary mentions the error");

        check(worstLevel({}) == CheckLevel::Info, "empty list is info");
    }

    if (failures == 0)
        std::cout << "AssistToolsTest: all checks passed\n";
    else
        std::cerr << "AssistToolsTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
