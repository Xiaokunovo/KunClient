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
    qint64 sourceSizeAtPreflight = -1; // Worker 预检快照；不允许执行时静默少写
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
    // 仅检查源包；动态起点与真实 LUN 容量必须连接设备后另行确认。
    /// 从刷机包目录自动发现 rawprogram XML（「自动选择」按钮用）。
    ///
    /// 查找顺序：目录本身 → 一级子目录（`IMAGES` / `images` 优先，其余按名字序）。
    /// 命中即返回该目录下的全部 `rawprogram*.xml`（标准与非标准**都返回**，由调用方
    /// 决定要不要混选），并按文件名排序。找不到时返回空并写 `error`。
    static QStringList discover(const QString& directory, QString* usedDirectory, QString* error);

    static bool inspect(const RawProgramPlan& plan, quint32 expectedSectorSize,
                        QStringList* details, QString* error);
};

} // namespace swt::edl
