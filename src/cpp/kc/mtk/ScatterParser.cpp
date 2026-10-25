#include "kc/mtk/ScatterParser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QXmlStreamReader>

namespace kc::mtk {
namespace {

/// 经典 scatter 的分区字段名（大小写不敏感）。用于「带 - 前缀的字段也归当前分区」的兼容判定。
bool isPartitionFieldName(const QString& keyLower)
{
    static const QStringList kFields = {
        QStringLiteral("partition_name"),
        QStringLiteral("file_name"),
        QStringLiteral("is_download"),
        QStringLiteral("physical_start_addr"),
        QStringLiteral("partition_size"),
        QStringLiteral("region"),
        QStringLiteral("storage"),
        QStringLiteral("operation_type"),
        QStringLiteral("is_reserved"),
        QStringLiteral("linear_start_addr"),
        QStringLiteral("type"),
        QStringLiteral("reserve"),
    };
    return kFields.contains(keyLower);
}

bool isTrueText(const QString& value)
{
    const QString v = value.trimmed().toLower();
    return v == QStringLiteral("true") || v == QStringLiteral("yes") || v == QStringLiteral("1");
}

/// 去掉 UTF-8 BOM（上游导出的 scatter 常带）。
QString stripBom(const QString& text)
{
    if (text.startsWith(QChar(0xFEFF))) return text.mid(1);
    return text;
}

void finalizeFiles(ScatterInfo& info, const QString& baseDir)
{
    for (ScatterPartition& part : info.partitions) {
        if (part.fileName.isEmpty()) continue;
        const QString abs = QDir::cleanPath(baseDir.isEmpty()
                                                ? part.fileName
                                                : QDir(baseDir).absoluteFilePath(part.fileName));
        part.absoluteFilePath = abs;
        part.fileExists = QFileInfo::exists(abs);
    }
}

} // namespace

bool ScatterParser::parseNumber(const QString& text, uint64_t* out)
{
    if (!out) return false;
    QString t = text.trimmed();
    if (t.isEmpty()) return false;

    bool ok = false;
    qint64 value = 0;
    if (t.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        value = t.mid(2).toLongLong(&ok, 16);
    } else {
        // 经典 scatter 的地址/尺寸偶尔写十进制
        value = t.toLongLong(&ok, 10);
    }
    if (!ok || value < 0) return false;
    *out = static_cast<uint64_t>(value);
    return true;
}

ScatterInfo ScatterParser::parseFile(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ScatterInfo info;
        info.errorMessage = QStringLiteral("无法打开 scatter 文件：%1").arg(filePath);
        return info;
    }
    const QString text = QString::fromUtf8(file.readAll());
    const QString baseDir = QFileInfo(filePath).absolutePath();

    if (filePath.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive)) {
        return parseXmlText(text, baseDir);
    }
    return parseClassicText(text, baseDir);
}

ScatterInfo ScatterParser::parseClassicText(const QString& text, const QString& baseDir)
{
    ScatterInfo info;
    const QString body = stripBom(text);
    const QStringList lines = body.split(QLatin1Char('\n'));

    int current = -1; // 当前分区下标；-1 = 不在分区块里

    for (const QString& rawLine : lines) {
        QString line = rawLine;
        line.remove(QLatin1Char('\r'));
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))) continue;

        // 去掉字段前的 "- " 前缀（有的打包工具给每个字段都加）
        QString bodyText = trimmed;
        if (bodyText.startsWith(QStringLiteral("- "))) bodyText = bodyText.mid(2).trimmed();

        const int colon = bodyText.indexOf(QLatin1Char(':'));
        if (colon < 0) continue;
        const QString key = bodyText.left(colon).trimmed().toLower();
        const QString value = bodyText.mid(colon + 1).trimmed();

        if (key == QStringLiteral("partition_index")) {
            ScatterPartition part;
            part.isDownload = true; // 默认下载；显式 false 才排除
            part.enabled = true;
            info.partitions.append(part);
            current = info.partitions.size() - 1;
            continue;
        }

        // 分区块内：已知分区字段一律归当前分区（不管有没有 - 前缀，见头注释）
        if (current >= 0 && isPartitionFieldName(key)) {
            ScatterPartition& part = info.partitions[current];
            if (key == QStringLiteral("partition_name")) {
                part.name = value;
            } else if (key == QStringLiteral("file_name")) {
                part.fileName = value;
            } else if (key == QStringLiteral("is_download")) {
                part.isDownload = isTrueText(value);
                part.enabled = part.isDownload;
            } else if (key == QStringLiteral("physical_start_addr")) {
                parseNumber(value, &part.startAddress);
            } else if (key == QStringLiteral("partition_size")) {
                parseNumber(value, &part.size);
            } else if (key == QStringLiteral("region")) {
                part.region = value;
            } else if (key == QStringLiteral("storage")) {
                part.storage = storageTypeFromName(value);
            } else if (key == QStringLiteral("operation_type")) {
                part.operationType = value.toUpper();
                if (part.operationType.contains(QStringLiteral("BINREGION"))
                    || part.operationType.contains(QStringLiteral("PROTECTED"))) {
                    info.protectedParts.append(part.name);
                }
            } else if (key == QStringLiteral("is_reserved")) {
                part.isReserved = isTrueText(value);
            }
            continue;
        }

        // 顶层通用字段（platform / storage / boot_channel …）。见到顶层字段 = 分区块结束。
        current = -1;
        if (key == QStringLiteral("platform") || key == QStringLiteral("chip_name")) {
            info.chipName = value;
        } else if (key == QStringLiteral("storage")) {
            info.storageName = value;
            info.storage = storageTypeFromName(value);
        }
    }

    if (info.partitions.isEmpty()) {
        info.errorMessage = QStringLiteral("scatter 中未找到任何分区（partition_index）");
        return info;
    }
    // 无名分区块 = 非致命。真实 scatter 常以 `- partition_index: SYS0` 收尾，
    // 有的版本不给 partition_name —— 直接丢掉该块并留一条警告，不要把整包判死
    // （判死 = 用户明明有完整线刷包却没法刷，代价远大于多一个警告）。
    QVector<ScatterPartition> kept;
    kept.reserve(info.partitions.size());
    for (const ScatterPartition& part : info.partitions) {
        if (part.name.isEmpty()) {
            info.warnings << QStringLiteral("已跳过 1 个没有 partition_name 的分区块");
            continue;
        }
        kept.append(part);
    }
    info.partitions = kept;
    if (info.partitions.isEmpty()) {
        info.errorMessage = QStringLiteral("scatter 中没有可用的分区（全部缺少 partition_name）");
        return info;
    }

    finalizeFiles(info, baseDir);
    info.valid = true;
    return info;
}

ScatterInfo ScatterParser::parseXmlText(const QString& text, const QString& baseDir)
{
    ScatterInfo info;
    QXmlStreamReader xml(stripBom(text));

    ScatterPartition current;
    bool inPartition = false;

    while (!xml.atEnd() && !xml.hasError()) {
        const QXmlStreamReader::TokenType token = xml.readNext();
        if (token != QXmlStreamReader::StartElement) continue;

        // 🔴 节点名归一成小写：上游 v6 的真实节点名全是小写（XMLConst.h：
        //    general/platform/storage_type/partition_index/is_download/partition_name/
        //    physical_start_addr/file_name/operation_type），归一后两种大小写都能吃。
        const QString tag = xml.name().toString().toLower();

        if (tag == QStringLiteral("platform")) {
            info.chipName = xml.readElementText().trimmed();
        } else if (tag == QStringLiteral("skip_pt_operate")) {
            info.skipPtOperation = xml.readElementText().trimmed().toUpper() == QStringLiteral("TRUE");
        } else if (tag == QStringLiteral("storage_type")) {
            const QString name =
                xml.attributes().value(QStringLiteral("name")).toString().trimmed();
            info.storageName = name;
            info.storage = storageTypeFromName(name);
        } else if (tag == QStringLiteral("partition_index")) {
            if (inPartition) info.partitions.append(current);
            current = ScatterPartition();
            current.isDownload = true;
            current.enabled = true;
            inPartition = true;
        } else if (inPartition && tag == QStringLiteral("is_download")) {
            const QString v = xml.readElementText().trimmed();
            current.isDownload = isTrueText(v);
            current.enabled = current.isDownload;
        } else if (inPartition && tag == QStringLiteral("partition_name")) {
            current.name = xml.readElementText().trimmed();
        } else if (inPartition && tag == QStringLiteral("physical_start_addr")) {
            parseNumber(xml.readElementText(), &current.startAddress);
        } else if (inPartition && tag == QStringLiteral("partition_size")) {
            parseNumber(xml.readElementText(), &current.size);
        } else if (inPartition && tag == QStringLiteral("region")) {
            current.region = xml.readElementText().trimmed();
        } else if (inPartition && tag == QStringLiteral("file_name")) {
            current.fileName = xml.readElementText().trimmed();
        } else if (inPartition && tag == QStringLiteral("operation_type")) {
            current.operationType = xml.readElementText().trimmed().toUpper();
            if (current.operationType.contains(QStringLiteral("BINREGION"))
                || current.operationType.contains(QStringLiteral("PROTECTED"))) {
                info.protectedParts.append(current.name);
            }
        } else if (inPartition && tag == QStringLiteral("storage")) {
            current.storage = storageTypeFromName(xml.readElementText());
        } else if (inPartition && tag == QStringLiteral("is_reserved")) {
            current.isReserved = isTrueText(xml.readElementText());
        }
    }
    if (inPartition) info.partitions.append(current);

    if (xml.hasError()) {
        info = ScatterInfo();
        info.errorMessage = QStringLiteral("XML 解析失败：%1").arg(xml.errorString());
        return info;
    }
    if (info.partitions.isEmpty()) {
        info.errorMessage = QStringLiteral("XML scatter 中未找到任何 PARTITION_INDEX");
        return info;
    }

    // 同经典格式：无名分区块（partition_index 收尾项）跳过并警告，不判死整包。
    QVector<ScatterPartition> keptXml;
    keptXml.reserve(info.partitions.size());
    for (const ScatterPartition& part : info.partitions) {
        if (part.name.isEmpty()) {
            info.warnings << QStringLiteral("已跳过 1 个没有 partition_name 的分区块");
            continue;
        }
        keptXml.append(part);
    }
    info.partitions = keptXml;
    if (info.partitions.isEmpty()) {
        info.errorMessage = QStringLiteral("XML scatter 中没有可用的分区（全部缺少 partition_name）");
        return info;
    }

    finalizeFiles(info, baseDir);
    info.valid = true;
    return info;
}

} // namespace kc::mtk
