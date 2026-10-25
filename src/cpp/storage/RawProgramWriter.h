#pragma once

#include "core/Types.h"

#include <QStringList>

namespace swt::edl {

class RawProgramWriter final {
public:
    static bool writeBackup(const QString& outputDirectory,
                            const QVector<PartitionInfo>& partitions,
                            const QStringList& imageNames,
                            quint32 sectorSize,
                            QStringList* generatedPaths,
                            QString* error);
};

} // namespace swt::edl
