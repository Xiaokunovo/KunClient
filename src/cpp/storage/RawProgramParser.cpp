#include "storage/RawProgramParser.h"

#include "storage/DynamicSuper.h"
#include "storage/SparseImage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QXmlStreamReader>

#include <algorithm>
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
    // 空 filename 在 Qualcomm 的 rawprogram 语义里是「擦除该范围」的标记。本工具
    // 没有「按 XML 执行 erase」的链路，只能跳过 —— 但**不许静默**：不聚合上报的话，
    // 用户会以为「XML 刷机完成」包含擦除，实际这些分区一个字节都没动。
    QStringList skippedEraseLabels;
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
            if (entry.filename.isEmpty()) {
                const QString eraseLabel = entry.label.isEmpty()
                    ? QStringLiteral("(无标签)")
                    : entry.label;
                if (!skippedEraseLabels.contains(eraseLabel, Qt::CaseInsensitive)) {
                    skippedEraseLabels.push_back(eraseLabel);
                }
                continue;
            }

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
            // super 条目识别：label 精确为 super，或文件名是 super.img / super_*.img
            // （真实包里 super.img、super_1.img、super_a.img 都出现过）。
            const QString superBasename = QFileInfo(entry.filename).fileName().toLower();
            const bool superEntry =
                entry.label.compare(QStringLiteral("super"), Qt::CaseInsensitive) == 0 ||
                superBasename == QStringLiteral("super.img") ||
                (superBasename.startsWith(QStringLiteral("super_")) &&
                 superBasename.endsWith(QStringLiteral(".img")));
            // super 镜像缺失，或是 0 字节占位（动态分区包里常见的空 super.img），都走
            // META 免合并。空文件若按普通路径往下走，只会在「镜像偏移超出文件」处硬
            // 失败 —— 用户拿着动态分区包会直接刷不了，必须先给免合并一次机会。
            if (superEntry && (!sourceInfo.isFile() || sourceInfo.size() == 0)) {
                QString definitionPath;
                QString dynamicError;
                DynamicSuperPlan dynamicPlan;
                if (DynamicSuperPlanner::discover(xmlPath, &definitionPath, &dynamicError) &&
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
                        "super.img %1：已启用 META 免合并计划（%2 个动态分区）")
                        .arg(sourceInfo.isFile() ? QStringLiteral("为空") : QStringLiteral("缺失"))
                        .arg(dynamicPlan.images.size()));
                    plan->entries.push_back(std::move(entry));
                    continue;
                }
            }

            if (!sourceInfo.isFile()) {
                plan->warnings.push_back(
                    QStringLiteral("跳过缺失镜像：%1 → %2")
                        .arg(entry.label.isEmpty() ? entry.filename : entry.label,
                             entry.filename));
                continue;
            }
            if (sourceInfo.size() == 0) {
                if (error) {
                    *error = QStringLiteral(
                        "镜像为 0 字节空文件：%1（动态分区包请确认包内带 META/super_def*.json）")
                        .arg(entry.filename);
                }
                return false;
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

    if (!skippedEraseLabels.isEmpty()) {
        plan->warnings.push_back(QStringLiteral(
            "跳过 %1 个擦除条目（rawprogram 里 filename 为空）：%2 —— "
            "本工具不会按 XML 执行 erase，这些分区没有被擦除。"
            "如需清空，请在分区表中勾选后点「擦除分区」。")
            .arg(skippedEraseLabels.size())
            .arg(skippedEraseLabels.join(QStringLiteral("、"))));
    }

    if (plan->entries.isEmpty()) {
        if (error) *error = QStringLiteral("所选 XML 中没有可刷写的 program 条目");
        return false;
    }
    return true;
}

QStringList RawProgramParser::discover(const QString& directory, QString* usedDirectory,
                                       QString* error)
{
    if (usedDirectory) usedDirectory->clear();
    const QDir root(directory);
    if (!root.exists()) {
        if (error) *error = QStringLiteral("目录不存在：%1").arg(QDir::toNativeSeparators(directory));
        return {};
    }

    const auto collect = [](const QDir& dir) {
        QStringList paths;
        const QFileInfoList files = dir.entryInfoList({QStringLiteral("rawprogram*.xml")},
                                                      QDir::Files, QDir::Name);
        for (const QFileInfo& file : files) paths.push_back(file.absoluteFilePath());
        return paths;
    };

    QStringList found = collect(root);
    if (!found.isEmpty()) {
        if (usedDirectory) *usedDirectory = root.absolutePath();
        return found;
    }

    // 一级子目录：IMAGES/images 提到最前（高通包的标准布局），其余按名字序。
    QFileInfoList subdirs = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    std::stable_sort(subdirs.begin(), subdirs.end(),
                     [](const QFileInfo& left, const QFileInfo& right) {
                         const bool leftImages =
                             left.fileName().compare(QStringLiteral("IMAGES"),
                                                     Qt::CaseInsensitive) == 0;
                         const bool rightImages =
                             right.fileName().compare(QStringLiteral("IMAGES"),
                                                      Qt::CaseInsensitive) == 0;
                         if (leftImages != rightImages) return leftImages;
                         return left.fileName().compare(right.fileName(),
                                                        Qt::CaseInsensitive) < 0;
                     });
    for (const QFileInfo& subdir : std::as_const(subdirs)) {
        const QDir dir(subdir.absoluteFilePath());
        found = collect(dir);
        if (found.isEmpty()) continue;
        if (usedDirectory) *usedDirectory = dir.absolutePath();
        return found;
    }

    if (error) {
        *error = QStringLiteral("目录里没有找到 rawprogram*.xml：%1\n"
                                "已检查该目录及其 %2 个一级子目录。")
                     .arg(QDir::toNativeSeparators(root.absolutePath()))
                     .arg(subdirs.size());
    }
    return {};
}

bool RawProgramParser::inspect(const RawProgramPlan& plan, quint32 expectedSectorSize,
                               QStringList* details, QString* error)
{
    QStringList lines;
    QStringList errors;
    lines.push_back(QStringLiteral(
        "仅源包预检：未向设备发送指令，没有刷写或擦除，也不会自动重启。\n"
        "动态起点和 LUN 容量尚需连接设备确认；数字区间仅检查算术，不代表真实 LUN 验证通过。"));
    lines.push_back(QStringLiteral("预期扇区大小：%1 字节；XML：%2 个；条目：%3 个")
                        .arg(expectedSectorSize).arg(plan.xmlPaths.size()).arg(plan.entries.size()));
    for (const QString& xmlPath : plan.xmlPaths) {
        lines.push_back(QStringLiteral("XML：%1").arg(xmlPath));
    }
    lines.push_back(QStringLiteral("=== 重要：原计划警告 / 跳过项（%1） ===")
                        .arg(plan.warnings.size()));
    for (const QString& warning : plan.warnings) {
        lines.push_back(QStringLiteral("【重要警告】%1").arg(warning));
    }
    const auto fail = [&lines, &errors](const QString& message) {
        errors.push_back(message);
        lines.push_back(QStringLiteral("【错误】%1").arg(message));
    };
    if (expectedSectorSize == 0) fail(QStringLiteral("预期扇区大小不能为 0"));
    if (plan.entries.isEmpty()) fail(QStringLiteral("计划中没有可预检的 program 条目"));

    constexpr quint64 maxValue = std::numeric_limits<quint64>::max();
    QVector<const RawProgramEntry*> numericEntries;
    for (qsizetype index = 0; index < plan.entries.size(); ++index) {
        const RawProgramEntry& entry = plan.entries.at(index);
        const QString context = QStringLiteral("条目 %1 | LUN %2 | label=%3")
                                    .arg(index + 1).arg(entry.lun)
                                    .arg(entry.label.isEmpty() ? QStringLiteral("(无标签)")
                                                               : entry.label);
        const qsizetype previousErrors = errors.size();
        lines.push_back(QStringLiteral("\n=== %1 ===").arg(context));
        lines.push_back(QStringLiteral("filename=%1\nsource=%2\n扇区大小=%3；count=%4；file_sector_offset=%5")
                            .arg(entry.filename, entry.sourcePath).arg(entry.sectorSize)
                            .arg(entry.sectorCount).arg(entry.fileSectorOffset));
        if (entry.sectorSize == 0 || entry.sectorSize != expectedSectorSize) {
            fail(QStringLiteral("%1：扇区大小错误（entry=%2，expected=%3）")
                     .arg(context).arg(entry.sectorSize).arg(expectedSectorSize));
        }
        if (entry.sectorCount == 0) fail(QStringLiteral("%1：扇区 count 不能为 0").arg(context));
        const bool targetValid = entry.sectorSize != 0 && entry.sectorCount != 0 &&
                                 entry.sectorCount <= maxValue / entry.sectorSize;
        quint64 targetBytes = 0;
        if (targetValid) {
            targetBytes = entry.sectorCount * entry.sectorSize;
            lines.push_back(QStringLiteral("XML 目标 range 长度=%1 扇区 / %2 字节")
                                .arg(entry.sectorCount).arg(targetBytes));
        } else if (entry.sectorSize != 0 && entry.sectorCount > maxValue / entry.sectorSize) {
            fail(QStringLiteral("%1：count * sector 溢出，目标 range 不可计算").arg(context));
        }

        if (!entry.startSectorResolved) {
            lines.push_back(QStringLiteral(
                "【待设备确认】start 表达式=%1；数值未解析；range=[%1, %1 + %2)，不能离线验证 LUN 容量")
                                .arg(entry.startSectorExpression).arg(entry.sectorCount));
        } else {
            lines.push_back(QStringLiteral("start 表达式=%1；数值=%2")
                                .arg(entry.startSectorExpression).arg(entry.startSector));
            if (entry.sectorCount > maxValue - entry.startSector) {
                fail(QStringLiteral("%1：start + count 溢出，数字 range 不可计算").arg(context));
            } else {
                const quint64 end = entry.startSector + entry.sectorCount;
                lines.push_back(QStringLiteral("数字 range=[%1, %2)（扇区，右端不含；不是实际 LUN 容量验证）")
                                    .arg(entry.startSector).arg(end));
                for (const RawProgramEntry* previous : numericEntries) {
                    if (previous->lun == entry.lun && entry.sectorCount != 0 &&
                        entry.startSector < previous->startSector + previous->sectorCount &&
                        previous->startSector < end) {
                        lines.push_back(QStringLiteral(
                            "【警告】同 LUN 数字区间重叠：%1 / %2；sparse 分段可能合法，仅提示、不拒绝，尚未验证真实 LUN")
                                            .arg(previous->label, entry.label));
                    }
                }
                if (entry.sectorCount != 0) numericEntries.push_back(&entry);
            }
        }

        const bool offsetValid = entry.sectorSize != 0 &&
                                 entry.fileSectorOffset <= maxValue / entry.sectorSize;
        quint64 offsetBytes = 0;
        if (offsetValid) {
            offsetBytes = entry.fileSectorOffset * entry.sectorSize;
            lines.push_back(QStringLiteral("源文件 offset=%1 字节").arg(offsetBytes));
        } else if (entry.sectorSize != 0) {
            fail(QStringLiteral("%1：offset * sector 溢出").arg(context));
        }
        QFile source(entry.sourcePath);
        if (!source.open(QIODevice::ReadOnly)) {
            lines.push_back(QStringLiteral("文件 size=不可读取；expanded/transfer=不可计算"));
            fail(QStringLiteral("%1：打开 source 失败：%2（%3）")
                     .arg(context, entry.sourcePath, source.errorString()));
            continue;
        }
        const qint64 actualSize = source.size();
        lines.push_back(QStringLiteral("文件 size=%1 字节%2")
                            .arg(actualSize)
                            .arg(entry.dynamicSuper ? QStringLiteral("（META 定义文件）") : QString()));
        if (actualSize < 0) {
            fail(QStringLiteral("%1：读取 source 实际长度失败").arg(context));
            continue;
        }
        const quint64 fileBytes = static_cast<quint64>(actualSize);
        const bool offsetInFile = offsetValid && offsetBytes < fileBytes;
        if (offsetValid && !offsetInFile) {
            fail(QStringLiteral("%1：镜像偏移超出文件实际长度（offset=%2，size=%3）")
                     .arg(context).arg(offsetBytes).arg(fileBytes));
        }

        if (entry.dynamicSuper) {
            DynamicSuperPlan dynamicPlan;
            QString dynamicError;
            if (!DynamicSuperPlanner::create(entry.xmlPath, entry.sourcePath,
                                              &dynamicPlan, &dynamicError)) {
                lines.push_back(QStringLiteral("dynamicSuper expanded/transfer=不可计算"));
                fail(QStringLiteral("%1：dynamicSuper create 失败：%2").arg(context, dynamicError));
            } else {
                lines.push_back(QStringLiteral(
                    "dynamicSuper expanded=%1 字节（blockDeviceSize）；transfer=%2 字节（含 META 和分区分配范围）")
                                    .arg(dynamicPlan.blockDeviceSize).arg(dynamicPlan.totalWriteBytes()));
                if (targetValid && dynamicPlan.blockDeviceSize > targetBytes) {
                    fail(QStringLiteral("%1：dynamicSuper blockDeviceSize 超出 XML 目标（%2 > %3）")
                             .arg(context).arg(dynamicPlan.blockDeviceSize).arg(targetBytes));
                }
                for (const DynamicSuperImage& image : dynamicPlan.images) {
                    lines.push_back(QStringLiteral("  动态分区 %1：source=%2；文件 size=%3；expanded=%4；分配/transfer=%5 字节")
                                        .arg(image.partitionName, image.sourcePath)
                                        .arg(QFileInfo(image.sourcePath).size())
                                        .arg(image.expandedSize).arg(image.allocatedSize));
                }
            }
        } else if (entry.sparse || SparseImage::hasSparseMagic(source)) {
            if (entry.fileSectorOffset != 0) {
                fail(QStringLiteral("%1：sparse 要求 file_sector_offset 为 0").arg(context));
            }
            SparseImageInfo sparseInfo;
            QString sparseError;
            if (!SparseImage::inspect(source, &sparseInfo, &sparseError)) {
                lines.push_back(QStringLiteral("sparse expanded/transfer=不可计算"));
                fail(QStringLiteral("%1：sparse inspect 失败：%2").arg(context, sparseError));
            } else {
                quint64 transferBytes = 0;
                for (const SparseExtent& extent : sparseInfo.extents) {
                    if (extent.type != SparseExtentType::Skip) {
                        transferBytes += extent.blockCount * sparseInfo.blockSize;
                    }
                }
                const quint64 expandedBytes = sparseInfo.expandedSize();
                lines.push_back(QStringLiteral("sparse block=%1；expanded=%2 字节；transfer=%3 字节（非 Skip）")
                                    .arg(sparseInfo.blockSize).arg(expandedBytes).arg(transferBytes));
                lines.push_back(QStringLiteral(
                    "Skip/DONT_CARE=%1 字节：跳过、不传输、不是擦除，不保证目标原有数据清零。")
                                    .arg(expandedBytes - transferBytes));
                if (entry.sectorSize != 0 && sparseInfo.blockSize % entry.sectorSize != 0) {
                    fail(QStringLiteral("%1：sparse block 未按 entry 扇区大小对齐").arg(context));
                }
                if (targetValid && expandedBytes > targetBytes) {
                    fail(QStringLiteral("%1：sparse 展开超出 XML 目标（%2 > %3）")
                             .arg(context).arg(expandedBytes).arg(targetBytes));
                }
            }
        } else if (offsetInFile && targetValid) {
            const quint64 available = fileBytes - offsetBytes;
            const quint64 transferBytes = qMin(available, targetBytes);
            lines.push_back(QStringLiteral("raw 剩余源数据=%1 字节；expanded=%2 字节；transfer=%2 字节")
                                .arg(available).arg(transferBytes));
            if (available != targetBytes) {
                lines.push_back(QStringLiteral(
                    "【提示】raw 按 min(剩余文件长度, XML 目标长度) 取数据；未覆盖的目标尾部不代表擦除。"));
            }
        } else {
            lines.push_back(QStringLiteral("raw expanded/transfer=不可计算"));
        }
        lines.push_back(errors.size() == previousErrors
                            ? QStringLiteral("源包条目校验通过（设备范围尚未确认）")
                            : QStringLiteral("源包条目校验失败；继续展示其余条目"));
    }
    lines.push_back(errors.isEmpty()
                        ? QStringLiteral("\n源包预检通过；请核对所有警告。未验证设备，未刷写。")
                        : QStringLiteral("\n源包预检失败：%1 项错误。未刷写。").arg(errors.size()));
    if (details) *details = lines;
    if (error) *error = errors.join(QLatin1Char('\n'));
    return errors.isEmpty();
}

} // namespace swt::edl
