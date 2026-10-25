#include "storage/DynamicSuper.h"

#include "storage/SparseImage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>

#include <cstring>
#include <algorithm>
#include <limits>

namespace swt::edl {
namespace {

constexpr quint32 GeometryMagic = 0x616c4467;
constexpr quint32 MetadataMagic = 0x414c5030;
constexpr quint64 LpSectorSize = 512;

template <typename T>
bool readLe(const QByteArray& bytes, qsizetype offset, T* value)
{
    if (!value || offset < 0 || offset + static_cast<qsizetype>(sizeof(T)) > bytes.size()) {
        return false;
    }
    T raw{};
    std::memcpy(&raw, bytes.constData() + offset, sizeof(T));
    *value = qFromLittleEndian(raw);
    return true;
}

QString fixedString(const QByteArray& bytes, qsizetype offset, qsizetype length)
{
    if (offset < 0 || length < 0 || offset + length > bytes.size()) return {};
    QByteArray value = bytes.mid(offset, length);
    const qsizetype zero = value.indexOf('\0');
    if (zero >= 0) value.truncate(zero);
    return QString::fromLatin1(value).trimmed();
}

bool jsonUnsigned(const QJsonValue& value, quint64* result)
{
    bool ok = false;
    quint64 parsed = 0;
    if (value.isString()) {
        QString text = value.toString().trimmed();
        parsed = text.toULongLong(&ok, text.startsWith(QStringLiteral("0x"),
                                                       Qt::CaseInsensitive) ? 16 : 10);
    } else if (value.isDouble() && value.toDouble() >= 0.0) {
        parsed = static_cast<quint64>(value.toDouble());
        ok = static_cast<double>(parsed) == value.toDouble();
    }
    if (ok && result) *result = parsed;
    return ok;
}

QString packageRootFor(const QString& programXmlPath, const QString& definitionPath = {})
{
    QFileInfo definition(definitionPath);
    if (!definitionPath.isEmpty() && definition.dir().dirName().compare(
            QStringLiteral("META"), Qt::CaseInsensitive) == 0) {
        QDir root = definition.dir();
        root.cdUp();
        return root.absolutePath();
    }
    QDir directory(QFileInfo(programXmlPath).absolutePath());
    if (directory.dirName().compare(QStringLiteral("IMAGES"), Qt::CaseInsensitive) == 0 ||
        directory.dirName().compare(QStringLiteral("META"), Qt::CaseInsensitive) == 0) {
        directory.cdUp();
    }
    return directory.absolutePath();
}

QString resolvePackageFile(const QString& root, const QString& definitionPath,
                           const QString& relativePath)
{
    const QString normalized = QDir::fromNativeSeparators(relativePath.trimmed());
    const QStringList candidates{
        normalized,
        QDir(root).filePath(normalized),
        QFileInfo(definitionPath).dir().filePath(normalized),
        QDir(root).filePath(QStringLiteral("IMAGES/") + QFileInfo(normalized).fileName()),
        QFileInfo(definitionPath).dir().filePath(QFileInfo(normalized).fileName())
    };
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isFile()) return info.canonicalFilePath();
    }
    return {};
}

struct LpExtent {
    quint64 sectors = 0;
    quint32 type = 0;
    quint64 targetData = 0;
    quint32 targetSource = 0;
};

struct LpPartition {
    QString name;
    quint32 firstExtent = 0;
    quint32 extentCount = 0;
};

struct VendorMetadata {
    quint32 metadataMaxSize = 0;
    quint32 metadataSlotCount = 0;
    quint32 logicalBlockSize = 0;
    quint16 majorVersion = 0;
    quint16 minorVersion = 0;
    quint64 blockDeviceSize = 0;
    QByteArray geometry;
    QByteArray metadataBlob;
    QVector<LpExtent> extents;
    QHash<QString, LpPartition> partitions;
};

bool verifySha256Field(const QByteArray& bytes, qsizetype dataOffset, qsizetype dataSize,
                       qsizetype checksumOffset)
{
    if (dataOffset < 0 || dataSize <= 0 || dataOffset + dataSize > bytes.size() ||
        checksumOffset < dataOffset || checksumOffset + 32 > dataOffset + dataSize) {
        return false;
    }
    QByteArray checked = bytes.mid(dataOffset, dataSize);
    const QByteArray expected = checked.mid(checksumOffset - dataOffset, 32);
    std::fill(checked.begin() + (checksumOffset - dataOffset),
              checked.begin() + (checksumOffset - dataOffset) + 32, '\0');
    return QCryptographicHash::hash(checked, QCryptographicHash::Sha256) == expected;
}

bool parseVendorMetadata(const QString& path, VendorMetadata* metadata, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("打开 super_meta 失败：%1").arg(file.errorString());
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 4096 + 128) {
        if (error) *error = QStringLiteral("super_meta 文件过小");
        return false;
    }

    quint32 geometryMagic = 0;
    quint32 geometrySize = 0;
    readLe(bytes, 0, &geometryMagic);
    readLe(bytes, 4, &geometrySize);
    if (geometryMagic != GeometryMagic || geometrySize < 52 || geometrySize > 4096 ||
        !verifySha256Field(bytes, 0, geometrySize, 8)) {
        if (error) *error = QStringLiteral("super_meta geometry 校验失败");
        return false;
    }
    readLe(bytes, 40, &metadata->metadataMaxSize);
    readLe(bytes, 44, &metadata->metadataSlotCount);
    readLe(bytes, 48, &metadata->logicalBlockSize);
    if (metadata->metadataMaxSize < 128 || metadata->metadataMaxSize % 512 != 0 ||
        metadata->metadataSlotCount == 0 || metadata->metadataSlotCount > 16 ||
        metadata->logicalBlockSize == 0) {
        if (error) *error = QStringLiteral("super_meta geometry 参数错误");
        return false;
    }

    constexpr qsizetype headerOffset = 4096;
    quint32 magic = 0;
    quint32 headerSize = 0;
    quint32 tablesSize = 0;
    readLe(bytes, headerOffset, &magic);
    readLe(bytes, headerOffset + 4, &metadata->majorVersion);
    readLe(bytes, headerOffset + 6, &metadata->minorVersion);
    readLe(bytes, headerOffset + 8, &headerSize);
    readLe(bytes, headerOffset + 44, &tablesSize);
    if (magic != MetadataMagic || metadata->majorVersion != 10 ||
        (headerSize != 128 && headerSize != 256) ||
        headerOffset + headerSize + tablesSize > bytes.size() ||
        !verifySha256Field(bytes, headerOffset, headerSize, headerOffset + 12)) {
        if (error) *error = QStringLiteral("super_meta metadata header 校验失败");
        return false;
    }
    const QByteArray expectedTablesHash = bytes.mid(headerOffset + 48, 32);
    const QByteArray tables = bytes.mid(headerOffset + headerSize, tablesSize);
    if (QCryptographicHash::hash(tables, QCryptographicHash::Sha256) != expectedTablesHash) {
        if (error) *error = QStringLiteral("super_meta tables 校验失败");
        return false;
    }

    quint32 partitionsOffset = 0;
    quint32 partitionsCount = 0;
    quint32 partitionEntrySize = 0;
    quint32 extentsOffset = 0;
    quint32 extentsCount = 0;
    quint32 extentEntrySize = 0;
    quint32 blockDevicesOffset = 0;
    quint32 blockDevicesCount = 0;
    quint32 blockDeviceEntrySize = 0;
    readLe(bytes, headerOffset + 80, &partitionsOffset);
    readLe(bytes, headerOffset + 84, &partitionsCount);
    readLe(bytes, headerOffset + 88, &partitionEntrySize);
    readLe(bytes, headerOffset + 92, &extentsOffset);
    readLe(bytes, headerOffset + 96, &extentsCount);
    readLe(bytes, headerOffset + 100, &extentEntrySize);
    readLe(bytes, headerOffset + 116, &blockDevicesOffset);
    readLe(bytes, headerOffset + 120, &blockDevicesCount);
    readLe(bytes, headerOffset + 124, &blockDeviceEntrySize);
    if (partitionEntrySize < 52 || extentEntrySize < 24 || blockDeviceEntrySize < 64 ||
        blockDevicesCount != 1 ||
        static_cast<quint64>(partitionsOffset) + static_cast<quint64>(partitionsCount) * partitionEntrySize > tablesSize ||
        static_cast<quint64>(extentsOffset) + static_cast<quint64>(extentsCount) * extentEntrySize > tablesSize ||
        static_cast<quint64>(blockDevicesOffset) + blockDeviceEntrySize > tablesSize) {
        if (error) *error = QStringLiteral("super_meta 表描述符越界");
        return false;
    }

    const qsizetype tablesBase = headerOffset + headerSize;
    for (quint32 index = 0; index < partitionsCount; ++index) {
        const qsizetype offset = tablesBase + partitionsOffset +
                                 static_cast<qsizetype>(index) * partitionEntrySize;
        LpPartition partition;
        partition.name = fixedString(bytes, offset, 36);
        readLe(bytes, offset + 40, &partition.firstExtent);
        readLe(bytes, offset + 44, &partition.extentCount);
        if (partition.name.isEmpty() ||
            static_cast<quint64>(partition.firstExtent) + partition.extentCount > extentsCount) {
            if (error) *error = QStringLiteral("super_meta 分区条目错误");
            return false;
        }
        metadata->partitions.insert(partition.name.toLower(), partition);
    }
    for (quint32 index = 0; index < extentsCount; ++index) {
        const qsizetype offset = tablesBase + extentsOffset +
                                 static_cast<qsizetype>(index) * extentEntrySize;
        LpExtent extent;
        readLe(bytes, offset, &extent.sectors);
        readLe(bytes, offset + 8, &extent.type);
        readLe(bytes, offset + 12, &extent.targetData);
        readLe(bytes, offset + 20, &extent.targetSource);
        metadata->extents.push_back(extent);
    }
    const qsizetype blockOffset = tablesBase + blockDevicesOffset;
    readLe(bytes, blockOffset + 16, &metadata->blockDeviceSize);

    metadata->geometry = bytes.left(4096);
    metadata->metadataBlob = bytes.mid(headerOffset, headerSize + tablesSize);
    return true;
}

QByteArray buildMetadataImage(const VendorMetadata& metadata)
{
    QByteArray result(4096, '\0');
    result.append(metadata.geometry);
    result.append(metadata.geometry);
    QByteArray padded(static_cast<qsizetype>(metadata.metadataMaxSize), '\0');
    std::memcpy(padded.data(), metadata.metadataBlob.constData(),
                static_cast<size_t>(qMin(padded.size(), metadata.metadataBlob.size())));
    for (quint32 index = 0; index < metadata.metadataSlotCount * 2U; ++index) {
        result.append(padded);
    }
    return result;
}

} // namespace

quint64 DynamicSuperPlan::totalWriteBytes() const
{
    quint64 total = static_cast<quint64>(metadataImage.size());
    for (const DynamicSuperImage& image : images) {
        if (image.allocatedSize > std::numeric_limits<quint64>::max() - total) {
            return std::numeric_limits<quint64>::max();
        }
        total += image.allocatedSize;
    }
    return total;
}

bool DynamicSuperPlanner::discover(const QString& programXmlPath, QString* definitionPath,
                                   QString* error)
{
    const QString root = packageRootFor(programXmlPath);
    QString nvId;
    QFile pointer(QDir(root).filePath(QStringLiteral("IMAGES/super_meta.raw")));
    if (pointer.open(QIODevice::ReadOnly)) {
        const QString name = QString::fromUtf8(pointer.readAll()).trimmed();
        const QString prefix = QStringLiteral("super_meta.");
        const QString suffix = QStringLiteral(".raw");
        if (name.startsWith(prefix, Qt::CaseInsensitive) &&
            name.endsWith(suffix, Qt::CaseInsensitive)) {
            nvId = name.mid(prefix.size(), name.size() - prefix.size() - suffix.size());
        }
    }

    QDir metaDirectory(QDir(root).filePath(QStringLiteral("META")));
    if (!nvId.isEmpty()) {
        const QFileInfo exact(metaDirectory.filePath(
            QStringLiteral("super_def.%1.json").arg(nvId)));
        if (exact.isFile()) {
            if (definitionPath) *definitionPath = exact.canonicalFilePath();
            return true;
        }
    }
    const QFileInfoList definitions = metaDirectory.entryInfoList(
        {QStringLiteral("super_def*.json")}, QDir::Files, QDir::Name);
    if (!definitions.isEmpty()) {
        if (definitionPath) *definitionPath = definitions.first().canonicalFilePath();
        return true;
    }
    if (error) *error = QStringLiteral("未找到 META/super_def*.json");
    return false;
}

bool DynamicSuperPlanner::create(const QString& programXmlPath, const QString& definitionPath,
                                 DynamicSuperPlan* plan, QString* error)
{
    if (!plan) {
        if (error) *error = QStringLiteral("super 计划输出参数为空");
        return false;
    }
    *plan = {};
    QFile definition(definitionPath);
    if (!definition.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("打开 super_def 失败：%1").arg(definition.errorString());
        return false;
    }
    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(definition.readAll(), &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("解析 super_def 失败：%1").arg(jsonError.errorString());
        return false;
    }
    const QJsonObject rootObject = document.object();
    const QString packageRoot = packageRootFor(programXmlPath, definitionPath);
    const QJsonObject superMeta = rootObject.value(QStringLiteral("super_meta")).toObject();
    const QString metadataPath = resolvePackageFile(
        packageRoot, definitionPath, superMeta.value(QStringLiteral("path")).toString());
    if (metadataPath.isEmpty()) {
        if (error) *error = QStringLiteral("super_def 引用的 super_meta 文件不存在");
        return false;
    }

    VendorMetadata vendor;
    if (!parseVendorMetadata(metadataPath, &vendor, error)) return false;

    const QJsonArray blockDevices = rootObject.value(QStringLiteral("block_devices")).toArray();
    quint64 definedBlockSize = 0;
    if (blockDevices.size() != 1 ||
        !jsonUnsigned(blockDevices.first().toObject().value(QStringLiteral("size")),
                      &definedBlockSize) || definedBlockSize == 0 ||
        vendor.blockDeviceSize != definedBlockSize) {
        if (error) *error = QStringLiteral("super_def 与 super_meta 的块设备容量不一致");
        return false;
    }

    plan->definitionPath = QFileInfo(definitionPath).canonicalFilePath();
    plan->metadataPath = metadataPath;
    plan->metadataFilename = QFileInfo(metadataPath).fileName();
    plan->metadataImage = buildMetadataImage(vendor);
    plan->blockDeviceSize = definedBlockSize;
    plan->metadataSlotCount = vendor.metadataSlotCount;
    plan->metadataMajorVersion = vendor.majorVersion;
    plan->metadataMinorVersion = vendor.minorVersion;

    const QJsonArray partitions = rootObject.value(QStringLiteral("partitions")).toArray();
    for (const QJsonValue& value : partitions) {
        const QJsonObject object = value.toObject();
        if (!object.value(QStringLiteral("is_dynamic")).toBool() ||
            object.value(QStringLiteral("path")).toString().trimmed().isEmpty()) {
            continue;
        }
        DynamicSuperImage image;
        image.partitionName = object.value(QStringLiteral("name")).toString().trimmed();
        image.sourcePath = resolvePackageFile(packageRoot, definitionPath,
                                               object.value(QStringLiteral("path")).toString());
        if (image.partitionName.isEmpty() || image.sourcePath.isEmpty()) {
            if (error) *error = QStringLiteral("动态分区镜像不存在：%1").arg(image.partitionName);
            return false;
        }

        QFile source(image.sourcePath);
        if (!source.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("打开动态分区镜像失败：%1").arg(source.errorString());
            return false;
        }
        image.sparse = SparseImage::hasSparseMagic(source);
        if (image.sparse) {
            SparseImageInfo sparseInfo;
            if (!SparseImage::inspect(source, &sparseInfo, error)) return false;
            image.expandedSize = sparseInfo.expandedSize();
        } else {
            image.expandedSize = static_cast<quint64>(source.size());
        }

        const auto partitionIt = vendor.partitions.constFind(image.partitionName.toLower());
        if (partitionIt == vendor.partitions.cend()) {
            if (error) *error = QStringLiteral("super_meta 中缺少动态分区：%1")
                                    .arg(image.partitionName);
            return false;
        }
        const LpPartition& partition = partitionIt.value();
        quint64 logicalOffset = 0;
        for (quint32 index = 0; index < partition.extentCount; ++index) {
            const LpExtent& extent = vendor.extents.at(
                static_cast<qsizetype>(partition.firstExtent + index));
            if (extent.type != 0 || extent.targetSource != 0 ||
                extent.sectors > std::numeric_limits<quint64>::max() / LpSectorSize ||
                extent.targetData > std::numeric_limits<quint64>::max() / LpSectorSize) {
                if (error) *error = QStringLiteral("动态分区 extent 类型错误：%1")
                                        .arg(image.partitionName);
                return false;
            }
            DynamicSuperExtent mapped;
            mapped.logicalOffset = logicalOffset;
            mapped.physicalOffset = extent.targetData * LpSectorSize;
            mapped.byteCount = extent.sectors * LpSectorSize;
            if (mapped.byteCount > plan->blockDeviceSize ||
                mapped.physicalOffset > plan->blockDeviceSize - mapped.byteCount) {
                if (error) *error = QStringLiteral("动态分区 extent 超出 super：%1")
                                        .arg(image.partitionName);
                return false;
            }
            logicalOffset += mapped.byteCount;
            image.extents.push_back(mapped);
        }
        image.allocatedSize = logicalOffset;
        if (image.expandedSize > image.allocatedSize || image.allocatedSize == 0) {
            if (error) *error = QStringLiteral("动态分区镜像超出 extent：%1（%2 > %3）")
                                    .arg(image.partitionName)
                                    .arg(image.expandedSize).arg(image.allocatedSize);
            return false;
        }
        plan->images.push_back(std::move(image));
    }
    if (plan->images.isEmpty()) {
        if (error) *error = QStringLiteral("super_def 中没有带镜像的动态分区");
        return false;
    }
    return true;
}

} // namespace swt::edl
