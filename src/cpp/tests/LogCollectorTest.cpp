// 日志采集单测：「上传日志给开发者」的纯逻辑层。
// 覆盖：落盘格式与按日切割、采集的保新丢旧裁剪、上报载荷结构（base64/截断）。
// 断言全部基于**实际文件内容**，不凭印象。

#include "kc/diag/LogCollector.h"
#include "kc/diag/SessionLog.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVector>

#include <iostream>

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

void writeFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(content);
}

QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return f.readAll();
}

} // namespace

int main()
{
    // ── SessionLog：落盘一行，格式 = [时间] [级别] 正文 ──────────────────
    {
        QTemporaryDir tmp;
        expect(tmp.isValid(), "sessionlog-tmpdir");
        kc::diag::SessionLog::setDirForTest(tmp.path());
        // 重新构造单例前的静态目录已定：直接走 instance() 首次 append 的开文件逻辑。
        // 注意单例只能初始化一次，这里只验证「能写、格式对」。
        const QDateTime t(QDate(2026, 10, 12), QTime(18, 55, 1));
        kc::diag::SessionLog::instance().append(t, QStringLiteral("INFO"),
                                                QStringLiteral("hello 世界"));
        const QString expectPath =
            QDir(tmp.path()).filePath(QStringLiteral("session-20261012.log"));
        expect(QFile::exists(expectPath), "sessionlog-file-per-day", expectPath.toStdString());
        const QByteArray content = readAll(expectPath);
        expect(content.contains("[2026-10-12 18:55:01] [INFO] hello"),
               "sessionlog-line-format", content.toStdString());
        expect(content.contains("==== KunClient"), "sessionlog-header",
               content.toStdString());
    }

    // ── collectRecentLogs：保新丢旧 + 文件数/总量双上限 ───────────────────
    {
        QTemporaryDir tmp;
        QVector<QByteArray> sizes;
        // 三天日志：老大中，各 100 字节（内容互不相同，便于核对选了谁）。
        writeFile(QDir(tmp.path()).filePath(QStringLiteral("session-20261010.log")),
                  QByteArray(100, 'a'));
        writeFile(QDir(tmp.path()).filePath(QStringLiteral("session-20261011.log")),
                  QByteArray(100, 'b'));
        writeFile(QDir(tmp.path()).filePath(QStringLiteral("session-20261012.log")),
                  QByteArray(100, 'c'));

        // 只取 2 个 → 应该拿到 11/12 两天（丢最老的 10 号）。
        const QVector<kc::diag::LogFileItem> two =
            kc::diag::collectRecentLogs(tmp.path(), 2, 1024 * 1024);
        expect(two.size() == 2, "collect-max-files");
        expect(two.size() == 2 && two[0].name == QStringLiteral("session-20261011.log")
                   && two[1].name == QStringLiteral("session-20261012.log"),
               "collect-keeps-newest", two.empty() ? "" : two.front().name.toStdString());
        expect(two.size() == 2 && two[0].content == QByteArray(100, 'b'),
               "collect-content-ascending-order");

        // 总量压到 150 字节 → 只装得下 12 号一天（100B），再装 11 号就超 → break。
        const QVector<kc::diag::LogFileItem> capped =
            kc::diag::collectRecentLogs(tmp.path(), 5, 150);
        expect(capped.size() == 1 && capped[0].name == QStringLiteral("session-20261012.log"),
               "collect-total-bytes-cap");

        // 目录不存在 → 空，不崩。
        const QVector<kc::diag::LogFileItem> none =
            kc::diag::collectRecentLogs(tmp.path() + "/no-such-dir", 5, 1024);
        expect(none.isEmpty(), "collect-missing-dir-empty");
    }

    // ── buildPayload：结构 + base64 + 截断 ──────────────────────────────
    {
        const QVector<kc::diag::LogFileItem> files{
            { QStringLiteral("session-20261012.log"), QByteArray("line1\nline2\n") },
        };
        QJsonObject meta;
        meta.insert(QStringLiteral("clientVersion"), QStringLiteral("9.9.9"));
        const QString longNote = QString(600, 'x');
        const QJsonObject payload =
            kc::diag::buildPayload(files, QStringLiteral("2.0.0.2"), meta, longNote);

        expect(payload.value(QStringLiteral("build")).toString() == QStringLiteral("2.0.0.2"),
               "payload-build");
        // note 截到 500 字。
        expect(payload.value(QStringLiteral("note")).toString().size() == 500,
               "payload-note-truncated-500");
        expect(payload.value(QStringLiteral("meta")).toObject()
                   .value(QStringLiteral("clientVersion")).toString()
                   == QStringLiteral("9.9.9"),
               "payload-meta-passthrough");

        const QJsonArray arr = payload.value(QStringLiteral("files")).toArray();
        expect(arr.size() == 1, "payload-files-count");
        if (arr.size() == 1) {
            const QJsonObject fo = arr[0].toObject();
            expect(fo.value(QStringLiteral("name")).toString()
                       == QStringLiteral("session-20261012.log"),
                   "payload-file-name");
            expect(fo.value(QStringLiteral("size")).toInt() == 12, "payload-file-size");
            const QByteArray decoded =
                QByteArray::fromBase64(fo.value(QStringLiteral("content")).toString().toLatin1());
            expect(decoded == QByteArray("line1\nline2\n"), "payload-file-base64-roundtrip");
        }
    }

    if (g_failures == 0) {
        std::cout << "ALL PASS\n";
        return 0;
    }
    std::cerr << g_failures << " FAILURE(S)\n";
    return 1;
}
