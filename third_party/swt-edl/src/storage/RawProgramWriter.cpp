#include "storage/RawProgramWriter.h"

#include <QDir>
#include <QMap>
#include <QSaveFile>
#include <QXmlStreamWriter>

namespace swt::edl {

bool RawProgramWriter::writeBackup(const QString& outputDirectory,
                                   const QVector<PartitionInfo>& partitions,
                                   const QStringList& imageNames,
                                   quint32 sectorSize,
                                   QStringList* generatedPaths,
                                   QString* error)
{
    if (generatedPaths) generatedPaths->clear();
    if (partitions.isEmpty() || partitions.size() != imageNames.size() || sectorSize == 0) {
        if (error) *error = QStringLiteral("生成 rawprogram XML 的参数异常");
        return false;
    }

    const QDir directory(outputDirectory);
    if (!directory.exists()) {
        if (error) *error = QStringLiteral("XML 输出目录不存在：%1").arg(outputDirectory);
        return false;
    }

    QMap<quint32, QVector<qsizetype>> entriesByLun;
    for (qsizetype index = 0; index < partitions.size(); ++index) {
        entriesByLun[partitions.at(index).lun].push_back(index);
    }

    for (auto iterator = entriesByLun.cbegin(); iterator != entriesByLun.cend(); ++iterator) {
        const QString path = directory.filePath(
            QStringLiteral("rawprogram%1.xml").arg(iterator.key()));
        QSaveFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (error) *error = QStringLiteral("创建 XML 失败：%1").arg(output.errorString());
            return false;
        }

        QXmlStreamWriter writer(&output);
        writer.setAutoFormatting(true);
        writer.writeStartDocument(QStringLiteral("1.0"));
        writer.writeStartElement(QStringLiteral("data"));
        for (const qsizetype index : iterator.value()) {
            const PartitionInfo& partition = partitions.at(index);
            const quint64 byteCount = partition.sectorCount() * static_cast<quint64>(sectorSize);
            writer.writeStartElement(QStringLiteral("program"));
            writer.writeAttribute(QStringLiteral("SECTOR_SIZE_IN_BYTES"),
                                  QString::number(sectorSize));
            writer.writeAttribute(QStringLiteral("file_sector_offset"), QStringLiteral("0"));
            writer.writeAttribute(QStringLiteral("filename"), imageNames.at(index));
            writer.writeAttribute(QStringLiteral("label"), partition.name);
            writer.writeAttribute(QStringLiteral("num_partition_sectors"),
                                  QString::number(partition.sectorCount()));
            writer.writeAttribute(QStringLiteral("physical_partition_number"),
                                  QString::number(partition.lun));
            writer.writeAttribute(QStringLiteral("size_in_KB"),
                                  QString::number(byteCount / 1024ULL));
            writer.writeAttribute(QStringLiteral("sparse"), QStringLiteral("false"));
            writer.writeAttribute(QStringLiteral("start_byte_hex"),
                                  QStringLiteral("0x%1").arg(
                                      partition.firstLba * static_cast<quint64>(sectorSize),
                                      0, 16));
            writer.writeAttribute(QStringLiteral("start_sector"),
                                  QString::number(partition.firstLba));
            writer.writeEndElement();
        }
        writer.writeEndElement();
        writer.writeEndDocument();

        if (writer.hasError() || !output.commit()) {
            if (error) *error = QStringLiteral("保存 XML 失败：%1").arg(output.errorString());
            return false;
        }
        if (generatedPaths) generatedPaths->push_back(path);
    }
    return true;
}

} // namespace swt::edl
