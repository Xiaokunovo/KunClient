#include "storage/PayloadParser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>

namespace swt::edl {
namespace {

constexpr quint64 kMaxManifestSize = 64LL * 1024 * 1024;
constexpr quint64 kMaxOperationBytes = 512LL * 1024 * 1024;

// ── 最小 protobuf 读取器（只支持 varint / length-delimited / 跳过）──────────
class ProtoReader {
public:
    ProtoReader(const char* data, qsizetype size) : m_data(data), m_size(size) {}

    bool atEnd() const { return m_pos >= m_size; }

    bool readVarint(quint64* value)
    {
        quint64 result = 0;
        int shift = 0;
        while (m_pos < m_size && shift <= 63) {
            const auto byte = static_cast<quint8>(m_data[m_pos++]);
            result |= static_cast<quint64>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                if (value) *value = result;
                return true;
            }
            shift += 7;
        }
        return false;
    }

    bool readTag(quint32* field, quint32* wire)
    {
        quint64 tag = 0;
        if (!readVarint(&tag)) return false;
        if (field) *field = static_cast<quint32>(tag >> 3);
        if (wire) *wire = static_cast<quint32>(tag & 0x7);
        return true;
    }

    /// 返回视图（不拷贝），生命周期与底层 buffer 相同。
    bool readLengthDelimited(const char** data, qsizetype* size)
    {
        quint64 length = 0;
        if (!readVarint(&length)) return false;
        if (length > static_cast<quint64>(m_size - m_pos)) return false;
        if (data) *data = m_data + m_pos;
        if (size) *size = static_cast<qsizetype>(length);
        m_pos += static_cast<qsizetype>(length);
        return true;
    }

    bool skip(quint32 wire)
    {
        switch (wire) {
        case 0: {
            quint64 ignored = 0;
            return readVarint(&ignored);
        }
        case 1:
            if (m_pos + 8 > m_size) return false;
            m_pos += 8;
            return true;
        case 2: {
            const char* ignoredData = nullptr;
            qsizetype ignoredSize = 0;
            return readLengthDelimited(&ignoredData, &ignoredSize);
        }
        case 5:
            if (m_pos + 4 > m_size) return false;
            m_pos += 4;
            return true;
        default:
            return false;
        }
    }

private:
    const char* m_data;
    qsizetype m_size;
    qsizetype m_pos = 0;
};

// ── manifest 结构 ─────────────────────────────────────────────────────────

struct ParsedOp {
    int type = -1;
    quint64 dataOffset = 0;
    quint64 dataLength = 0;
    QVector<QPair<quint64, quint64>> extents;   // (start_block, num_blocks)
};

struct ParsedPartition {
    QString name;
    quint64 size = 0;
    QVector<ParsedOp> ops;
};

bool parseExtent(const char* data, qsizetype size, quint64* startBlock, quint64* numBlocks)
{
    ProtoReader reader(data, size);
    while (!reader.atEnd()) {
        quint32 field = 0;
        quint32 wire = 0;
        if (!reader.readTag(&field, &wire)) return false;
        if (wire == 0 && field == 1) {
            if (!reader.readVarint(startBlock)) return false;
        } else if (wire == 0 && field == 2) {
            if (!reader.readVarint(numBlocks)) return false;
        } else if (!reader.skip(wire)) {
            return false;
        }
    }
    return true;
}

bool parseOperation(const char* data, qsizetype size, ParsedOp* op)
{
    ProtoReader reader(data, size);
    while (!reader.atEnd()) {
        quint32 field = 0;
        quint32 wire = 0;
        if (!reader.readTag(&field, &wire)) return false;
        if (wire == 0 && field == 1) {
            quint64 type = 0;
            if (!reader.readVarint(&type)) return false;
            op->type = static_cast<int>(type);
        } else if (wire == 0 && field == 2) {
            if (!reader.readVarint(&op->dataOffset)) return false;
        } else if (wire == 0 && field == 3) {
            if (!reader.readVarint(&op->dataLength)) return false;
        } else if (wire == 2 && field == 6) {
            const char* extentData = nullptr;
            qsizetype extentSize = 0;
            if (!reader.readLengthDelimited(&extentData, &extentSize)) return false;
            quint64 start = 0;
            quint64 count = 0;
            if (!parseExtent(extentData, extentSize, &start, &count)) return false;
            op->extents.append({ start, count });
        } else if (!reader.skip(wire)) {
            return false;
        }
    }
    return true;
}

bool parsePartitionInfoSize(const char* data, qsizetype size, quint64* partitionSize)
{
    ProtoReader reader(data, size);
    while (!reader.atEnd()) {
        quint32 field = 0;
        quint32 wire = 0;
        if (!reader.readTag(&field, &wire)) return false;
        if (wire == 0 && field == 1) {
            if (!reader.readVarint(partitionSize)) return false;
        } else if (!reader.skip(wire)) {
            return false;
        }
    }
    return true;
}

bool parsePartition(const char* data, qsizetype size, ParsedPartition* partition, QString* error)
{
    ProtoReader reader(data, size);
    while (!reader.atEnd()) {
        quint32 field = 0;
        quint32 wire = 0;
        if (!reader.readTag(&field, &wire)) {
            if (error) *error = QStringLiteral("payload manifest 里的分区结构损坏。");
            return false;
        }
        if (wire == 2 && field == 1) {
            const char* nameData = nullptr;
            qsizetype nameSize = 0;
            if (!reader.readLengthDelimited(&nameData, &nameSize)) return false;
            partition->name = QString::fromUtf8(nameData, nameSize);
        } else if (wire == 2 && field == 7) {
            const char* infoData = nullptr;
            qsizetype infoSize = 0;
            if (!reader.readLengthDelimited(&infoData, &infoSize)) return false;
            parsePartitionInfoSize(infoData, infoSize, &partition->size);
        } else if (wire == 2 && field == 8) {
            const char* opData = nullptr;
            qsizetype opSize = 0;
            if (!reader.readLengthDelimited(&opData, &opSize)) return false;
            ParsedOp op;
            if (!parseOperation(opData, opSize, &op)) {
                if (error) *error = QStringLiteral("payload manifest 里的操作结构损坏。");
                return false;
            }
            partition->ops.append(op);
        } else if (!reader.skip(wire)) {
            if (error) *error = QStringLiteral("payload manifest 字段无法解析（wire 类型异常）。");
            return false;
        }
    }
    return true;
}

bool parseManifest(const QByteArray& manifest, QVector<ParsedPartition>* partitions,
                   QString* error)
{
    ProtoReader reader(manifest.constData(), manifest.size());
    while (!reader.atEnd()) {
        quint32 field = 0;
        quint32 wire = 0;
        if (!reader.readTag(&field, &wire)) {
            if (error) *error = QStringLiteral("payload manifest 头部损坏。");
            return false;
        }
        if (wire == 2 && field == 13) {
            const char* data = nullptr;
            qsizetype size = 0;
            if (!reader.readLengthDelimited(&data, &size)) return false;
            ParsedPartition partition;
            if (!parsePartition(data, size, &partition, error)) return false;
            if (!partition.name.isEmpty()) partitions->append(partition);
        } else if (!reader.skip(wire)) {
            if (error) *error = QStringLiteral("payload manifest 字段无法解析（wire 类型异常）。");
            return false;
        }
    }
    return true;
}

/// 读 CrAU 头 + manifest。成功时 *dataOffset 是操作数据区起点。
bool readHeaderAndManifest(QFile& file, QByteArray* manifest, quint64* dataOffset,
                           QString* error)
{
    if (file.size() < 24) {
        if (error) *error = QStringLiteral("文件太小，不是有效的 payload.bin。");
        return false;
    }
    if (!file.seek(0)) return false;
    const QByteArray header = file.read(24);
    if (header.size() != 24) {
        if (error) *error = QStringLiteral("读取 payload 头失败。");
        return false;
    }
    if (header.left(4) != QByteArray("CrAU")) {
        if (error) *error = QStringLiteral("不是 CrAU 格式的 payload.bin。");
        return false;
    }

    const quint64 version = qFromBigEndian<quint64>(header.constData() + 4);
    const quint64 manifestSize = qFromBigEndian<quint64>(header.constData() + 12);
    quint64 signatureSize = 0;
    if (version >= 2) signatureSize = qFromBigEndian<quint32>(header.constData() + 20);

    if (version != 1 && version != 2) {
        if (error) *error = QStringLiteral("不支持的 payload 版本: %1").arg(version);
        return false;
    }
    if (manifestSize == 0 || manifestSize > kMaxManifestSize
        || 24 + manifestSize > static_cast<quint64>(file.size())) {
        if (error) *error = QStringLiteral("payload manifest 长度异常: %1").arg(manifestSize);
        return false;
    }

    if (!file.seek(24)) return false;
    *manifest = file.read(static_cast<qint64>(manifestSize));
    if (static_cast<quint64>(manifest->size()) != manifestSize) {
        if (error) *error = QStringLiteral("payload manifest 读取不完整。");
        return false;
    }
    *dataOffset = 24 + manifestSize + signatureSize;
    return true;
}

// ── 解压 ──────────────────────────────────────────────────────────────────

QByteArray decompressBz2(const QByteArray& input, quint64 expected, QString* error)
{
    if (expected == 0 || expected > kMaxOperationBytes) {
        if (error) *error = QStringLiteral("bzip2 段长度异常: %1").arg(expected);
        return {};
    }
    QByteArray output(static_cast<qsizetype>(expected), '\0');
    unsigned int destinationLength = static_cast<unsigned int>(expected);
    const int rc = BZ2_bzBuffToBuffDecompress(output.data(), &destinationLength,
                                              const_cast<char*>(input.constData()),
                                              static_cast<unsigned int>(input.size()), 0, 0);
    if (rc != BZ_OK) {
        if (error) *error = QStringLiteral("bzip2 解压失败（错误码 %1）").arg(rc);
        return {};
    }
    output.resize(static_cast<qsizetype>(destinationLength));
    return output;
}

QByteArray decompressXz(const QByteArray& input, quint64 expected, QString* error)
{
    if (expected == 0 || expected > kMaxOperationBytes) {
        if (error) *error = QStringLiteral("xz 段长度异常: %1").arg(expected);
        return {};
    }
    QByteArray output(static_cast<qsizetype>(expected), '\0');
    quint64 memoryLimit = UINT64_MAX;
    size_t inputPosition = 0;
    size_t outputPosition = 0;
    const lzma_ret rc = lzma_stream_buffer_decode(
        &memoryLimit, 0, nullptr,
        reinterpret_cast<const uint8_t*>(input.constData()), &inputPosition,
        static_cast<size_t>(input.size()),
        reinterpret_cast<uint8_t*>(output.data()), &outputPosition,
        static_cast<size_t>(expected));
    if (rc != LZMA_OK) {
        if (error) *error = QStringLiteral("xz 解压失败（错误码 %1）").arg(static_cast<int>(rc));
        return {};
    }
    output.resize(static_cast<qsizetype>(outputPosition));
    return output;
}

QByteArray decompressZstd(const QByteArray& input, quint64 expected, QString* error)
{
    if (expected == 0 || expected > kMaxOperationBytes) {
        if (error) *error = QStringLiteral("zstd 段长度异常: %1").arg(expected);
        return {};
    }
    QByteArray output(static_cast<qsizetype>(expected), '\0');
    const size_t rc = ZSTD_decompress(output.data(), static_cast<size_t>(expected),
                                      input.constData(), static_cast<size_t>(input.size()));
    if (ZSTD_isError(rc)) {
        if (error) *error = QStringLiteral("zstd 解压失败: %1")
                                .arg(QString::fromLatin1(ZSTD_getErrorName(rc)));
        return {};
    }
    output.resize(static_cast<qsizetype>(rc));
    return output;
}

quint64 extentsBytes(const QVector<QPair<quint64, quint64>>& extents)
{
    quint64 total = 0;
    for (const auto& extent : extents) total += extent.second * PayloadParser::BlockSize;
    return total;
}

bool writeAcrossExtents(QFile& output, const QVector<QPair<quint64, quint64>>& extents,
                        const QByteArray& data, const QString& partition, QString* error)
{
    qsizetype offset = 0;
    for (const auto& extent : extents) {
        const qint64 bytes = static_cast<qint64>(extent.second * PayloadParser::BlockSize);
        if (offset + bytes > data.size()) {
            if (error) {
                *error = QStringLiteral("%1: 解出的数据比目标区间短（%2 < %3）")
                             .arg(partition)
                             .arg(data.size())
                             .arg(offset + bytes);
            }
            return false;
        }
        if (!output.seek(static_cast<qint64>(extent.first * PayloadParser::BlockSize))) {
            if (error) *error = QStringLiteral("%1: 输出文件定位失败。").arg(partition);
            return false;
        }
        if (output.write(data.constData() + offset, bytes) != bytes) {
            if (error) *error = QStringLiteral("%1: 写入失败 %2").arg(partition,
                                                                      output.errorString());
            return false;
        }
        offset += bytes;
    }
    return true;
}

} // namespace

QString PayloadParser::operationTypeName(int type)
{
    switch (type) {
    case 0: return QStringLiteral("REPLACE");
    case 1: return QStringLiteral("REPLACE_BZ");
    case 2: return QStringLiteral("MOVE");
    case 3: return QStringLiteral("BSDIFF");
    case 4: return QStringLiteral("SOURCE_COPY");
    case 5: return QStringLiteral("SOURCE_BSDIFF");
    case 6: return QStringLiteral("ZERO");
    case 7: return QStringLiteral("DISCARD");
    case 8: return QStringLiteral("REPLACE_XZ");
    case 9: return QStringLiteral("PUFFDIFF");
    case 10: return QStringLiteral("BROTLI_BSDIFF");
    case 11: return QStringLiteral("ZUCCHINI");
    case 12: return QStringLiteral("LZ4DIFF_BSDIFF");
    case 13: return QStringLiteral("LZ4DIFF_PUFFDIFF");
    case 14: return QStringLiteral("REPLACE_ZSTD");
    default: break;
    }
    return QStringLiteral("未知(%1)").arg(type);
}

bool PayloadParser::isCrAU(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray magic = file.read(4);
    return magic == QByteArray("CrAU");
}

bool PayloadParser::inspect(const QString& path, PayloadInfo* info, QString* error)
{
    if (info) *info = PayloadInfo{};
    if (error) error->clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 payload: %1").arg(file.errorString());
        return false;
    }

    QByteArray manifest;
    quint64 dataOffset = 0;
    if (!readHeaderAndManifest(file, &manifest, &dataOffset, error)) return false;

    QVector<ParsedPartition> partitions;
    if (!parseManifest(manifest, &partitions, error)) return false;
    if (partitions.isEmpty()) {
        if (error) *error = QStringLiteral("payload manifest 里没有任何分区。");
        return false;
    }

    if (info) {
        info->fileSize = static_cast<quint64>(file.size());
        info->manifestSize = static_cast<quint64>(manifest.size());
        info->dataOffset = dataOffset;
        for (const ParsedPartition& partition : partitions) {
            PayloadPartitionInfo entry;
            entry.name = partition.name;
            entry.operationCount = partition.ops.size();
            quint64 extentTotal = 0;
            for (const ParsedOp& op : partition.ops) {
                entry.dataBytes += op.dataLength;
                if (op.extents.isEmpty()) continue;
                const auto& last = op.extents.last();
                const quint64 end = (last.first + last.second) * BlockSize;
                extentTotal = std::max(extentTotal, end);
            }
            entry.size = partition.size > 0 ? partition.size : extentTotal;
            info->partitions.append(entry);
        }
    }
    return true;
}

bool PayloadParser::extract(const QString& path, const QString& outputDirectory,
                            const QStringList& partitions, QStringList* extracted,
                            QString* error,
                            const std::function<void(int, const QString&)>& progress,
                            const std::function<bool()>& cancel)
{
    if (error) error->clear();
    if (extracted) extracted->clear();
    if (outputDirectory.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("未指定输出目录。");
        return false;
    }
    if (!QDir().mkpath(outputDirectory)) {
        if (error) *error = QStringLiteral("无法创建输出目录: %1").arg(outputDirectory);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 payload: %1").arg(file.errorString());
        return false;
    }

    QByteArray manifest;
    quint64 dataOffset = 0;
    if (!readHeaderAndManifest(file, &manifest, &dataOffset, error)) return false;

    QVector<ParsedPartition> allPartitions;
    if (!parseManifest(manifest, &allPartitions, error)) return false;

    // 先把「请求的分区是否存在」校验完，再挑目标 —— 这样报错能指名道姓。
    if (!partitions.isEmpty()) {
        for (const QString& requested : partitions) {
            const bool found = std::any_of(allPartitions.cbegin(), allPartitions.cend(),
                                           [&requested](const ParsedPartition& partition) {
                                               return partition.name == requested;
                                           });
            if (!found) {
                if (error) *error = QStringLiteral("payload 里没有分区: %1").arg(requested);
                return false;
            }
        }
    }

    QVector<ParsedPartition> targets;
    for (const ParsedPartition& partition : allPartitions) {
        if (partitions.isEmpty() || partitions.contains(partition.name)) targets.append(partition);
    }
    if (targets.isEmpty()) {
        if (error) *error = QStringLiteral("没有匹配的分区可提取。");
        return false;
    }

    int totalOps = 0;
    for (const ParsedPartition& partition : targets) totalOps += partition.ops.size();
    int doneOps = 0;

    for (const ParsedPartition& partition : targets) {
        const QString outputPath =
            QDir(outputDirectory).filePath(partition.name + QStringLiteral(".img"));
        QFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("无法创建 %1: %2").arg(outputPath,
                                                                      output.errorString());
            return false;
        }

        quint64 partitionSize = partition.size;
        if (partitionSize == 0) {
            for (const ParsedOp& op : partition.ops) {
                for (const auto& extent : op.extents) {
                    partitionSize = std::max(partitionSize,
                                             (extent.first + extent.second) * BlockSize);
                }
            }
        }
        if (partitionSize > 0 && !output.resize(static_cast<qint64>(partitionSize))) {
            if (error) *error = QStringLiteral("无法调整 %1 大小: %2").arg(outputPath,
                                                                          output.errorString());
            return false;
        }

        for (const ParsedOp& op : partition.ops) {
            if (cancel && cancel()) {
                if (error) *error = QStringLiteral("已取消");
                return false;
            }

            const quint64 targetBytes = extentsBytes(op.extents);
            const QString label = QStringLiteral("%1 (%2)").arg(partition.name,
                                                                 operationTypeName(op.type));

            switch (op.type) {
            case 6: { // ZERO —— 显式写零：同一区间可能先被 REPLACE 写过
                static const QByteArray zeros(1 << 20, '\0');
                for (const auto& extent : op.extents) {
                    if (!output.seek(static_cast<qint64>(extent.first * BlockSize))) {
                        if (error) *error = QStringLiteral("%1: 输出文件定位失败。").arg(partition.name);
                        return false;
                    }
                    quint64 remaining = extent.second * BlockSize;
                    while (remaining > 0) {
                        const qint64 chunk =
                            static_cast<qint64>(std::min<quint64>(remaining, zeros.size()));
                        if (output.write(zeros.constData(), chunk) != chunk) {
                            if (error) *error = QStringLiteral("%1: 写零失败 %2")
                                                    .arg(partition.name, output.errorString());
                            return false;
                        }
                        remaining -= static_cast<quint64>(chunk);
                    }
                }
                break;
            }
            case 7:   // DISCARD —— 语义就是「可以丢弃」，留洞即可
                break;
            case 0: { // REPLACE
                if (op.dataLength < targetBytes) {
                    if (error) *error = QStringLiteral("%1: 数据长度不足 %2 < %3")
                                            .arg(partition.name)
                                            .arg(op.dataLength)
                                            .arg(targetBytes);
                    return false;
                }
                if (!file.seek(static_cast<qint64>(dataOffset + op.dataOffset))) {
                    if (error) *error = QStringLiteral("%1: payload 定位失败。").arg(partition.name);
                    return false;
                }
                const QByteArray data = file.read(static_cast<qint64>(targetBytes));
                if (static_cast<quint64>(data.size()) != targetBytes) {
                    if (error) *error = QStringLiteral("%1: 读取操作数据不完整。").arg(partition.name);
                    return false;
                }
                if (!writeAcrossExtents(output, op.extents, data, partition.name, error)) {
                    return false;
                }
                break;
            }
            case 1:
            case 8:
            case 14: {   // REPLACE_BZ / REPLACE_XZ / REPLACE_ZSTD
                if (op.dataLength == 0 || op.dataLength > kMaxOperationBytes) {
                    if (error) *error = QStringLiteral("%1: 压缩段长度异常 %2")
                                            .arg(partition.name)
                                            .arg(op.dataLength);
                    return false;
                }
                if (!file.seek(static_cast<qint64>(dataOffset + op.dataOffset))) {
                    if (error) *error = QStringLiteral("%1: payload 定位失败。").arg(partition.name);
                    return false;
                }
                const QByteArray compressed = file.read(static_cast<qint64>(op.dataLength));
                if (static_cast<quint64>(compressed.size()) != op.dataLength) {
                    if (error) *error = QStringLiteral("%1: 读取压缩段不完整。").arg(partition.name);
                    return false;
                }
                QByteArray data;
                if (op.type == 1) {
                    data = decompressBz2(compressed, targetBytes, error);
                } else if (op.type == 8) {
                    data = decompressXz(compressed, targetBytes, error);
                } else {
                    data = decompressZstd(compressed, targetBytes, error);
                }
                if (data.isNull() || data.isEmpty()) {
                    if (error && error->isEmpty()) {
                        *error = QStringLiteral("%1: 解压结果为空。").arg(partition.name);
                    }
                    return false;
                }
                if (!writeAcrossExtents(output, op.extents, data, partition.name, error)) {
                    return false;
                }
                break;
            }
            default:
                if (error) {
                    *error = QStringLiteral(
                        "%1: 该分区是增量包操作 %2，需要旧镜像参与，暂不支持。"
                        "请使用完整 OTA（full payload）。")
                                 .arg(partition.name)
                                 .arg(operationTypeName(op.type));
                }
                return false;
            }

            ++doneOps;
            if (progress) {
                progress(totalOps > 0 ? doneOps * 100 / totalOps : 100,
                         QStringLiteral("已写入 %1").arg(label));
            }
        }

        output.close();
        if (extracted) extracted->append(partition.name + QStringLiteral(".img"));
    }

    if (progress) progress(100, QStringLiteral("全部完成"));
    return true;
}

} // namespace swt::edl
