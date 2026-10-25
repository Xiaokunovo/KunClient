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
};

} // namespace swt::edl
