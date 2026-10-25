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

    for (quint32 index = 0; index < totalChunks; ++index) {
        QByteArray chunk;
        if (!readExact(file, cursor, chunkHeaderSize, &chunk)) {
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
            if (totalSize != static_cast<quint32>(chunkHeaderSize) + 4U) {
                if (error) *error = QStringLiteral("sparse CRC chunk %1 长度错误").arg(index);
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
            if (!readExact(file, cursor + chunkHeaderSize, 4, &fill) ||
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

} // namespace swt::edl
