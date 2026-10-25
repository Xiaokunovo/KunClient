#include "storage/SparseImage.h"

#include <QFile>
#include <QtEndian>

#include <cstring>
#include <limits>

namespace swt::edl {
namespace {

template <typename T>
bool readLe(const QByteArray& bytes, qsizetype offset, T* value)
{
    if (!value || offset < 0 || offset + static_cast<qsizetype>(sizeof(T)) > bytes.size()) {
        return false;
    }
    T raw{};
    std::memcpy(&raw, bytes.constData() + offset, sizeof(T));
    *value = qFromLittleEndian(raw);
    return true;
}

bool readExact(QFile& file, qint64 offset, qint64 length, QByteArray* bytes)
{
    if (!file.seek(offset)) return false;
    *bytes = file.read(length);
    return bytes->size() == length;
}

/// 顺序缓冲读：chunk 头只有 12 字节，逐 chunk 一次 seek+read 在万级 chunk 的
/// 镜像上是纯 syscall 损耗。缓冲区按需前移，只在跨出缓冲范围时重新填充；
/// 大 RAW chunk 会把缓冲顶穿，此时退化为「每个大 chunk 一次读」，不会更差。
constexpr qsizetype ChunkBufferBytes = 64 * 1024;

bool bufferedRead(QFile& file, qint64 offset, qint64 length, QByteArray* bytes,
                  QByteArray* buffer, qint64* bufferOffset, qsizetype* bufferLength)
{
    if (offset < 0 || length < 0) return false;
    if (*bufferOffset < 0 || offset < *bufferOffset ||
        offset + length > *bufferOffset + static_cast<qint64>(*bufferLength)) {
        const qint64 want = qMax<qint64>(length, ChunkBufferBytes);
        if (!file.seek(offset)) return false;
        *buffer = file.read(want);
        *bufferOffset = offset;
        *bufferLength = buffer->size();
        if (*bufferLength < length) return false;
    }
    const qsizetype at = static_cast<qsizetype>(offset - *bufferOffset);
    *bytes = buffer->mid(at, static_cast<qsizetype>(length));
    return bytes->size() == length;
}

} // namespace

bool SparseImage::hasSparseMagic(QFile& file)
{
    const qint64 original = file.pos();
    QByteArray bytes;
    quint32 magic = 0;
    const bool result = readExact(file, 0, 4, &bytes) && readLe(bytes, 0, &magic) && magic == Magic;
    file.seek(original);
    return result;
}

bool SparseImage::inspect(QFile& file, SparseImageInfo* info, QString* error)
{
    if (!info || !file.isOpen()) {
        if (error) *error = QStringLiteral("稀疏镜像参数错误");
        return false;
    }
    *info = {};

    QByteArray header;
    if (!readExact(file, 0, 28, &header)) {
        if (error) *error = QStringLiteral("读取 Android sparse 文件头失败");
        return false;
    }
    quint32 magic = 0;
    quint16 major = 0;
    quint16 fileHeaderSize = 0;
    quint16 chunkHeaderSize = 0;
    quint32 blockSize = 0;
    quint32 totalBlocks = 0;
    quint32 totalChunks = 0;
    readLe(header, 0, &magic);
    readLe(header, 4, &major);
    readLe(header, 8, &fileHeaderSize);
    readLe(header, 10, &chunkHeaderSize);
    readLe(header, 12, &blockSize);
    readLe(header, 16, &totalBlocks);
    readLe(header, 20, &totalChunks);

    if (magic != Magic || major != 1 || fileHeaderSize < 28 || chunkHeaderSize < 12 ||
        blockSize == 0 || totalBlocks == 0) {
        if (error) *error = QStringLiteral("Android sparse 文件头无效");
        return false;
    }

    constexpr quint16 rawChunk = 0xCAC1;
    constexpr quint16 fillChunk = 0xCAC2;
    constexpr quint16 skipChunk = 0xCAC3;
    constexpr quint16 crcChunk = 0xCAC4;
    qint64 cursor = fileHeaderSize;
    quint64 outputBlock = 0;
    QByteArray buffer;
    qint64 bufferOffset = -1;
    qsizetype bufferLength = 0;

    for (quint32 index = 0; index < totalChunks; ++index) {
        QByteArray chunk;
        if (!bufferedRead(file, cursor, chunkHeaderSize, &chunk, &buffer,
                          &bufferOffset, &bufferLength)) {
            if (error) *error = QStringLiteral("读取 sparse chunk %1 失败").arg(index);
            return false;
        }
        quint16 type = 0;
        quint32 chunkBlocks = 0;
        quint32 totalSize = 0;
        readLe(chunk, 0, &type);
        readLe(chunk, 4, &chunkBlocks);
        readLe(chunk, 8, &totalSize);
        if (totalSize < chunkHeaderSize || cursor + totalSize > file.size()) {
            if (error) *error = QStringLiteral("sparse chunk %1 长度越界").arg(index);
            return false;
        }

        if (type == crcChunk) {
            if (chunkBlocks != 0 || totalSize != static_cast<quint32>(chunkHeaderSize) + 4U) {
                if (error) *error = QStringLiteral("sparse CRC chunk %1 长度或输出块数错误").arg(index);
                return false;
            }
            cursor += totalSize;
            continue;
        }
        if (chunkBlocks == 0 || outputBlock + chunkBlocks > totalBlocks) {
            if (error) *error = QStringLiteral("sparse chunk %1 输出范围错误").arg(index);
            return false;
        }

        SparseExtent extent;
        extent.outputBlock = outputBlock;
        extent.blockCount = chunkBlocks;
        if (type == rawChunk) {
            if (chunkBlocks > std::numeric_limits<quint64>::max() / blockSize ||
                totalSize != static_cast<quint64>(chunkHeaderSize) +
                                 static_cast<quint64>(chunkBlocks) * blockSize) {
                if (error) *error = QStringLiteral("sparse RAW chunk %1 长度错误").arg(index);
                return false;
            }
            extent.type = SparseExtentType::Raw;
            extent.fileOffset = static_cast<quint64>(cursor + chunkHeaderSize);
        } else if (type == fillChunk) {
            if (totalSize != static_cast<quint32>(chunkHeaderSize) + 4U) {
                if (error) *error = QStringLiteral("sparse FILL chunk %1 长度错误").arg(index);
                return false;
            }
            QByteArray fill;
            if (!bufferedRead(file, cursor + chunkHeaderSize, 4, &fill, &buffer,
                              &bufferOffset, &bufferLength) ||
                !readLe(fill, 0, &extent.fillValue)) {
                if (error) *error = QStringLiteral("读取 sparse FILL chunk %1 失败").arg(index);
                return false;
            }
            extent.type = SparseExtentType::Fill;
        } else if (type == skipChunk) {
            if (totalSize != chunkHeaderSize) {
                if (error) *error = QStringLiteral("sparse DONT_CARE chunk %1 长度错误").arg(index);
                return false;
            }
            extent.type = SparseExtentType::Skip;
        } else {
            if (error) *error = QStringLiteral("未知 sparse chunk 类型：0x%1")
                                    .arg(type, 4, 16, QLatin1Char('0'));
            return false;
        }
        info->extents.push_back(extent);
        outputBlock += chunkBlocks;
        cursor += totalSize;
    }

    if (outputBlock != totalBlocks) {
        if (error) *error = QStringLiteral("sparse 展开块数不匹配：%1 / %2")
                                .arg(outputBlock).arg(totalBlocks);
        return false;
    }
    info->blockSize = blockSize;
    info->totalBlocks = totalBlocks;
    file.seek(0);
    return true;
}

QVector<SparseExtent> SparseImage::coalesce(const QVector<SparseExtent>& extents,
                                            quint32 blockSize)
{
    QVector<SparseExtent> merged;
    merged.reserve(extents.size());
    for (const SparseExtent& extent : extents) {
        if (extent.type == SparseExtentType::Skip) continue;  // 本就不传输
        if (!merged.isEmpty()) {
            const SparseExtent& last = merged.last();
            const bool contiguous = last.outputBlock + last.blockCount == extent.outputBlock;
            bool mergeable = contiguous && last.type == extent.type;
            if (mergeable && extent.type == SparseExtentType::Raw) {
                // RAW 还要求文件侧连续：两个 chunk 在镜像文件里必须首尾相接，
                // 合并后才能用一次顺序读拿完整数据。fileOffset 是字节偏移，
                // blockCount 是块数，连续性要乘 blockSize。
                // blockSize==0 时 max/blockSize 就是除零 UB（inspect 已挡，
                // 但这是 public API，这里再 fail-closed 一次：RAW 一律不合并）。
                mergeable = blockSize != 0 &&
                    last.blockCount <= std::numeric_limits<quint64>::max() / blockSize &&
                    last.fileOffset + last.blockCount * blockSize == extent.fileOffset;
            }
            if (mergeable && extent.type == SparseExtentType::Fill) {
                mergeable = last.fillValue == extent.fillValue;
            }
            if (mergeable) {
                merged.last().blockCount += extent.blockCount;
                continue;
            }
        }
        merged.push_back(extent);
    }
    return merged;
}

} // namespace swt::edl
