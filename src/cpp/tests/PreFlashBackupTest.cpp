// 刷机前自动备份（kc/diag/PreFlashBackup）的回归测试。
//
// 这块的价值全在「计划对不对」和「落盘可不可信」：
//   · 白名单漏了 persist / modem 这类分区 → 用户以为备了、其实没备；
//   · 上限失效 → 把 system 的几 GB 也备进去，磁盘直接爆；
//   · manifest 字段漂 → 将来还原时对不上文件名。
// 所以测的是这些，不是「函数能跑」。

#include "kc/diag/PreFlashBackup.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

constexpr qint64 kMiB = 1024LL * 1024;

} // namespace

int main()
{
    using namespace kc::diag;

    // ── ① 默认白名单必须覆盖「丢了救不回来」的分区 ────────────────────────
    {
        const QStringList keys = defaultKeyPartitions();
        check(!keys.isEmpty(), "key partition list is not empty");

        // 这几个是**唯一数据**类：刷机不会重建，误擦就永久没了。
        for (const char* must : {"persist", "modem", "efs", "nvram"}) {
            const bool has = keys.contains(QString::fromLatin1(must), Qt::CaseInsensitive);
            if (!has) std::cerr << "  missing: " << must << '\n';
            check(has, "unique-data partition is in the whitelist");
        }
        // system / userdata 这类**不该**备（几 GB，且重刷即可）。
        check(!keys.contains(QStringLiteral("system"), Qt::CaseInsensitive),
              "system is NOT backed up (too large, restorable by reflash)");
        check(!keys.contains(QStringLiteral("userdata"), Qt::CaseInsensitive),
              "userdata is NOT backed up");
        check(!keys.contains(QStringLiteral("super"), Qt::CaseInsensitive),
              "super is NOT backed up");
    }

    // ── ② 计划：GPT 永远第一，且只收白名单 + 小分区 ────────────────────────
    {
        QVector<QPair<QString, qint64>> parts = {
            {QStringLiteral("persist"), 32 * kMiB},
            {QStringLiteral("boot"), 64 * kMiB},
            {QStringLiteral("system"), 3LL * 1024 * kMiB},   // 超大，跳过
            {QStringLiteral("userdata"), 100LL * 1024 * kMiB}, // 不在白名单
            {QStringLiteral("modem"), 100 * kMiB},
            {QStringLiteral("randomstuff"), 1 * kMiB},       // 不在白名单
        };
        const BackupPlan plan = buildBackupPlan(parts);

        check(!plan.targets.isEmpty(), "plan has targets");
        check(plan.targets.first().isGpt, "GPT is the first target (it explains everything else)");
        checkStr(plan.targets.first().name, QString::fromLatin1(kGptTargetName),
                 "GPT target uses the fixed name");

        // 白名单内的三个都应该在
        const auto hasName = [&plan](const QString& n) {
            for (const BackupTarget& t : plan.targets)
                if (t.name.compare(n, Qt::CaseInsensitive) == 0) return true;
            return false;
        };
        check(hasName(QStringLiteral("persist")), "persist is planned");
        check(hasName(QStringLiteral("boot")), "boot is planned");
        check(hasName(QStringLiteral("modem")), "modem is planned");
        check(!hasName(QStringLiteral("system")), "system is not planned");
        check(!hasName(QStringLiteral("userdata")), "userdata is not planned");
        check(!hasName(QStringLiteral("randomstuff")), "non-whitelisted partition is not planned");

        // 跳过的要有说明（用户得知道为什么少了几个）
        check(!plan.skipped.isEmpty(), "skipped entries are explained");
        check(plan.totalBytes > 0, "total bytes is computed");
    }

    // ── ③ 上限生效：超过 cap 的白名单分区也要跳过 ──────────────────────────
    {
        QVector<QPair<QString, qint64>> parts = {
            {QStringLiteral("persist"), 10 * kMiB},
            {QStringLiteral("modem"), 500 * kMiB},   // 超过 256MiB 默认上限
        };
        const BackupPlan plan = buildBackupPlan(parts);
        int modemPlanned = 0;
        for (const BackupTarget& t : plan.targets)
            if (t.name == QStringLiteral("modem")) ++modemPlanned;
        check(modemPlanned == 0, "oversized whitelisted partition is skipped by the cap");
        check(!plan.skipped.isEmpty(), "oversized skip is explained");

        // 放宽上限后应该能进来
        const BackupPlan wide = buildBackupPlan(parts, 1024LL * kMiB);
        int modemWide = 0;
        for (const BackupTarget& t : wide.targets)
            if (t.name == QStringLiteral("modem")) ++modemWide;
        check(modemWide == 1, "raising the cap lets it in");
    }

    // ── ④ 大小未知（0）的分区不备，但要说明 ───────────────────────────────
    {
        QVector<QPair<QString, qint64>> parts = {{QStringLiteral("persist"), 0}};
        const BackupPlan plan = buildBackupPlan(parts);
        int n = 0;
        for (const BackupTarget& t : plan.targets)
            if (t.name == QStringLiteral("persist")) ++n;
        check(n == 0, "partition with unknown size is skipped");
        check(!plan.skipped.isEmpty(), "unknown-size skip is explained");
    }

    // ── ⑤ 空分区表：仍然要有 GPT 一项 ──────────────────────────────────────
    {
        const BackupPlan plan = buildBackupPlan({});
        check(plan.targets.size() == 1 && plan.targets.first().isGpt,
              "empty partition table still plans the GPT");
    }

    // ── ⑥ 目录名：设备标识里的非法字符要被清洗 ────────────────────────────
    {
        QString err;
        const QString dir = createBackupDir(QStringLiteral("SM/855:ABC DEF*?"), &err);
        check(!dir.isEmpty(), "backup dir is created");
        if (!dir.isEmpty()) {
            const QString base = QFileInfo(dir).fileName();
            // 不能含路径分隔符（否则会跑到别的目录去）
            check(!base.contains(QLatin1Char('/')), "dir name has no path separator");
            check(!base.contains(QLatin1Char('\\')), "dir name has no backslash");
            check(!base.contains(QLatin1Char(' ')), "dir name has no space");
            check(base.contains(QStringLiteral("SM_855")), "device tag is preserved (cleaned)");
            QDir().rmdir(dir);
        }
        // 空标识要能兜底
        const QString dir2 = createBackupDir(QString(), &err);
        check(!dir2.isEmpty(), "empty device tag still yields a dir");
        if (!dir2.isEmpty()) QDir().rmdir(dir2);
    }

    // ── ⑦ manifest：字段齐全且能被解析回来 ────────────────────────────────
    {
        const QString dir = createBackupDir(QStringLiteral("TESTDEV"), nullptr);
        check(!dir.isEmpty(), "manifest test dir created");
        if (!dir.isEmpty()) {
            BackupResult r;
            r.ok = true;
            r.succeededCount = 2;
            r.elapsedMs = 1234;

            BackupFileResult a;
            a.name = QStringLiteral("__gpt__");
            a.path = QDir(dir).filePath(QStringLiteral("__gpt__.bin"));
            a.sizeBytes = 34816;
            a.sha256 = QStringLiteral("aabb");
            a.ok = true;
            r.files.append(a);

            BackupFileResult b;
            b.name = QStringLiteral("persist");
            b.path = QDir(dir).filePath(QStringLiteral("persist.img"));
            b.sizeBytes = 1024;
            b.sha256 = QStringLiteral("ccdd");
            b.ok = true;
            r.files.append(b);

            r.skipped.append(QStringLiteral("system（过大）"));

            QString err;
            check(writeManifest(dir, r, QStringLiteral("SM8550 / ABC123"), &err),
                  "manifest is written");

            const QString mf = QDir(dir).filePath(QStringLiteral("manifest.json"));
            QFile f(mf);
            check(f.open(QIODevice::ReadOnly), "manifest can be read back");
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            f.close();

            checkStr(o.value(QStringLiteral("device")).toString(),
                     QStringLiteral("SM8550 / ABC123"), "manifest keeps device info");
            check(o.value(QStringLiteral("ok")).toBool(), "manifest records ok");
            check(o.value(QStringLiteral("files")).toArray().size() == 2,
                  "manifest lists both files");
            check(o.value(QStringLiteral("skipped")).toArray().size() == 1,
                  "manifest records what was skipped");

            const QJsonObject first = o.value(QStringLiteral("files")).toArray().at(0).toObject();
            checkStr(first.value(QStringLiteral("sha256")).toString(),
                     QStringLiteral("aabb"), "manifest keeps per-file sha256");
            checkStr(first.value(QStringLiteral("file")).toString(),
                     QStringLiteral("__gpt__.bin"), "manifest stores the file name only");

            QDir(dir).removeRecursively();
        }
    }

    // ── ⑧ prune：只保留最近 N 次 ───────────────────────────────────────────
    // 备份会占磁盘，必须能自动清理；这里用真实目录验证「删旧留新」。
    {
        const QString root = backupRootDir();
        QDir().mkpath(root);
        // 造 5 个按时间排序的假备份目录
        QStringList made;
        for (int i = 0; i < 5; ++i) {
            const QString name = QStringLiteral("2026010%1-120000_DEV").arg(i);
            QDir().mkpath(QDir(root).filePath(name));
            made.append(name);
        }
        pruneOldBackups(2);

        int remain = 0;
        for (const QString& n : made) {
            if (QDir(QDir(root).filePath(n)).exists()) ++remain;
        }
        check(remain <= 2, "prune keeps only the newest N backups");

        // 清理干净，别给后续测试留垃圾
        for (const QString& n : made) QDir(QDir(root).filePath(n)).removeRecursively();
    }

    if (failures == 0)
        std::cout << "PreFlashBackupTest: all checks passed\n";
    else
        std::cerr << "PreFlashBackupTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
