#include "kc/tools/DeviceReport.h"

#include "kc/tools/TextTable.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace kc::tools {

namespace {

QString humanBytes(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("-");
    const double kb = 1024.0;
    if (bytes < kb) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < kb * kb) return QStringLiteral("%1 KB").arg(bytes / kb, 0, 'f', 1);
    if (bytes < kb * kb * kb) return QStringLiteral("%1 MB").arg(bytes / (kb * kb), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(bytes / (kb * kb * kb), 0, 'f', 2);
}

QString nowIso()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

} // namespace

QString buildTextReport(const DeviceReportInput& input)
{
    QStringList lines;

    lines << QStringLiteral("KunClient 设备报告");
    lines << QStringLiteral("========================================");
    lines << QStringLiteral("生成时间: %1")
                 .arg(input.generatedAt.isEmpty() ? nowIso() : input.generatedAt);
    if (!input.appVersion.trimmed().isEmpty()) {
        lines << QStringLiteral("客户端  : KunClient %1").arg(input.appVersion);
    }
    lines << QStringLiteral("设备    : %1")
                 .arg(input.deviceSummary.trimmed().isEmpty()
                          ? QStringLiteral("（未连接）")
                          : input.deviceSummary);
    lines << QString();

    // ── 附加信息 ──────────────────────────────────────────────────────────
    if (!input.extraInfo.isEmpty()) {
        lines << QStringLiteral("── 附加信息 ──");
        // 用等宽对齐：键右对齐到固定宽度，值左对齐。
        for (const auto& kv : input.extraInfo) {
            lines << QStringLiteral("%1: %2").arg(kv.first, kv.second);
        }
        lines << QString();
    }

    // ── 分区表 ────────────────────────────────────────────────────────────
    if (!input.partitions.isEmpty()) {
        lines << QStringLiteral("── 分区表（%1 个）──").arg(input.partitions.size());

        // 复用 TextTable 的等宽对齐能力（它已处理 CJK 宽度）。
        QVector<QStringList> rows;
        qint64 total = 0;
        for (const auto& p : input.partitions) {
            rows.append({p.first, humanBytes(p.second)});
            total += p.second;
        }
        rows.append({QStringLiteral("合计"), humanBytes(total)});

        const QString table = buildPlainTable({QStringLiteral("分区名"), QStringLiteral("大小")}, rows);
        lines << table;
    } else {
        lines << QStringLiteral("（未读取分区表）");
    }

    lines << QString();
    lines << QStringLiteral("—— 由 KunClient 生成");

    return lines.join(QLatin1Char('\n'));
}

QByteArray buildJsonReport(const DeviceReportInput& input)
{
    QJsonObject root;
    root.insert(QStringLiteral("tool"), QStringLiteral("KunClient"));
    root.insert(QStringLiteral("schema"), 1);
    root.insert(QStringLiteral("generatedAt"),
                input.generatedAt.isEmpty() ? nowIso() : input.generatedAt);
    if (!input.appVersion.trimmed().isEmpty()) {
        root.insert(QStringLiteral("appVersion"), input.appVersion);
    }
    root.insert(QStringLiteral("device"), input.deviceSummary);

    QJsonObject extra;
    for (const auto& kv : input.extraInfo) extra.insert(kv.first, kv.second);
    root.insert(QStringLiteral("extra"), extra);

    QJsonArray parts;
    for (const auto& p : input.partitions) {
        QJsonObject o;
        o.insert(QStringLiteral("name"), p.first);
        // 🔴 用 double 存字节数：QJsonValue 没有 qint64 重载，
        //    分区大小远小于 2^53，double 精度足够。
        o.insert(QStringLiteral("sizeBytes"), static_cast<double>(p.second));
        parts.append(o);
    }
    root.insert(QStringLiteral("partitions"), parts);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QString suggestedReportFileName(const QString& suffix)
{
    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString tail = suffix.trimmed().isEmpty() ? QStringLiteral("txt") : suffix.trimmed();
    return QStringLiteral("KunClient-设备报告-%1.%2").arg(stamp, tail);
}

} // namespace kc::tools
