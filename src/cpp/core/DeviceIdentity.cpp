#include "core/DeviceIdentity.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>

namespace swt::edl {
namespace {

QString chipCodeFromText(const QString& text)
{
    static const QRegularExpression pattern(
        QStringLiteral("(?:^|[^A-Z0-9])((?:SM|MSM|SDM|APQ)[\\s_-]?[0-9]{4,5})(?=$|[^0-9])"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) return {};
    QString result = match.captured(1).toUpper();
    result.remove(QRegularExpression(QStringLiteral("[\\s_-]")));
    return result;
}

quint32 normalizedMsmId(const QString& text, bool* ok)
{
    QString value = text.trimmed();
    if (value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) value.remove(0, 2);
    const quint64 parsed = value.toULongLong(ok, 16);
    // Sahara V1/V2 usually prefixes the 24-bit MSM_ID with 00. Some devices
    // return a wider value, while the public EDL tables key on the low 24 bits.
    return static_cast<quint32>(parsed & 0x00ffffffULL);
}

} // namespace

QString normalizeQualcommChipName(const QString& text)
{
    return chipCodeFromText(text);
}

QString qualcommChipFromMsmId(const QString& msmId)
{
    bool ok = false;
    const quint32 id = normalizedMsmId(msmId, &ok);
    if (!ok) return {};

    // Values follow the public bkerler/edl Qualcomm msmids table. Sahara V3's
    // SM8550 identifier is documented by the same project as 0x28c0e1.
    static const QHash<quint32, QString> names{
        {0x08b0e1, QStringLiteral("SDM845")},
        {0x0a50e1, QStringLiteral("SM8150")},
        {0x0a60e1, QStringLiteral("SM8150")},
        {0x0c30e1, QStringLiteral("SM8250")},
        {0x0ce0e1, QStringLiteral("SM8250")},
        {0x1560e1, QStringLiteral("SM8250")},
        {0x11e0e1, QStringLiteral("SM7250")},
        {0x1430e1, QStringLiteral("SM7250")},
        {0x12a0e1, QStringLiteral("SM7125")},
        {0x12b0e1, QStringLiteral("SM7125")},
        {0x1350e1, QStringLiteral("SM8350")},
        {0x1420e1, QStringLiteral("SM8350")},
        {0x1520e1, QStringLiteral("SM8350")},
        {0x19e0e1, QStringLiteral("SM8350")},
        {0x28c0e1, QStringLiteral("SM8550")}
    };
    return names.value(id);
}

QString qualcommChipDisplayName(const QString& chipName)
{
    const QString chip = normalizeQualcommChipName(chipName);
    if (chip.isEmpty()) return chipName.trimmed();
    static const QHash<QString, QString> names{
        {QStringLiteral("SDM845"), QStringLiteral("SDM845 (Snapdragon 845)")},
        {QStringLiteral("SM7125"), QStringLiteral("SM7125 (Snapdragon 720G)")},
        {QStringLiteral("SM7250"), QStringLiteral("SM7250 (Snapdragon 765G)")},
        {QStringLiteral("SM8150"), QStringLiteral("SM8150 (Snapdragon 855)")},
        {QStringLiteral("SM8250"), QStringLiteral("SM8250 (Snapdragon 865)")},
        {QStringLiteral("SM8350"), QStringLiteral("SM8350 (Snapdragon 888)")},
        {QStringLiteral("SM8450"), QStringLiteral("SM8450 (Snapdragon 8 Gen 1)")},
        {QStringLiteral("SM8475"), QStringLiteral("SM8475 (Snapdragon 8+ Gen 1)")},
        {QStringLiteral("SM8550"), QStringLiteral("SM8550 (Snapdragon 8 Gen 2)")},
        {QStringLiteral("SM8650"), QStringLiteral("SM8650 (Snapdragon 8 Gen 3)")},
        {QStringLiteral("SM8750"), QStringLiteral("SM8750 (Snapdragon 8 Elite)")}
    };
    return names.value(chip, chip);
}

QString detectProgrammerChip(const QString& programmerPath, const QStringList& pathHints)
{
    for (const QString& hint : pathHints) {
        const QString chip = chipCodeFromText(hint);
        if (!chip.isEmpty()) return chip;
    }
    const QString pathChip = chipCodeFromText(programmerPath);
    if (!pathChip.isEmpty()) return pathChip;

    QFile programmer(programmerPath);
    if (!programmer.open(QIODevice::ReadOnly)) return {};
    // Firehose programmers are normally a few MiB. Eight MiB covers their ELF
    // metadata while keeping the UI-side package inspection bounded.
    const QString binaryText = QString::fromLatin1(programmer.read(8 * 1024 * 1024));
    static const QRegularExpression pattern(
        QStringLiteral("(?:^|[^A-Z0-9])((?:SM|MSM|SDM|APQ)[\\s_-]?[0-9]{4,5})(?=$|[^0-9])"),
        QRegularExpression::CaseInsensitiveOption);

    QHash<QString, int> counts;
    QString first;
    QRegularExpressionMatchIterator matches = pattern.globalMatch(binaryText);
    while (matches.hasNext()) {
        QString chip = matches.next().captured(1).toUpper();
        chip.remove(QRegularExpression(QStringLiteral("[\\s_-]")));
        if (first.isEmpty()) first = chip;
        ++counts[chip];
    }
    QString best = first;
    int bestCount = counts.value(best);
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        if (it.value() > bestCount) {
            best = it.key();
            bestCount = it.value();
        }
    }
    return best;
}

} // namespace swt::edl
