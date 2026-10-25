#include "storage/RawProgramParser.h"

#include "storage/DynamicSuper.h"
#include "storage/SparseImage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QXmlStreamReader>

#include <limits>

namespace swt::edl {
namespace {

bool parseUnsigned(QString value, quint64* result)
{
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('.'))) value.chop(1);
    bool ok = false;
    const int base = value.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10;
    const quint64 parsed = value.toULongLong(&ok, base);
    if (ok && result) *result = parsed;
    return ok;
}

bool parseU32(const QString& value, quint32* result)
{
    quint64 parsed = 0;
    if (!parseUnsigned(value, &parsed) || parsed > std::numeric_limits<quint32>::max()) return false;
    *result = static_cast<quint32>(parsed);
    return true;
}

bool parseBoolean(const QString& value)
{
    return value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 ||
           value == QStringLiteral("1") ||
           value.compare(QStringLiteral("yes"), Qt::CaseInsensitive) == 0;
}

QString resolveImage(const QString& xmlPath, const QString& filename)
{
    QString normalized = QDir::fromNativeSeparators(filename.trimmed());
    if (normalized.isEmpty()) return {};

    QFileInfo sourceInfo(normalized);
    if (sourceInfo.isAbsolute() && sourceInfo.isFile()) return sourceInfo.canonicalFilePath();

    // Strict rawprogram semantics: only the exact path written in filename is
    // accepted. Decorated, prefixed and project-specific lookalikes are left
    // untouched and reported as missing by the caller.
    const QDir xmlDirectory(QFileInfo(xmlPath).absolutePath());
    const QString direct = QDir::cleanPath(xmlDirectory.filePath(normalized));
    if (QFileInfo::exists(direct) && QFileInfo(direct).isFile()) {
        return QFileInfo(direct).canonicalFilePath();
    }
    return direct;
}

QString attribute(const QXmlStreamAttributes& attributes, const char* name)
{
    return attributes.value(QLatin1String(name)).toString().trimmed();
}

} // namespace

bool RawProgramParser::parse(const QStringList& xmlPaths, RawProgramPlan* plan, QString* error)
{
    if (!plan) {
        if (error) *error = QStringLiteral("XML 刷机计划输出参数为空");
        return false;
    }
    *plan = {};
    if (xmlPaths.isEmpty()) {
        if (error) *error = QStringLiteral("尚未选择 rawprogram XML");
        return false;
    }

    QSet<QString> seenXml;
    for (const QString& requestedPath : xmlPaths) {
        const QFileInfo xmlInfo(requestedPath);
        const QString xmlPath = xmlInfo.canonicalFilePath();
        if (xmlPath.isEmpty() || !xmlInfo.isFile()) {
            if (error) *error = QStringLiteral("XML 文件不存在：%1").arg(requestedPath);
            return false;
        }
        if (seenXml.contains(xmlPath.toLower())) continue;
        seenXml.insert(xmlPath.toLower());
        plan->xmlPaths.push_back(xmlPath);

        QFile xmlFile(xmlPath);
        if (!xmlFile.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("打开 XML 失败：%1").arg(xmlFile.errorString());
            return false;
        }

        QXmlStreamReader reader(&xmlFile);
        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement() ||
                reader.name().compare(QLatin1String("program"), Qt::CaseInsensitive) != 0) {
                continue;
            }

            const QXmlStreamAttributes attrs = reader.attributes();
            RawProgramEntry entry;
            entry.xmlPath = xmlPath;
            entry.filename = attribute(attrs, "filename");
            entry.label = attribute(attrs, "label");
            entry.startSectorExpression = attribute(attrs, "start_sector");
            entry.sparse = parseBoolean(attribute(attrs, "sparse"));
            if (entry.filename.isEmpty()) continue;

            if (!parseU32(attribute(attrs, "SECTOR_SIZE_IN_BYTES"), &entry.sectorSize) ||
                entry.sectorSize == 0) {
                if (error) *error = QStringLiteral("%1：program 的 SECTOR_SIZE_IN_BYTES 错误")
                                        .arg(xmlInfo.fileName());
                return false;
            }
            if (!parseU32(attribute(attrs, "physical_partition_number"), &entry.lun)) {
                if (error) *error = QStringLiteral("%1：program 的 physical_partition_number 错误")
                                        .arg(xmlInfo.fileName());
                return false;
            }
            if (!parseUnsigned(attribute(attrs, "num_partition_sectors"), &entry.sectorCount)) {
                if (error) *error = QStringLiteral("%1：program 的 num_partition_sectors 错误")
                                        .arg(xmlInfo.fileName());
                return false;
            }
            entry.sourcePath = resolveImage(xmlPath, entry.filename);
            if (entry.sectorCount == 0 && QFileInfo(entry.sourcePath).isFile()) {
                QFile source(entry.sourcePath);
                if (source.open(QIODevice::ReadOnly) &&
                    (entry.sparse || SparseImage::hasSparseMagic(source))) {
                    SparseImageInfo sparseInfo;
                    QString sparseError;
                    if (!SparseImage::inspect(source, &sparseInfo, &sparseError)) {
                        if (error) {
                            *error = QStringLiteral("%1：零扇区 sparse 镜像解析失败：%2")
                                         .arg(entry.filename, sparseError);
                        }
                        return false;
                    }
                    const quint64 expandedBytes = sparseInfo.expandedSize();
                    entry.sectorCount = expandedBytes / entry.sectorSize +
                                        (expandedBytes % entry.sectorSize == 0 ? 0ULL : 1ULL);
                    entry.sparse = true;
                    plan->warnings.push_back(QStringLiteral(
                        "零扇区 sparse 条目已按镜像头解析：%1，展开 %2 字节 / %3 扇区")
                        .arg(entry.label.isEmpty() ? entry.filename : entry.label)
                        .arg(expandedBytes).arg(entry.sectorCount));
                }
            }
            if (entry.sectorCount == 0) {
                plan->warnings.push_back(QStringLiteral("跳过零扇区条目：%1")
                                             .arg(entry.label.isEmpty() ? entry.filename
                                                                        : entry.label));
                continue;
            }
            const QString fileOffset = attribute(attrs, "file_sector_offset");
            if (!fileOffset.isEmpty() && !parseUnsigned(fileOffset, &entry.fileSectorOffset)) {
                if (error) *error = QStringLiteral("%1：program 的 file_sector_offset 错误")
                                        .arg(xmlInfo.fileName());
                return false;
            }
            if (!parseUnsigned(entry.startSectorExpression, &entry.startSector)) {
                entry.startSectorResolved = false;
                plan->warnings.push_back(
                    QStringLiteral("动态起始扇区将在设备端解析：%1 (%2)")
                        .arg(entry.label.isEmpty() ? entry.filename : entry.label,
                             entry.startSectorExpression));
            }

            const QFileInfo sourceInfo(entry.sourcePath);
            if (!sourceInfo.isFile()) {
                const bool superEntry = entry.label.compare(
                        QStringLiteral("super"), Qt::CaseInsensitive) == 0 ||
                    QFileInfo(entry.filename).fileName().compare(
                        QStringLiteral("super.img"), Qt::CaseInsensitive) == 0;
                QString definitionPath;
                QString dynamicError;
                DynamicSuperPlan dynamicPlan;
                if (superEntry &&
                    DynamicSuperPlanner::discover(xmlPath, &definitionPath, &dynamicError) &&
                    DynamicSuperPlanner::create(xmlPath, definitionPath, &dynamicPlan,
                                                &dynamicError)) {
                    entry.dynamicSuper = true;
                    entry.sourcePath = definitionPath;
                    const quint64 dynamicBytes = dynamicPlan.totalWriteBytes();
                    plan->totalSourceBytes = dynamicBytes >
                            std::numeric_limits<quint64>::max() - plan->totalSourceBytes
                        ? std::numeric_limits<quint64>::max()
                        : plan->totalSourceBytes + dynamicBytes;
                    ++plan->superEntryCount;
                    ++plan->dynamicSuperCount;
                    plan->warnings.push_back(QStringLiteral(
                        "super.img 为空：已启用 META 免合并计划（%1 个动态分区）")
                        .arg(dynamicPlan.images.size()));
                    plan->entries.push_back(std::move(entry));
                    continue;
                }
                plan->warnings.push_back(
                    QStringLiteral("跳过缺失镜像：%1 → %2")
                        .arg(entry.label.isEmpty() ? entry.filename : entry.label,
                             entry.filename));
                continue;
            }

            if (entry.label.compare(QStringLiteral("ocdt"), Qt::CaseInsensitive) == 0) {
                QFile ocdt(entry.sourcePath);
                const QByteArray magic = ocdt.open(QIODevice::ReadOnly) ? ocdt.read(4)
                                                                        : QByteArray{};
                if (sourceInfo.size() < 68 || magic != QByteArrayLiteral("TDCO")) {
                    plan->warnings.push_back(QStringLiteral(
                        "跳过无效 OCDT 镜像：%1（要求 TDCO 文件头且长度至少 68 字节）")
                        .arg(sourceInfo.fileName()));
                    continue;
                }
            }

            if (entry.fileSectorOffset >
                std::numeric_limits<quint64>::max() / entry.sectorSize) {
                if (error) *error = QStringLiteral("镜像偏移数值溢出：%1").arg(entry.filename);
                return false;
            }
            const quint64 offsetBytes = entry.fileSectorOffset * entry.sectorSize;
            const quint64 fileBytes = static_cast<quint64>(sourceInfo.size());
            if (offsetBytes >= fileBytes) {
                if (error) *error = QStringLiteral("镜像偏移超出文件：%1").arg(entry.filename);
                return false;
            }
            const quint64 capacity = entry.sectorCount >
                    std::numeric_limits<quint64>::max() / entry.sectorSize
                ? std::numeric_limits<quint64>::max()
                : entry.sectorCount * entry.sectorSize;
            const quint64 sourceBytes = qMin(fileBytes - offsetBytes, capacity);
            plan->totalSourceBytes = sourceBytes >
                    std::numeric_limits<quint64>::max() - plan->totalSourceBytes
                ? std::numeric_limits<quint64>::max()
                : plan->totalSourceBytes + sourceBytes;

            const QString superProbe = entry.label + QLatin1Char(' ') + entry.filename;
            if (superProbe.contains(QStringLiteral("super"), Qt::CaseInsensitive)) {
                ++plan->superEntryCount;
            }
            plan->entries.push_back(std::move(entry));
        }

        if (reader.hasError()) {
            if (error) {
                *error = QStringLiteral("XML 解析失败：%1（第 %2 行，第 %3 列）")
                             .arg(reader.errorString())
                             .arg(reader.lineNumber())
                             .arg(reader.columnNumber());
            }
            return false;
        }
    }

    if (plan->entries.isEmpty()) {
        if (error) *error = QStringLiteral("所选 XML 中没有可刷写的 program 条目");
        return false;
    }
    return true;
}

} // namespace swt::edl
