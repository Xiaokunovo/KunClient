#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace swt::edl {

struct RawProgramEntry {
    QString xmlPath;
    QString filename;
    QString sourcePath;
    QString label;
    QString startSectorExpression;
    quint32 lun = 0;
    quint32 sectorSize = 0;
    quint64 startSector = 0;
    quint64 sectorCount = 0;
    quint64 fileSectorOffset = 0;
    bool sparse = false;
    bool startSectorResolved = true;
    bool dynamicSuper = false;
};

struct RawProgramPlan {
    QStringList xmlPaths;
    QVector<RawProgramEntry> entries;
    QStringList warnings;
    quint64 totalSourceBytes = 0;
    int superEntryCount = 0;
    int dynamicSuperCount = 0;

    [[nodiscard]] bool hasSplitSuper() const
    {
        return superEntryCount > 1 || dynamicSuperCount > 0;
    }
};

class RawProgramParser final {
public:
    static bool parse(const QStringList& xmlPaths, RawProgramPlan* plan, QString* error);
};

} // namespace swt::edl
