#pragma once

#include <QString>
#include <QVector>

class QFile;

namespace swt::edl {

enum class SparseExtentType {
    Raw,
    Fill,
    Skip
};

struct SparseExtent {
    SparseExtentType type = SparseExtentType::Skip;
    quint64 outputBlock = 0;
    quint64 blockCount = 0;
    quint64 fileOffset = 0;
    quint32 fillValue = 0;
};

struct SparseImageInfo {
    quint32 blockSize = 0;
    quint64 totalBlocks = 0;
    QVector<SparseExtent> extents;

    [[nodiscard]] quint64 expandedSize() const { return totalBlocks * blockSize; }
};

class SparseImage final {
public:
    static constexpr quint32 Magic = 0xED26FF3A;

    static bool hasSparseMagic(QFile& file);
    static bool inspect(QFile& file, SparseImageInfo* info, QString* error);

    /// 合并输出侧相邻、类型相同的 extent：连续 RAW（文件侧也必须连续）并成一段，
    /// 同值连续 FILL 并成一段。DONT_CARE 直接丢弃（不产生传输）。
    /// 目的：真实 sparse 镜像常有大量相邻同类 chunk，逐 chunk 下一次 program
    /// 命令 = 逐 chunk 一次 XML 往返 + 一次 ACK 等待；合并后往返次数大幅下降。
    /// 纯函数，可单测。
    static QVector<SparseExtent> coalesce(const QVector<SparseExtent>& extents,
                                          quint32 blockSize);
};

} // namespace swt::edl
