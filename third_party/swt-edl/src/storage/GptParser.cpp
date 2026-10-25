#include "storage/GptParser.h"

#include <QByteArray>
#include <QUuid>
#include <QtEndian>

#include <algorithm>

namespace swt::edl {
namespace {

quint32 u32(QByteArrayView data, qsizetype offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.data() + offset));
}

quint64 u64(QByteArrayView data, qsizetype offset)
{
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(data.data() + offset));
}

bool allZero(QByteArrayView bytes)
{
    for (const char byte : bytes) {
        if (byte != 0) return false;
    }
    return true;
}

QString gptGuid(QByteArrayView bytes)
{
    if (bytes.size() != 16) return {};
    QByteArray rfc(bytes.data(), bytes.size());
    std::reverse(rfc.begin(), rfc.begin() + 4);
    std::reverse(rfc.begin() + 4, rfc.begin() + 6);
    std::reverse(rfc.begin() + 6, rfc.begin() + 8);
    return QUuid::fromRfc4122(rfc).toString(QUuid::WithoutBraces).toUpper();
}

QString utf16Name(QByteArrayView bytes)
{
    QString name;
    name.reserve(bytes.size() / 2);
    for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) {
        const quint16 value = qFromLittleEndian<quint16>(
            reinterpret_cast<const uchar*>(bytes.data() + i));
        if (value == 0) break;
        name.append(QChar(value));
    }
    return name.trimmed();
}

} // namespace

bool GptParser::parse(QByteArrayView data, quint32 sectorSize, quint32 lun,
                      QVector<PartitionInfo>* partitions, QString* error)
{
    if (!partitions || sectorSize < 512 || data.size() < static_cast<qsizetype>(sectorSize * 2ULL)) {
        if (error) *error = QStringLiteral("GPT 数据长度不足");
        return false;
    }

    const qsizetype header = sectorSize;
    if (data.sliced(header, 8) != QByteArrayView("EFI PART", 8)) {
        if (error) *error = QStringLiteral("未找到 GPT 头签名");
        return false;
    }

    const quint32 headerSize = u32(data, header + 12);
    const quint64 entryLba = u64(data, header + 72);
    const quint32 entryCount = u32(data, header + 80);
    const quint32 entrySize = u32(data, header + 84);
    if (headerSize < 92 || headerSize > sectorSize || entrySize < 128 || entrySize > 4096 ||
        entryCount == 0 || entryCount > 4096) {
        if (error) *error = QStringLiteral("GPT 头字段异常");
        return false;
    }

    const quint64 tableOffset = entryLba * sectorSize;
    const quint64 tableBytes = static_cast<quint64>(entryCount) * entrySize;
    if (tableOffset > static_cast<quint64>(data.size()) ||
        tableBytes > static_cast<quint64>(data.size()) - tableOffset) {
        if (error) *error = QStringLiteral("GPT 分区项超出已读取范围");
        return false;
    }

    partitions->clear();
    for (quint32 index = 0; index < entryCount; ++index) {
        const qsizetype offset = static_cast<qsizetype>(tableOffset + static_cast<quint64>(index) * entrySize);
        const QByteArrayView entry = data.sliced(offset, entrySize);
        if (allZero(entry.first(16))) continue;

        PartitionInfo partition;
        partition.typeGuid = gptGuid(entry.sliced(0, 16));
        partition.uniqueGuid = gptGuid(entry.sliced(16, 16));
        partition.firstLba = u64(entry, 32);
        partition.lastLba = u64(entry, 40);
        partition.attributes = u64(entry, 48);
        partition.name = utf16Name(entry.sliced(56, qMin<qsizetype>(72, entrySize - 56)));
        partition.lun = lun;
        if (partition.lastLba >= partition.firstLba) partitions->push_back(std::move(partition));
    }

    if (partitions->isEmpty()) {
        if (error) *error = QStringLiteral("GPT 中没有有效分区项");
        return false;
    }
    return true;
}

} // namespace swt::edl
