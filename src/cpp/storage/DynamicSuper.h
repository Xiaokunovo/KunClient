#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace swt::edl {

struct DynamicSuperExtent {
    quint64 logicalOffset = 0;
    quint64 physicalOffset = 0;
    quint64 byteCount = 0;
};

struct DynamicSuperImage {
    QString partitionName;
    QString sourcePath;
    quint64 expandedSize = 0;
    quint64 allocatedSize = 0;
    bool sparse = false;
    QVector<DynamicSuperExtent> extents;
};

struct DynamicSuperPlan {
    QString definitionPath;
    QString metadataPath;
    QString metadataFilename;
    QByteArray metadataImage;
    quint64 blockDeviceSize = 0;
    quint32 metadataSlotCount = 0;
    quint16 metadataMajorVersion = 0;
    quint16 metadataMinorVersion = 0;
    QVector<DynamicSuperImage> images;

    [[nodiscard]] quint64 totalWriteBytes() const;
};

class DynamicSuperPlanner final {
public:
    static bool discover(const QString& programXmlPath, QString* definitionPath,
                         QString* error);
    static bool create(const QString& programXmlPath, const QString& definitionPath,
                       DynamicSuperPlan* plan, QString* error);
};

} // namespace swt::edl
