#include "kc/diag/LogCollector.h"

#include "kc/diag/SessionLog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace kc::diag {
namespace {

constexpr int kMaxNoteLength = 500;

} // namespace

QVector<LogFileItem> collectRecentLogs(const QString& dir, int maxFiles, qint64 maxTotalBytes)
{
    QVector<LogFileItem> out;
    if (maxFiles <= 0 || maxTotalBytes <= 0) return out;

    const QString d = dir.isEmpty() ? SessionLog::logDir() : dir;
    const QDir qdir(d);
    if (!qdir.exists()) return out;

    // 文件名即日期（session-YYYYMMDD.log）→ Name 排序 = 时间升序。
    const QFileInfoList infos =
        qdir.entryInfoList({ QStringLiteral("session-*.log") }, QDir::Files, QDir::Name);

    qint64 total = 0;
    for (int i = infos.size() - 1; i >= 0 && out.size() < maxFiles; --i) {
        const QFileInfo& fi = infos.at(i);
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray content = f.readAll();
        if (content.isEmpty()) continue;
        // 已经装了更旧的文件就不再超预算塞新的：宁缺勿滥（保新丢旧的另一半）。
        if (!out.isEmpty() && total + content.size() > maxTotalBytes) break;
        total += content.size();
        // 头部插入 → 最终顺序仍是时间升序（旧 → 新），开发者按顺序看。
        out.prepend({ fi.fileName(), content });
    }
    return out;
}

QJsonObject buildPayload(const QVector<LogFileItem>& files, const QString& build,
                         const QJsonObject& meta, const QString& note)
{
    QJsonObject root;
    root.insert(QStringLiteral("build"), build.left(40));
    root.insert(QStringLiteral("note"), note.left(kMaxNoteLength));
    root.insert(QStringLiteral("meta"), meta);

    QJsonArray arr;
    for (const LogFileItem& item : files) {
        QJsonObject fo;
        fo.insert(QStringLiteral("name"), item.name.left(64));
        fo.insert(QStringLiteral("size"), item.content.size());
        fo.insert(QStringLiteral("content"),
                  QString::fromLatin1(item.content.toBase64()));
        arr.append(fo);
    }
    root.insert(QStringLiteral("files"), arr);
    return root;
}

} // namespace kc::diag
