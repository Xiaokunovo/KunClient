#include "storage/LunCapacity.h"

#include <QJsonArray>
#include <QLatin1Char>
#include <QRegularExpression>
#include <QString>

#include <limits>

namespace swt::edl {
namespace {

/// storage_info 里的数值可能是 JSON 数字，也可能是带引号的十进制 / 0x 字符串。
/// 只接受「明确为正且不溢出」的值；0 表示「未知」，与「没有这个字段」同义。
bool parseStorageUnsigned(const QJsonValue& value, quint64* result)
{
    if (value.isString()) {
        QString text = value.toString().trimmed();
        while (text.endsWith(QLatin1Char('.'))) text.chop(1);
        bool ok = false;
        const int base = text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10;
        const quint64 parsed = text.toULongLong(&ok, base);
        if (ok && result) *result = parsed;
        return ok;
    }
    if (value.isDouble()) {
        const double raw = value.toDouble();
        // double 精确表示整数上限 2^53 ≈ 9.0e15；再往上既丢精度也不可能是一片
        // 真实存储的尺寸，直接拒。
        if (!(raw > 0.0) || raw > 9.0e15) return false;
        if (result) *result = static_cast<quint64>(raw);
        return true;
    }
    return false;
}

} // namespace

bool resolveSectorExpression(const QString& expression, quint64 diskSectors,
                             quint64* value)
{
    // 语义与 EdlWorker.cpp 匿名命名空间里的原版逐行一致（原样搬迁，零变化），
    // 仅把「diskSectors == 0 → false」这条 fail-closed 前提写进头文件注释。
    QString text = expression.trimmed();
    while (text.endsWith(QLatin1Char('.'))) text.chop(1);
    const QString token = QStringLiteral("NUM_DISK_SECTORS");
    if (!text.startsWith(token, Qt::CaseInsensitive) || diskSectors == 0) {
        return false;
    }
    QString remainder = text.mid(token.size()).trimmed();
    if (remainder.isEmpty()) {
        *value = diskSectors;
        return true;
    }
    const QChar operation = remainder.front();
    remainder.removeFirst();
    while (remainder.endsWith(QLatin1Char('.'))) remainder.chop(1);
    bool ok = false;
    const quint64 operand = remainder.trimmed().toULongLong(&ok);
    if (!ok) return false;
    if (operation == QLatin1Char('-') && operand <= diskSectors) {
        *value = diskSectors - operand;
        return true;
    }
    if (operation == QLatin1Char('+') &&
        operand <= std::numeric_limits<quint64>::max() - diskSectors) {
        *value = diskSectors + operand;
        return true;
    }
    return false;
}

QHash<quint32, quint64> parseLunSizes(const QJsonObject& storageInfo, int queryLun)
{
    QHash<quint32, quint64> sizes;

    // ① lunN_size / lunN_size 带下标字段：最可信，找到即收工。
    //    注意 "lun_size" 本身不匹配（正则要求至少一位数字）。
    static const QRegularExpression indexedKey(
        QStringLiteral("^lun[_]?(\\d+)_size$"),
        QRegularExpression::CaseInsensitiveOption);
    for (auto it = storageInfo.begin(); it != storageInfo.end(); ++it) {
        const QRegularExpressionMatch match = indexedKey.match(it.key());
        if (!match.hasMatch()) continue;
        quint64 bytes = 0;
        if (!parseStorageUnsigned(it.value(), &bytes) || bytes == 0) continue;
        bool indexOk = false;
        const quint32 lun = match.captured(1).toUInt(&indexOk);
        if (!indexOk) continue;
        sizes.insert(lun, bytes);
    }
    if (!sizes.isEmpty()) return sizes;

    // ② total_blocks × block_size：标量形态，只有知道在问哪个 LUN 才能归属。
    if (queryLun >= 0) {
        quint64 totalBlocks = 0;
        quint64 blockSize = 0;
        if (parseStorageUnsigned(storageInfo.value(QStringLiteral("total_blocks")),
                                 &totalBlocks) &&
            parseStorageUnsigned(storageInfo.value(QStringLiteral("block_size")),
                                 &blockSize) &&
            totalBlocks > 0 && blockSize > 0 &&
            totalBlocks <= std::numeric_limits<quint64>::max() / blockSize) {
            sizes.insert(static_cast<quint32>(queryLun), totalBlocks * blockSize);
        }
    }
    if (!sizes.isEmpty()) return sizes;

    // ③ lun_size：数组按下标（无需 queryLun），标量归属查询 LUN。
    const QJsonValue lunSize = storageInfo.value(QStringLiteral("lun_size"));
    if (lunSize.isArray()) {
        const QJsonArray array = lunSize.toArray();
        for (int index = 0; index < array.size(); ++index) {
            quint64 bytes = 0;
            if (!parseStorageUnsigned(array.at(index), &bytes) || bytes == 0) continue;
            sizes.insert(static_cast<quint32>(index), bytes);
        }
    } else if (queryLun >= 0) {
        quint64 bytes = 0;
        if (parseStorageUnsigned(lunSize, &bytes) && bytes > 0) {
            sizes.insert(static_cast<quint32>(queryLun), bytes);
        }
    }
    return sizes;
}

} // namespace swt::edl
