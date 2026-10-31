// 刷机历史单测：JSONL 行格式、CSV 导出、落盘与容量上限。
// 覆盖：format/parse 往返（中文/逗号/引号/换行）、坏行拒绝、CSV 转义与表头、
//       追加后最新在前、重新读盘一致、超过 MaxRecords 只留最近 500 条、clear。
// 断言全部基于**实际文件内容**，不凭印象。
// 注意：每个用例都先 QTemporaryDir + setDirForTest，绝不写真实用户目录。

#include "kc/diag/FlashHistory.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
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

QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

/// 造一条字段都好认的记录，便于逐字段核对。
kc::diag::FlashRecord makeRecord(const QDateTime& t, const QString& op, bool success,
                                 qint64 durationMs)
{
    kc::diag::FlashRecord r;
    r.time = t;
    r.operation = op;
    r.device = QStringLiteral("COM7 · 小米 23127PN0CC · 0x1234abcd");
    r.package = QStringLiteral("umi_images_V14.xml");
    r.partitions = QStringLiteral("boot, system, vendor 等 3 个");
    r.success = success;
    r.message = QStringLiteral("完成");
    r.durationMs = durationMs;
    return r;
}

} // namespace

int main()
{
    // ── formatRecordLine → parseRecordLine 往返：中文与 CSV 敏感字符都不能坏 ──
    {
        kc::diag::FlashRecord original;
        original.time = QDateTime(QDate(2026, 10, 29), QTime(18, 31, 52, 123));
        original.operation = QStringLiteral("XML 刷机");
        original.device = QStringLiteral("COM7 · 小米 23127PN0CC · 0x1234abcd");
        original.package = QStringLiteral("含,逗号 \"引号\" 的包名.zip");
        original.partitions = QStringLiteral("69 个分区");
        original.success = true;
        original.message = QStringLiteral("第一行\n第二行,带逗号 \"还有引号\"");
        original.durationMs = 12345;

        const QString line = kc::diag::formatRecordLine(original);
        expect(!line.contains(QLatin1Char('\n')), "format-no-newline",
               line.toStdString());
        expect(line.startsWith(QLatin1Char('{')) && line.endsWith(QLatin1Char('}'))
                   && line.contains(QStringLiteral("\"success\":true")),
               "format-json-shape", line.toStdString());

        kc::diag::FlashRecord parsed;
        const bool ok = kc::diag::parseRecordLine(line.toUtf8(), &parsed);
        expect(ok, "roundtrip-parse-ok");
        expect(parsed.time == original.time, "roundtrip-time",
               parsed.time.toString(Qt::ISODateWithMs).toStdString());
        expect(parsed.operation == original.operation, "roundtrip-operation",
               parsed.operation.toStdString());
        expect(parsed.device == original.device, "roundtrip-device");
        expect(parsed.package == original.package, "roundtrip-package",
               parsed.package.toStdString());
        expect(parsed.partitions == original.partitions, "roundtrip-partitions");
        expect(parsed.success == original.success, "roundtrip-success");
        expect(parsed.message == original.message, "roundtrip-message",
               parsed.message.toStdString());
        expect(parsed.durationMs == original.durationMs, "roundtrip-duration",
               std::to_string(parsed.durationMs));

        // 毫秒必须真的留在文件里（ISODateWithMs），不是四舍五入到秒。
        expect(line.contains(QStringLiteral("2026-10-29T18:31:52.123")),
               "format-iso-with-ms", line.toStdString());

        // 空的可选字段也要能往返（package / partitions 允许为空）。
        kc::diag::FlashRecord bare;
        bare.time = QDateTime(QDate(2026, 1, 1), QTime(0, 0, 0));
        bare.operation = QStringLiteral("擦除");
        const kc::diag::FlashRecord bareParsed = [&bare] {
            kc::diag::FlashRecord out;
            kc::diag::parseRecordLine(kc::diag::formatRecordLine(bare).toUtf8(), &out);
            return out;
        }();
        expect(bareParsed.package.isEmpty() && bareParsed.partitions.isEmpty(),
               "roundtrip-empty-optional");
        expect(!bareParsed.success && bareParsed.durationMs == 0, "roundtrip-defaults");
    }

    // ── 坏行：返回 false 且不改 *out ────────────────────────────────────
    {
        kc::diag::FlashRecord sentinel;
        sentinel.operation = QStringLiteral("不许被改");
        sentinel.device = QStringLiteral("SENTINEL");

        const QVector<QByteArray> badLines{
            QByteArray(),                                        // 空行
            QByteArray("   "),                                   // 只有空白
            QByteArray("{"),                                     // 半个 JSON
            QByteArray("纯文本不是 JSON"),                        // 纯文本
            QByteArray("[1,2,3]"),                               // 是 JSON 但不是对象
            QByteArray(R"({"time":"2026-10-29T18:31:52.123","operation":"写入",)"
                       R"("device":"d","package":"p","partitions":"q","success":"yes",)"
                       R"("message":"m","durationMs":1})"),       // success 类型不对
            QByteArray(R"({"time":"2026-10-29T18:31:52.123","operation":"写入",)"
                       R"("device":"d","package":"p","partitions":"q","success":true,)"
                       R"("message":"m","durationMs":"12"})"),    // durationMs 类型不对
            QByteArray(R"({"operation":"写入","device":"d","package":"p",)"
                       R"("partitions":"q","success":true,"message":"m","durationMs":1})"),
            // 缺 time 字段
        };

        for (const QByteArray& bad : badLines) {
            kc::diag::FlashRecord out = sentinel;
            const bool parsed = kc::diag::parseRecordLine(bad, &out);
            expect(!parsed, "bad-line-rejected", bad.toStdString());
            expect(out.operation == sentinel.operation && out.device == sentinel.device,
                   "bad-line-keeps-out", bad.toStdString());
        }

        // 空指针也不能崩。
        expect(!kc::diag::parseRecordLine(QByteArray("{}"), nullptr), "bad-line-null-out");
    }

    // ── recordsToCsv：表头、RFC4180 转义、空表、耗时格式化 ──────────────
    {
        const QString emptyCsv = kc::diag::recordsToCsv({});
        expect(emptyCsv == QStringLiteral("时间,操作,设备,来源包,分区,结果,耗时(秒),消息\n"),
               "csv-header-only-when-empty", emptyCsv.toStdString());

        kc::diag::FlashRecord a;
        a.time = QDateTime(QDate(2026, 10, 29), QTime(9, 5, 3));
        a.operation = QStringLiteral("写入");
        a.device = QStringLiteral("COM7 · 小米");
        a.package = QStringLiteral("a,b.zip");                       // 逗号 → 加引号
        a.partitions = QStringLiteral("他说\"OK\"");                  // 引号 → 翻倍
        a.success = true;
        a.durationMs = 12340;                                        // → 12.34
        a.message = QStringLiteral("第一行\n第二行");                  // 换行 → 加引号

        kc::diag::FlashRecord b;
        b.time = QDateTime(QDate(2026, 10, 29), QTime(9, 6, 4));
        b.operation = QStringLiteral("擦除");
        b.device = QStringLiteral("COM3");
        b.success = false;
        b.durationMs = 500;                                          // → 0.50

        const QString csv = kc::diag::recordsToCsv({ a, b });
        expect(csv.startsWith(QStringLiteral("时间,操作,设备,来源包,分区,结果,耗时(秒),消息\n")),
               "csv-header", csv.left(80).toStdString());
        expect(csv.contains(QStringLiteral("2026-10-29 09:05:03")), "csv-time-format");
        expect(csv.contains(QStringLiteral("\"a,b.zip\"")), "csv-escape-comma");
        expect(csv.contains(QStringLiteral("\"他说\"\"OK\"\"\"")), "csv-escape-quote");
        expect(csv.contains(QStringLiteral("\"第一行\n第二行\"")), "csv-escape-newline");
        expect(csv.contains(QStringLiteral(",成功,12.34,")), "csv-success-and-duration",
               csv.toStdString());
        expect(csv.contains(QStringLiteral(",失败,0.50,")), "csv-failure-and-duration",
               csv.toStdString());
        // 表头 1 + 两行各 1 + a.message 里的换行 1 = 4。
        expect(csv.count(QLatin1Char('\n')) == 4, "csv-line-count",
               std::to_string(csv.count(QLatin1Char('\n'))));
    }

    // ── FlashHistory：append → 最新在前 → 重新读盘一致 ──────────────────
    {
        QTemporaryDir tmp;
        expect(tmp.isValid(), "history-tmpdir");
        kc::diag::FlashHistory::setDirForTest(tmp.path());

        kc::diag::FlashHistory& history = kc::diag::FlashHistory::instance();
        expect(history.records().isEmpty(), "history-starts-empty");

        const QDateTime base(QDate(2026, 10, 29), QTime(10, 0, 0));
        const bool a1 = history.append(makeRecord(base, QStringLiteral("写入"), true, 1000));
        const bool a2 = history.append(makeRecord(base.addSecs(60), QStringLiteral("擦除"), false, 2000));
        const bool a3 = history.append(makeRecord(base.addSecs(120), QStringLiteral("XML 刷机"), true, 3000));
        expect(a1 && a2 && a3, "history-append-ok");

        const QVector<kc::diag::FlashRecord> live = history.records();
        expect(live.size() == 3, "history-count", std::to_string(live.size()));
        expect(live.size() == 3 && live[0].operation == QStringLiteral("XML 刷机")
                   && live[1].operation == QStringLiteral("擦除")
                   && live[2].operation == QStringLiteral("写入"),
               "history-newest-first");

        const QString path = history.filePath();
        expect(QFile::exists(path), "history-file-exists", path.toStdString());
        const QByteArray content = readAll(path);
        expect(content.count('\n') == 3, "history-file-line-count",
               std::to_string(content.count('\n')));
        // 文件按追加顺序（旧 → 新）落盘，第一行是最老的。
        const QList<QByteArray> lines = content.split('\n');
        expect(lines.size() >= 1 && lines[0].contains("\"写入\""), "history-file-oldest-first",
               lines.isEmpty() ? "" : lines[0].toStdString());

        // 「新进程视角」：复位单例的加载状态后重读磁盘，内容必须与内存一致。
        kc::diag::FlashHistory::setDirForTest(tmp.path());
        const QVector<kc::diag::FlashRecord> reloaded = history.records();
        expect(reloaded.size() == live.size(), "history-reload-count");
        bool same = reloaded.size() == live.size();
        for (int i = 0; same && i < reloaded.size(); ++i) {
            same = reloaded[i].time == live[i].time
                   && reloaded[i].operation == live[i].operation
                   && reloaded[i].device == live[i].device
                   && reloaded[i].package == live[i].package
                   && reloaded[i].partitions == live[i].partitions
                   && reloaded[i].success == live[i].success
                   && reloaded[i].message == live[i].message
                   && reloaded[i].durationMs == live[i].durationMs;
        }
        expect(same, "history-reload-identical");

        // 坏行混进文件里：读的时候跳过，不报错不崩。
        {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Append)) {
                f.write("这不是 JSON\n{\n\n");
            }
        }
        kc::diag::FlashHistory::setDirForTest(tmp.path());
        expect(history.records().size() == 3, "history-skips-bad-lines",
               std::to_string(history.records().size()));
    }

    // ── 超过 MaxRecords：只留最近 500 条（文件也裁） ─────────────────────
    {
        QTemporaryDir tmp;
        expect(tmp.isValid(), "cap-tmpdir");
        kc::diag::FlashHistory::setDirForTest(tmp.path());

        kc::diag::FlashHistory& history = kc::diag::FlashHistory::instance();
        const QDateTime base(QDate(2026, 10, 29), QTime(0, 0, 0));
        const int total = kc::diag::FlashHistory::MaxRecords + 1;   // 501
        for (int i = 0; i < total; ++i) {
            history.append(makeRecord(base.addSecs(i), QStringLiteral("写入"), true, i));
        }

        const QVector<kc::diag::FlashRecord> capped = history.records();
        expect(capped.size() == kc::diag::FlashHistory::MaxRecords, "cap-memory-size",
               std::to_string(capped.size()));
        // 最新的在前 → 第 0 条是最后 append 的（durationMs == 500），最旧的是第 1 条。
        expect(!capped.isEmpty() && capped[0].durationMs == 500, "cap-keeps-newest",
               capped.isEmpty() ? "" : std::to_string(capped[0].durationMs));
        expect(!capped.isEmpty()
                   && capped[capped.size() - 1].durationMs == 1,
               "cap-drops-oldest", capped.isEmpty() ? "" : std::to_string(capped.back().durationMs));

        // 文件也被裁到 500 行（重写后不带旧内容）。
        const QByteArray content = readAll(history.filePath());
        expect(content.count('\n') == kc::diag::FlashHistory::MaxRecords, "cap-file-rewritten",
               std::to_string(content.count('\n')));
    }

    // ── clear：内存空、文件没了 ────────────────────────────────────────
    {
        QTemporaryDir tmp;
        expect(tmp.isValid(), "clear-tmpdir");
        kc::diag::FlashHistory::setDirForTest(tmp.path());

        kc::diag::FlashHistory& history = kc::diag::FlashHistory::instance();
        history.append(makeRecord(QDateTime::currentDateTime(), QStringLiteral("读取"), true, 10));
        expect(!history.records().isEmpty(), "clear-precondition-nonempty");
        const QString path = history.filePath();
        expect(QFile::exists(path), "clear-precondition-file");

        history.clear();
        expect(history.records().isEmpty(), "clear-memory-empty");
        expect(!QFile::exists(path), "clear-file-removed", path.toStdString());
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " FAILURE(S)\n";
    return 1;
}
