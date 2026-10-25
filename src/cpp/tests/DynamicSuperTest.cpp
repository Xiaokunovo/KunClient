#include "storage/DynamicSuper.h"
#include "storage/RawProgramParser.h"
#include "storage/SparseImage.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>
#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

void putLe16(QByteArray& bytes, qsizetype offset, quint16 value)
{
    const quint16 little = qToLittleEndian(value);
    std::memcpy(bytes.data() + offset, &little, sizeof(little));
}

void putLe32(QByteArray& bytes, qsizetype offset, quint32 value)
{
    const quint32 little = qToLittleEndian(value);
    std::memcpy(bytes.data() + offset, &little, sizeof(little));
}

void putLe64(QByteArray& bytes, qsizetype offset, quint64 value)
{
    const quint64 little = qToLittleEndian(value);
    std::memcpy(bytes.data() + offset, &little, sizeof(little));
}

constexpr quint32 GeometryMagic = 0x616c4467;
constexpr quint32 MetadataMagic = 0x414c5030;

// ── 合成 super_meta ─────────────────────────────────────────────────────────
// 按 storage/DynamicSuper.cpp 的 parseVendorMetadata 口径逐字段构造：
// geometry（含 SHA256）+ metadata header（含 SHA256 + tables 哈希）+ 三张表。
// 目的是把 discover/create 的路径解析与计划生成测穿，而不是只测「编译通过」。

QByteArray buildGeometry(quint32 metadataMaxSize, quint32 slotCount)
{
    QByteArray geometry(4096, '\0');
    putLe32(geometry, 0, GeometryMagic);
    putLe32(geometry, 4, 52);
    putLe32(geometry, 40, metadataMaxSize);
    putLe32(geometry, 44, slotCount);
    putLe32(geometry, 48, 4096);
    const QByteArray digest = QCryptographicHash::hash(geometry.left(52),
                                                       QCryptographicHash::Sha256);
    std::memcpy(geometry.data() + 8, digest.constData(), 32);
    return geometry;
}

struct SyntheticPartition {
    QString name;
    quint32 firstExtent = 0;
    quint32 extentCount = 0;
};

struct SyntheticExtent {
    quint64 sectors = 0;
    quint32 type = 0;
    quint64 targetData = 0;
    quint32 targetSource = 0;
};

QByteArray buildSuperMeta(quint64 blockDeviceSize,
                          const QVector<SyntheticPartition>& partitions,
                          const QVector<SyntheticExtent>& extents,
                          quint32 metadataMaxSize = 512, quint32 slotCount = 2)
{
    const QByteArray geometry = buildGeometry(metadataMaxSize, slotCount);
    constexpr quint32 partitionEntrySize = 52;
    constexpr quint32 extentEntrySize = 24;
    constexpr quint32 blockDeviceEntrySize = 64;
    const quint32 partitionsOffset = 0;
    const quint32 extentsOffset = partitionsOffset +
        static_cast<quint32>(partitions.size()) * partitionEntrySize;
    const quint32 blockDevicesOffset = extentsOffset +
        static_cast<quint32>(extents.size()) * extentEntrySize;
    const quint32 tablesSize = blockDevicesOffset + blockDeviceEntrySize;

    QByteArray tables(tablesSize, '\0');
    for (qsizetype index = 0; index < partitions.size(); ++index) {
        const SyntheticPartition& partition = partitions.at(index);
        const qsizetype at = partitionsOffset + index * partitionEntrySize;
        const QByteArray name = partition.name.toLatin1().leftJustified(36, '\0', true);
        std::memcpy(tables.data() + at, name.constData(), 36);
        putLe32(tables, at + 40, partition.firstExtent);
        putLe32(tables, at + 44, partition.extentCount);
    }
    for (qsizetype index = 0; index < extents.size(); ++index) {
        const SyntheticExtent& extent = extents.at(index);
        const qsizetype at = extentsOffset + index * extentEntrySize;
        putLe64(tables, at, extent.sectors);
        putLe32(tables, at + 8, extent.type);
        putLe64(tables, at + 12, extent.targetData);
        putLe32(tables, at + 20, extent.targetSource);
    }
    putLe64(tables, blockDevicesOffset + 16, blockDeviceSize);

    QByteArray header(128, '\0');
    putLe32(header, 0, MetadataMagic);
    putLe16(header, 4, 10);
    putLe16(header, 6, 1);
    putLe32(header, 8, 128);
    putLe32(header, 44, tablesSize);
    const QByteArray tablesHash = QCryptographicHash::hash(tables,
                                                           QCryptographicHash::Sha256);
    std::memcpy(header.data() + 48, tablesHash.constData(), 32);
    putLe32(header, 80, partitionsOffset);
    putLe32(header, 84, static_cast<quint32>(partitions.size()));
    putLe32(header, 88, partitionEntrySize);
    putLe32(header, 92, extentsOffset);
    putLe32(header, 96, static_cast<quint32>(extents.size()));
    putLe32(header, 100, extentEntrySize);
    putLe32(header, 116, blockDevicesOffset);
    putLe32(header, 120, 1);
    putLe32(header, 124, blockDeviceEntrySize);
    const QByteArray headerHash = QCryptographicHash::hash(header,
                                                           QCryptographicHash::Sha256);
    std::memcpy(header.data() + 12, headerHash.constData(), 32);

    QByteArray blob = geometry;
    blob.append(header);
    blob.append(tables);
    return blob;
}

// ── 合成包辅助 ──────────────────────────────────────────────────────────────

constexpr quint64 BlockDeviceSize = 134217728;  // 128 MB
constexpr quint64 SystemBytes = 2097152;        // 2 MB（两个 1 MB extent）
constexpr quint64 VendorBytes = 1048576;        // 1 MB（一个 1 MB extent）
constexpr quint64 BootBytes = 1048576;          // 1 MB
// metadata 镜像 = 3 × 4096 几何副本 + slotCount(2) × 2 × metadataMaxSize(512)
constexpr qsizetype MetadataImageSize = 12288 + 2048;
constexpr quint64 DynamicTotalBytes = MetadataImageSize + SystemBytes + VendorBytes;

// system：extent0 → 物理 1 MB，extent1 → 物理 2 MB；vendor：extent2 → 物理 4 MB。
QVector<SyntheticPartition> standardPartitions()
{
    return {SyntheticPartition{QStringLiteral("system"), 0, 2},
            SyntheticPartition{QStringLiteral("vendor"), 2, 1}};
}

QVector<SyntheticExtent> standardExtents()
{
    return {SyntheticExtent{2048, 0, 2048, 0},
            SyntheticExtent{2048, 0, 4096, 0},
            SyntheticExtent{2048, 0, 8192, 0}};
}

QByteArray buildSuperDef(const QString& metaPath,
                         const QVector<QPair<QString, QString>>& dynamicPartitions,
                         const QVector<QPair<QString, QString>>& staticPartitions = {})
{
    QJsonObject root;
    QJsonObject meta;
    meta.insert(QStringLiteral("path"), metaPath);
    root.insert(QStringLiteral("super_meta"), meta);
    QJsonArray devices;
    QJsonObject device;
    device.insert(QStringLiteral("size"), QString::number(BlockDeviceSize));
    devices.append(device);
    root.insert(QStringLiteral("block_devices"), devices);
    QJsonArray partitions;
    for (const QPair<QString, QString>& partition : dynamicPartitions) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), partition.first);
        object.insert(QStringLiteral("path"), partition.second);
        object.insert(QStringLiteral("is_dynamic"), true);
        partitions.append(object);
    }
    for (const QPair<QString, QString>& partition : staticPartitions) {
        QJsonObject object;
        object.insert(QStringLiteral("name"), partition.first);
        object.insert(QStringLiteral("path"), partition.second);
        object.insert(QStringLiteral("is_dynamic"), false);
        partitions.append(object);
    }
    root.insert(QStringLiteral("partitions"), partitions);
    return QJsonDocument(root).toJson();
}

QByteArray buildRawProgramXml(const QVector<QPair<QString, QString>>& entries)
{
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<data>\n");
    for (const QPair<QString, QString>& entry : entries) {
        xml += QStringLiteral(
            "  <program SECTOR_SIZE_IN_BYTES=\"4096\" file_sector_offset=\"0\" filename=\"%1\""
            " label=\"%2\" num_partition_sectors=\"32768\" partofsingleimage=\"false\""
            " physical_partition_number=\"0\" readbackverify=\"false\" start_sector=\"8192\" />\n")
            .arg(entry.first, entry.second);
    }
    xml += QStringLiteral("</data>\n");
    return xml.toUtf8();
}

// 合法 Android sparse：DONT_CARE 跳过块 + RAW 数据块，展开 = totalBlocks × blockSize。
QByteArray buildSparseImage(quint32 blockSize, quint32 skipBlocks, quint32 rawBlocks, char fill)
{
    const quint32 totalBlocks = skipBlocks + rawBlocks;
    QByteArray image(28, '\0');
    putLe32(image, 0, swt::edl::SparseImage::Magic);
    putLe16(image, 4, 1);
    putLe16(image, 8, 28);
    putLe16(image, 10, 12);
    putLe32(image, 12, blockSize);
    putLe32(image, 16, totalBlocks);
    putLe32(image, 20, 2);

    QByteArray skip(12, '\0');
    putLe16(skip, 0, 0xCAC3);
    putLe32(skip, 4, skipBlocks);
    putLe32(skip, 8, 12);
    image.append(skip);

    QByteArray raw(12, '\0');
    putLe16(raw, 0, 0xCAC1);
    putLe32(raw, 4, rawBlocks);
    putLe32(raw, 8, 12 + rawBlocks * blockSize);
    image.append(raw);
    image.append(QByteArray(static_cast<qsizetype>(rawBlocks) * blockSize, fill));
    return image;
}

// 逐 chunk 合成 sparse：上面的 helper 只覆盖「skip + raw」固定两块，
// 缓冲读 / 合并语义需要任意 chunk 序列（FILL 值各异、掺 CRC、万级 chunk）。
// totalChunks 由调用方写头时给，这里只负责往尾部追加。
void appendSparseChunk(QByteArray& image, quint16 type, quint32 blocks,
                       const QByteArray& payload)
{
    QByteArray chunk(12, '\0');
    putLe16(chunk, 0, type);
    putLe32(chunk, 4, blocks);
    putLe32(chunk, 8, 12 + static_cast<quint32>(payload.size()));
    image.append(chunk);
    image.append(payload);
}

void writeSparseHeader(QByteArray& image, quint32 blockSize, quint32 totalBlocks,
                       quint32 totalChunks)
{
    image = QByteArray(28, '\0');
    putLe32(image, 0, swt::edl::SparseImage::Magic);
    putLe16(image, 4, 1);
    putLe16(image, 8, 28);
    putLe16(image, 10, 12);
    putLe32(image, 12, blockSize);
    putLe32(image, 16, totalBlocks);
    putLe32(image, 20, totalChunks);
}

struct Package {
    QTemporaryDir directory;
    QString root() const { return directory.path(); }
    QString path(const QString& relative) const { return QDir(root()).filePath(relative); }
    bool write(const QString& relative, const QByteArray& bytes)
    {
        const QString target = path(relative);
        QDir maker;
        if (!maker.mkpath(QFileInfo(target).absolutePath())) return false;
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly)) return false;
        return file.write(bytes) == bytes.size();
    }
};

bool containsText(const QStringList& texts, const QString& needle)
{
    for (const QString& text : texts) {
        if (text.contains(needle)) return true;
    }
    return false;
}

// 标准 META 包：META/super_meta.raw + META/super_def.json + image/{system,vendor}.img。
// metaPath / dynamicPaths 由调用方按被测布局给。
bool writeStandardMeta(Package& package, const QString& metaPath,
                       const QVector<QPair<QString, QString>>& dynamicPaths)
{
    return package.write(QStringLiteral("META/super_meta.raw"),
                         buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                        standardExtents())) &&
        package.write(QStringLiteral("META/super_def.json"),
                      buildSuperDef(metaPath, dynamicPaths,
                                    {{QStringLiteral("boot"), QStringLiteral("boot.img")}})) &&
        package.write(dynamicPaths.at(0).second, QByteArray(SystemBytes, 'S')) &&
        package.write(dynamicPaths.at(1).second, QByteArray(VendorBytes, 'V'));
}

} // namespace

// 免合并 super：META 发现（布局 / nvId 指针 / 上限）、触发条件（缺失 / 空文件）、
// 计划生成（extent 映射 / 总量 / sparse）。2026-10-14 用户场景：
// 「动态分区镜像 + META、无 super.img」必须能直接开刷。
int main()
{
    using swt::edl::DynamicSuperPlan;
    using swt::edl::DynamicSuperPlanner;
    using swt::edl::RawProgramParser;
    using swt::edl::RawProgramPlan;

    // ── 1. 经典布局：<root>/rawprogram0.xml + <root>/META/ ────────────────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(writeStandardMeta(package, QStringLiteral("META/super_meta.raw"),
                                {{QStringLiteral("system"), QStringLiteral("image/system.img")},
                                 {QStringLiteral("vendor"), QStringLiteral("image/vendor.img")}}),
              "standard META package must be writable");
        check(package.write(QStringLiteral("boot.img"), QByteArray(BootBytes, 'B')),
              "boot image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")},
                                                    {QStringLiteral("boot.img"),
                                                     QStringLiteral("boot")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        const bool discovered = DynamicSuperPlanner::discover(xml, &definition, &error);
        check(discovered, "discover must find META/super_def.json next to the XML");
        if (!discovered) {
            std::cerr << "  discover error: " << error.toUtf8().constData() << '\n';
        }
        check(QFileInfo(definition).fileName() == QStringLiteral("super_def.json"),
              "discover must return the super_def file");

        RawProgramPlan plan;
        const bool parsed = RawProgramParser::parse({xml}, &plan, &error);
        check(parsed, "parse must succeed when super.img is missing but META exists");
        if (!parsed) {
            std::cerr << "  parse error: " << error.toUtf8().constData() << '\n';
        }
        check(plan.entries.size() == 2, "plan must keep the super and boot entries");
        check(plan.dynamicSuperCount == 1 && plan.superEntryCount == 1,
              "the missing super.img entry must become a dynamic super entry");
        check(plan.hasSplitSuper(), "a dynamic super entry counts as split super");
        check(plan.entries.at(0).dynamicSuper, "the super entry must be flagged dynamicSuper");
        check(plan.entries.at(0).sourcePath == definition,
              "the dynamic entry must point at super_def.json");
        check(!plan.entries.at(1).dynamicSuper, "boot must stay a normal entry");
        check(plan.totalSourceBytes == DynamicTotalBytes + BootBytes,
              "total source bytes must be metadata + images + boot");
        check(containsText(plan.warnings, QStringLiteral("免合并计划")),
              "parse must warn that the META plan is in use");
        check(containsText(plan.warnings, QStringLiteral("super.img 缺失")),
              "the warning must say super.img is missing (not present-but-empty)");

        DynamicSuperPlan dynamicPlan;
        const bool created = DynamicSuperPlanner::create(xml, definition, &dynamicPlan, &error);
        check(created, "create must build a plan from the synthetic super_meta");
        if (!created) {
            std::cerr << "  create error: " << error.toUtf8().constData() << '\n';
        }
        check(dynamicPlan.images.size() == 2, "plan must hold both dynamic partitions");
        check(dynamicPlan.images.at(0).partitionName == QStringLiteral("system"),
              "system must be the first image");
        check(dynamicPlan.images.at(0).allocatedSize == SystemBytes,
              "system allocation must equal the sum of its extents");
        check(dynamicPlan.images.at(0).extents.size() == 2, "system must have two extents");
        check(dynamicPlan.images.at(0).extents.at(0).physicalOffset == 1048576 &&
                  dynamicPlan.images.at(0).extents.at(0).byteCount == 1048576,
              "system extent 0 must map to physical 1 MB");
        check(dynamicPlan.images.at(0).extents.at(0).logicalOffset == 0 &&
                  dynamicPlan.images.at(0).extents.at(1).logicalOffset == 1048576,
              "logical offsets must accumulate per extent");
        check(dynamicPlan.images.at(0).extents.at(1).physicalOffset == 2097152,
              "system extent 1 must map to physical 2 MB");
        check(dynamicPlan.images.at(1).partitionName == QStringLiteral("vendor"),
              "vendor must be the second image");
        check(dynamicPlan.images.at(1).extents.size() == 1 &&
                  dynamicPlan.images.at(1).extents.at(0).physicalOffset == 4194304,
              "vendor extent must map to physical 4 MB");
        check(dynamicPlan.images.at(0).sourcePath ==
                  QFileInfo(package.path(QStringLiteral("image/system.img"))).canonicalFilePath(),
              "system image path must resolve through the package root");
        check(!dynamicPlan.images.at(0).sparse, "the raw system image must not be sparse");
        check(dynamicPlan.metadataImage.size() == MetadataImageSize,
              "metadata image size must match the geometry + slot layout");
        check(dynamicPlan.blockDeviceSize == BlockDeviceSize,
              "block device size must come from super_meta");
        check(dynamicPlan.totalWriteBytes() == DynamicTotalBytes,
              "total write bytes must be metadata + every dynamic image");
    }

    // ── 2. 真实高通布局：<root>/image/rawprogram0.xml + <root>/META/ ───────────
    // TBJ607F 实测包就是 image/（小写）+ META/，老逻辑在这种布局下找不到 META。
    // super_def 里写裸文件名（system.img），镜像与 XML 同在 image/ —— 解析必须
    // 回落到 XML 目录候选。
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("image/rawprogram0.xml"));
        check(package.write(QStringLiteral("META/super_meta.raw"),
                            buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                           standardExtents())),
              "image-layout super_meta must be writable");
        check(package.write(QStringLiteral("META/super_def.json"),
                            buildSuperDef(QStringLiteral("META/super_meta.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("system.img")},
                                           {QStringLiteral("vendor"),
                                            QStringLiteral("vendor.img")}})),
              "image-layout super_def must be writable");
        check(package.write(QStringLiteral("image/system.img"),
                            QByteArray(SystemBytes, 'S')),
              "system image must be writable next to the XML");
        check(package.write(QStringLiteral("image/vendor.img"),
                            QByteArray(VendorBytes, 'V')),
              "vendor image must be writable next to the XML");
        check(package.write(QStringLiteral("image/boot.img"), QByteArray(BootBytes, 'B')),
              "boot image must be writable next to the XML");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")},
                                                    {QStringLiteral("boot.img"),
                                                     QStringLiteral("boot")}})),
              "image-layout rawprogram xml must be writable");

        QString definition;
        QString error;
        check(DynamicSuperPlanner::discover(xml, &definition, &error),
              "discover must walk up from image/ to the package root META");
        check(QFileInfo(definition).fileName() == QStringLiteral("super_def.json"),
              "discover must find the META definition above image/");

        RawProgramPlan plan;
        const bool parsed = RawProgramParser::parse({xml}, &plan, &error);
        check(parsed, "parse must succeed in the image/ layout");
        check(plan.dynamicSuperCount == 1,
              "the dynamic plan must trigger in the image/ layout too");

        DynamicSuperPlan dynamicPlan;
        const bool created = DynamicSuperPlanner::create(xml, definition, &dynamicPlan,
                                                         &error);
        check(created, "create must resolve bare image names next to the XML");
        check(created && dynamicPlan.images.at(0).sourcePath ==
                  QFileInfo(package.path(QStringLiteral("image/system.img")))
                      .canonicalFilePath(),
              "bare super_def paths must resolve in the XML directory");
    }

    // ── 3. 空 super.img（0 字节占位）必须触发免合并 ───────────────────────────
    // 老逻辑只在文件缺失时触发；空文件会一路走到「镜像偏移超出文件」硬失败。
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(writeStandardMeta(package, QStringLiteral("META/super_meta.raw"),
                                {{QStringLiteral("system"), QStringLiteral("image/system.img")},
                                 {QStringLiteral("vendor"), QStringLiteral("image/vendor.img")}}),
              "empty-super META package must be writable");
        check(package.write(QStringLiteral("boot.img"), QByteArray(BootBytes, 'B')),
              "boot image must be writable");
        check(package.write(QStringLiteral("super.img"), QByteArray()),
              "the 0-byte super.img placeholder must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")},
                                                    {QStringLiteral("boot.img"),
                                                     QStringLiteral("boot")}})),
              "rawprogram xml must be writable");

        RawProgramPlan plan;
        QString error;
        const bool parsed = RawProgramParser::parse({xml}, &plan, &error);
        check(parsed, "parse must succeed with a 0-byte super.img placeholder");
        if (!parsed) {
            std::cerr << "  parse error: " << error.toUtf8().constData() << '\n';
        }
        check(plan.dynamicSuperCount == 1,
              "a 0-byte super.img must trigger the META plan");
        check(containsText(plan.warnings, QStringLiteral("super.img 为空")),
              "the warning must say super.img is empty");
        check(plan.entries.at(0).dynamicSuper, "the empty super entry must be dynamic");
    }

    // ── 4. 非 super 的 0 字节镜像必须硬失败（fail-closed）──────────────────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(package.write(QStringLiteral("boot.img"), QByteArray()),
              "the 0-byte boot.img must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("boot.img"),
                                                     QStringLiteral("boot")}})),
              "rawprogram xml must be writable");

        RawProgramPlan plan;
        QString error;
        check(!RawProgramParser::parse({xml}, &plan, &error),
              "a 0-byte non-super image must fail the parse");
        check(error.contains(QStringLiteral("0 字节")),
              "the failure must name the 0-byte image");
    }

    // ── 5. 没有 META：缺失 super.img 只能跳过并警告 ───────────────────────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(package.write(QStringLiteral("boot.img"), QByteArray(BootBytes, 'B')),
              "boot image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")},
                                                    {QStringLiteral("boot.img"),
                                                     QStringLiteral("boot")}})),
              "rawprogram xml must be writable");

        RawProgramPlan plan;
        QString error;
        const bool parsed = RawProgramParser::parse({xml}, &plan, &error);
        check(parsed, "parse must still succeed when super.img is missing and no META exists");
        check(plan.entries.size() == 1 && !plan.entries.at(0).dynamicSuper,
              "only boot may remain when there is no META");
        check(plan.dynamicSuperCount == 0, "no META means no dynamic plan");
        check(containsText(plan.warnings, QStringLiteral("跳过缺失镜像")),
              "the missing super.img must be reported as skipped");

        QString definition;
        check(!DynamicSuperPlanner::discover(xml, &definition, &error),
              "discover must fail when no super_def exists anywhere");
        check(error.contains(QStringLiteral("未找到")),
              "discover must report that no super_def was found");
    }

    // ── 6. super_meta 损坏：免合并必须失败并回退为跳过 ────────────────────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        QByteArray meta = buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                         standardExtents());
        meta[0] = static_cast<char>(meta[0] ^ 0xFF);  // 破坏 geometry magic
        check(package.write(QStringLiteral("META/super_meta.raw"), meta),
              "corrupt super_meta must be writable");
        check(package.write(QStringLiteral("META/super_def.json"),
                            buildSuperDef(QStringLiteral("META/super_meta.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("image/system.img")},
                                           {QStringLiteral("vendor"),
                                            QStringLiteral("image/vendor.img")}})),
              "super_def must be writable");
        check(package.write(QStringLiteral("image/system.img"),
                            QByteArray(SystemBytes, 'S')),
              "system image must be writable");
        check(package.write(QStringLiteral("image/vendor.img"),
                            QByteArray(VendorBytes, 'V')),
              "vendor image must be writable");
        check(package.write(QStringLiteral("boot.img"), QByteArray(BootBytes, 'B')),
              "boot image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")},
                                                    {QStringLiteral("boot.img"),
                                                     QStringLiteral("boot")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        check(DynamicSuperPlanner::discover(xml, &definition, &error),
              "discover still finds super_def even when super_meta is corrupt");

        DynamicSuperPlan dynamicPlan;
        check(!DynamicSuperPlanner::create(xml, definition, &dynamicPlan, &error),
              "create must reject a corrupt super_meta");
        check(error.contains(QStringLiteral("super_meta")),
              "the create failure must mention super_meta");

        RawProgramPlan plan;
        const bool parsed = RawProgramParser::parse({xml}, &plan, &error);
        check(parsed, "parse must fall back to skipping when the plan cannot be built");
        check(plan.dynamicSuperCount == 0,
              "a corrupt super_meta must not produce a dynamic plan");
        check(containsText(plan.warnings, QStringLiteral("跳过缺失镜像")),
              "the fallback must be reported as a skipped image");
    }

    // ── 7. nvId 指针文件：IMAGES/super_meta.raw 指定 super_def.<nvId>.json ─────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(package.write(QStringLiteral("IMAGES/super_meta.raw"),
                            QByteArray("super_meta.9.raw\n")),
              "the nvId pointer file must be writable");
        check(package.write(QStringLiteral("META/super_meta.raw"),
                            buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                           standardExtents())),
              "the real super_meta must be writable");
        check(package.write(QStringLiteral("META/super_def.9.json"),
                            buildSuperDef(QStringLiteral("META/super_meta.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("image/system.img")},
                                           {QStringLiteral("vendor"),
                                            QStringLiteral("image/vendor.img")}})),
              "the nvId definition must be writable");
        // 诱饵：nvId 不同且镜像不存在，若被选中 create 必失败
        check(package.write(QStringLiteral("META/super_def.7.json"),
                            buildSuperDef(QStringLiteral("META/super_meta.raw"),
                                          {{QStringLiteral("decoy"),
                                            QStringLiteral("image/decoy.img")}})),
              "the decoy definition must be writable");
        check(package.write(QStringLiteral("image/system.img"),
                            QByteArray(SystemBytes, 'S')),
              "system image must be writable");
        check(package.write(QStringLiteral("image/vendor.img"),
                            QByteArray(VendorBytes, 'V')),
              "vendor image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        check(DynamicSuperPlanner::discover(xml, &definition, &error),
              "discover must find the nvId definition");
        check(QFileInfo(definition).fileName() == QStringLiteral("super_def.9.json"),
              "discover must honour the nvId pointer over alphabetical order");

        DynamicSuperPlan dynamicPlan;
        check(DynamicSuperPlanner::create(xml, definition, &dynamicPlan, &error),
              "create must succeed with the nvId-selected definition");
    }

    // ── 8. super_1.img 分段命名同样触发免合并 ─────────────────────────────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(writeStandardMeta(package, QStringLiteral("META/super_meta.raw"),
                                {{QStringLiteral("system"), QStringLiteral("image/system.img")},
                                 {QStringLiteral("vendor"), QStringLiteral("image/vendor.img")}}),
              "split-name META package must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super_1.img"),
                                                     QStringLiteral("super")}})),
              "rawprogram xml must be writable");

        RawProgramPlan plan;
        QString error;
        check(RawProgramParser::parse({xml}, &plan, &error),
              "parse must succeed with a missing super_1.img");
        check(plan.dynamicSuperCount == 1,
              "super_1.img must be recognised as a super entry");
    }

    // ── 9. 向上查找上限：最多两层，且必须命中 super_def ────────────────────────
    {
        Package near;
        check(near.directory.isValid(), "temp package directory must be valid");
        const QString nearXml = near.path(QStringLiteral("a/b/c/rawprogram0.xml"));
        check(writeStandardMeta(near, QStringLiteral("META/super_meta.raw"),
                                {{QStringLiteral("system"), QStringLiteral("system.img")},
                                 {QStringLiteral("vendor"), QStringLiteral("vendor.img")}}),
              "near-layout META package must be writable");
        // 上移一层：镜像/定义放进 <root>/a/b/c/
        check(near.write(QStringLiteral("a/b/c/META/super_meta.raw"),
                         buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                        standardExtents())),
              "near super_meta must be writable");
        check(near.write(QStringLiteral("a/b/c/META/super_def.json"),
                         buildSuperDef(QStringLiteral("META/super_meta.raw"),
                                       {{QStringLiteral("system"),
                                         QStringLiteral("system.img")},
                                        {QStringLiteral("vendor"),
                                         QStringLiteral("vendor.img")}})),
              "near super_def must be writable");
        check(near.write(QStringLiteral("a/b/c/system.img"), QByteArray(SystemBytes, 'S')),
              "near system image must be writable");
        check(near.write(QStringLiteral("a/b/c/vendor.img"), QByteArray(VendorBytes, 'V')),
              "near vendor image must be writable");
        check(near.write(nearXml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                      QStringLiteral("super")}})),
              "near rawprogram xml must be writable");

        QString definition;
        QString error;
        check(DynamicSuperPlanner::discover(nearXml, &definition, &error),
              "discover must find the META one level above the XML");
        check(definition ==
                  QFileInfo(near.path(QStringLiteral("a/b/c/META/super_def.json")))
                      .canonicalFilePath(),
              "the closest META must win over the package root META");
    }
    {
        Package far;
        check(far.directory.isValid(), "temp package directory must be valid");
        const QString farXml = far.path(QStringLiteral("a/b/c/d/rawprogram0.xml"));
        check(writeStandardMeta(far, QStringLiteral("META/super_meta.raw"),
                                {{QStringLiteral("system"), QStringLiteral("system.img")},
                                 {QStringLiteral("vendor"), QStringLiteral("vendor.img")}}),
              "far-layout META package must be writable");
        check(far.write(farXml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                    QStringLiteral("super")}})),
              "far rawprogram xml must be writable");

        QString definition;
        QString error;
        check(!DynamicSuperPlanner::discover(farXml, &definition, &error),
              "discover must not walk up more than two levels");
    }

    // ── 10. 就近优先：XML 目录里的 super_def 盖过上层的 META 诱饵 ──────────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("image/rawprogram0.xml"));
        check(package.write(QStringLiteral("image/super_def.json"),
                            buildSuperDef(QStringLiteral("super_meta.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("system.img")},
                                           {QStringLiteral("vendor"),
                                            QStringLiteral("vendor.img")}})),
              "the XML-directory super_def must be writable");
        check(package.write(QStringLiteral("image/super_meta.raw"),
                            buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                           standardExtents())),
              "the XML-directory super_meta must be writable");
        check(package.write(QStringLiteral("image/system.img"),
                            QByteArray(SystemBytes, 'S')),
              "system image must be writable");
        check(package.write(QStringLiteral("image/vendor.img"),
                            QByteArray(VendorBytes, 'V')),
              "vendor image must be writable");
        // 上层诱饵：块设备容量不一致，若被选中 create 必失败
        QByteArray decoy = buildSuperDef(QStringLiteral("super_meta.raw"),
                                         {{QStringLiteral("decoy"),
                                           QStringLiteral("decoy.img")}});
        decoy.replace(QByteArray("134217728"), QByteArray("999"));
        check(package.write(QStringLiteral("META/super_def.json"), decoy),
              "the decoy definition must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        check(DynamicSuperPlanner::discover(xml, &definition, &error),
              "discover must accept a super_def sitting next to the XML");
        check(definition == QFileInfo(package.path(QStringLiteral("image/super_def.json")))
                                .canonicalFilePath(),
              "the closest super_def must beat the decoy META above");

        DynamicSuperPlan dynamicPlan;
        check(DynamicSuperPlanner::create(xml, definition, &dynamicPlan, &error),
              "create must use the XML-directory definition, not the decoy");
        check(dynamicPlan.images.size() == 2, "the chosen definition has two images");
    }

    // ── 11. 动态分区镜像本身是 sparse：expandedSize 仍按 extent 校验 ───────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(package.write(QStringLiteral("META/super_meta.raw"),
                            buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                           standardExtents())),
              "super_meta must be writable");
        check(package.write(QStringLiteral("META/super_def.json"),
                            buildSuperDef(QStringLiteral("META/super_meta.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("image/system.img")},
                                           {QStringLiteral("vendor"),
                                            QStringLiteral("image/vendor.img")}})),
              "super_def must be writable");
        // 4096 字节块、512 块 = 2 MB 展开，与 system 的 extent 总量一致
        check(package.write(QStringLiteral("image/system.img"),
                            buildSparseImage(4096, 256, 256, 'S')),
              "the sparse system image must be writable");
        check(package.write(QStringLiteral("image/vendor.img"),
                            QByteArray(VendorBytes, 'V')),
              "vendor image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        DynamicSuperPlan dynamicPlan;
        const bool created =
            DynamicSuperPlanner::discover(xml, &definition, &error) &&
            DynamicSuperPlanner::create(xml, definition, &dynamicPlan, &error);
        check(created, "create must accept a sparse dynamic image");
        if (!created) {
            std::cerr << "  create error: " << error.toUtf8().constData() << '\n';
        }
        check(created && dynamicPlan.images.at(0).sparse,
              "the sparse system image must be flagged sparse");
        check(created && dynamicPlan.images.at(0).expandedSize == SystemBytes,
              "expanded size must come from the sparse header");
        check(created && dynamicPlan.images.at(0).allocatedSize == SystemBytes,
              "allocation must still come from the extents");
    }

    // ── 12. SparseImage::coalesce：相邻同类合并的纯函数语义 ──────────────
    // 合并错一个字节就是刷错数据，这里把每种「不能合」的情形各钉一条。
    {
        using swt::edl::SparseExtent;
        using swt::edl::SparseExtentType;
        constexpr quint32 BlockSize = 4096;
        auto extent = [](SparseExtentType type, quint64 outputBlock, quint64 blockCount,
                         quint64 fileOffset = 0, quint32 fillValue = 0) {
            SparseExtent value;
            value.type = type;
            value.outputBlock = outputBlock;
            value.blockCount = blockCount;
            value.fileOffset = fileOffset;
            value.fillValue = fillValue;
            return value;
        };

        // ① 相邻 RAW 且文件侧首尾相接 → 并成一段，块数累加、偏移取首段
        const QVector<SparseExtent> rawRun{
            extent(SparseExtentType::Raw, 0, 2, 28),
            extent(SparseExtentType::Raw, 2, 3, 28 + 2 * BlockSize),
            extent(SparseExtentType::Raw, 5, 1, 28 + 5 * BlockSize),
        };
        const QVector<SparseExtent> rawMerged =
            swt::edl::SparseImage::coalesce(rawRun, BlockSize);
        check(rawMerged.size() == 1,
              "contiguous RAW chunks with contiguous file offsets must merge");
        check(rawMerged.size() == 1 && rawMerged.first().blockCount == 6,
              "the merged RAW extent must sum the block counts");
        check(rawMerged.size() == 1 && rawMerged.first().fileOffset == 28,
              "the merged RAW extent must keep the first file offset");

        // ② 输出相邻但文件侧有洞 → 不合并（一次顺序读会串入别处数据）
        const QVector<SparseExtent> rawHole{
            extent(SparseExtentType::Raw, 0, 2, 28),
            extent(SparseExtentType::Raw, 2, 2, 28 + 3 * BlockSize),
        };
        check(swt::edl::SparseImage::coalesce(rawHole, BlockSize).size() == 2,
              "RAW chunks with a file-side hole must not merge");

        // ③ 同值 FILL 并、异值 FILL 不并
        const QVector<SparseExtent> fillSame{
            extent(SparseExtentType::Fill, 0, 4, 0, 0x11223344),
            extent(SparseExtentType::Fill, 4, 6, 0, 0x11223344),
        };
        const QVector<SparseExtent> fillMerged =
            swt::edl::SparseImage::coalesce(fillSame, BlockSize);
        check(fillMerged.size() == 1 && fillMerged.first().blockCount == 10,
              "same-value FILL chunks must merge");
        const QVector<SparseExtent> fillMixed{
            extent(SparseExtentType::Fill, 0, 4, 0, 0x11223344),
            extent(SparseExtentType::Fill, 4, 6, 0, 0xAABBCCDD),
        };
        check(swt::edl::SparseImage::coalesce(fillMixed, BlockSize).size() == 2,
              "different-value FILL chunks must not merge");

        // ④ 输出不相邻 → 不合并（中间隔着 DONT_CARE 或空洞）
        const QVector<SparseExtent> gap{
            extent(SparseExtentType::Raw, 0, 2, 28),
            extent(SparseExtentType::Raw, 4, 2, 28 + 2 * BlockSize),
        };
        check(swt::edl::SparseImage::coalesce(gap, BlockSize).size() == 2,
              "extents with an output gap must not merge");

        // ⑤ DONT_CARE 直接丢弃；它造成的空洞同时阻止两侧 RAW 合并
        const QVector<SparseExtent> withSkip{
            extent(SparseExtentType::Raw, 0, 2, 28),
            extent(SparseExtentType::Skip, 2, 3),
            extent(SparseExtentType::Raw, 5, 1, 28 + 2 * BlockSize),
        };
        const QVector<SparseExtent> skipMerged =
            swt::edl::SparseImage::coalesce(withSkip, BlockSize);
        check(skipMerged.size() == 2, "DONT_CARE extents must be dropped");
        check(skipMerged.size() == 2 &&
                  skipMerged.at(0).type == SparseExtentType::Raw &&
                  skipMerged.at(1).type == SparseExtentType::Raw,
              "only RAW extents must survive the coalesce");

        // ⑥ blockSize=0 不除零崩溃（public API 的 fail-closed：RAW 一律不合并）
        const QVector<SparseExtent> zeroGuard{
            extent(SparseExtentType::Raw, 0, 2, 28),
            extent(SparseExtentType::Raw, 2, 2, 28 + 2 * BlockSize),
        };
        const QVector<SparseExtent> zeroMerged =
            swt::edl::SparseImage::coalesce(zeroGuard, 0);
        check(zeroMerged.size() == 2,
              "blockSize=0 must not merge RAW (division-by-zero guard)");
    }

    // ── 13. SparseImage::inspect：64KB 顺序缓冲跨 refill 的正确性 ───────
    // 6000 个 FILL chunk（16 B/个 = 96 KB）必然把 64KB 缓冲顶穿至少一次；
    // 掺 DONT_CARE、CRC、首尾 RAW，抽验首/中/尾 chunk 的解析结果。
    {
        constexpr quint32 BlockSize = 4096;
        constexpr int FillChunks = 6000;
        QByteArray image;
        // RAW 1 块 + DONT_CARE 2 块 + 6000 × FILL 1 块 + RAW 1 块 + CRC 1 个
        const quint32 totalBlocks = 1 + 2 + FillChunks + 1;
        writeSparseHeader(image, BlockSize, totalBlocks, FillChunks + 4);
        appendSparseChunk(image, 0xCAC1, 1, QByteArray(BlockSize, 'R'));
        appendSparseChunk(image, 0xCAC3, 2, QByteArray{});
        for (int i = 0; i < FillChunks; ++i) {
            QByteArray payload(4, '\0');
            putLe32(payload, 0, static_cast<quint32>(0x10000 + i));
            appendSparseChunk(image, 0xCAC2, 1, payload);
        }
        appendSparseChunk(image, 0xCAC1, 1, QByteArray(BlockSize, 'T'));
        appendSparseChunk(image, 0xCAC4, 0, QByteArray(4, '\0'));

        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString imagePath = package.path(QStringLiteral("chunky.img"));
        check(package.write(QStringLiteral("chunky.img"), image),
              "the 6000-chunk sparse image must be writable");
        check(image.size() > 64 * 1024,
              "the synthetic image must exceed the 64KB chunk buffer");

        QFile file(imagePath);
        check(file.open(QIODevice::ReadOnly), "the sparse image must be openable");
        swt::edl::SparseImageInfo info;
        QString error;
        const bool inspected = swt::edl::SparseImage::inspect(file, &info, &error);
        check(inspected, "inspect must parse the 6000-chunk sparse image");
        if (!inspected) {
            std::cerr << "  inspect error: " << error.toUtf8().constData() << '\n';
        }
        check(inspected && info.blockSize == BlockSize, "block size must round-trip");
        check(inspected && info.totalBlocks == totalBlocks,
              "total blocks must round-trip");
        check(inspected && info.extents.size() == FillChunks + 3,
              "CRC chunk must not become an extent");
        check(inspected && info.expandedSize() ==
                  static_cast<quint64>(totalBlocks) * BlockSize,
              "expanded size must come from the header");

        // extents: [0]=RAW(1) [1]=Skip(2) [2..6001]=FILL(1) [6002]=RAW(1)
        check(inspected && info.extents.at(0).type == swt::edl::SparseExtentType::Raw &&
                  info.extents.at(0).blockCount == 1,
              "the first RAW chunk must be parsed");
        check(inspected && info.extents.at(0).fileOffset == 28 + 12,
              "the first RAW file offset must point past its chunk header");
        check(inspected && info.extents.at(1).type == swt::edl::SparseExtentType::Skip,
              "the DONT_CARE chunk must be kept as a Skip extent");
        for (int probe : {0, 1, 2999, FillChunks - 1}) {
            if (!inspected) break;  // 解析失败时 extents 可能为空，.at() 会直接 abort
            const swt::edl::SparseExtent& fill = info.extents.at(2 + probe);
            const bool ok = fill.type == swt::edl::SparseExtentType::Fill &&
                fill.blockCount == 1 &&
                fill.outputBlock == 3 + static_cast<quint64>(probe) &&
                fill.fillValue == static_cast<quint32>(0x10000 + probe);
            check(ok, "every FILL chunk must keep its own value across buffer refills");
            if (!ok) {
                std::cerr << "  fill probe " << probe << ": block="
                          << fill.outputBlock << " value=0x" << std::hex
                          << fill.fillValue << std::dec << '\n';
            }
        }
        check(inspected && info.extents.back().type == swt::edl::SparseExtentType::Raw &&
                  info.extents.back().blockCount == 1,
              "the trailing RAW chunk must be parsed");
        check(inspected && info.extents.back().fileOffset ==
                  28 + (12 + BlockSize) + 12 +
                      static_cast<quint64>(FillChunks) * 16 + 12,
              "the trailing RAW file offset must account for all preceding chunks");
        file.close();

        // 错误路径：chunk 声明长度越过文件尾必须报错，不能静默少解析
        QByteArray truncated;
        writeSparseHeader(truncated, BlockSize, 4, 2);
        appendSparseChunk(truncated, 0xCAC1, 2, QByteArray(2 * BlockSize, 'R'));
        appendSparseChunk(truncated, 0xCAC1, 2, QByteArray(2 * BlockSize, 'R'));
        truncated.chop(64);  // 第二个 chunk 的数据被截掉
        Package broken;
        check(broken.directory.isValid(), "temp package directory must be valid");
        check(broken.write(QStringLiteral("broken.img"), truncated),
              "the truncated sparse image must be writable");
        QFile brokenFile(broken.path(QStringLiteral("broken.img")));
        check(brokenFile.open(QIODevice::ReadOnly), "the truncated image must be openable");
        swt::edl::SparseImageInfo brokenInfo;
        QString brokenError;
        check(!swt::edl::SparseImage::inspect(brokenFile, &brokenInfo, &brokenError),
              "inspect must reject a chunk that overruns the file tail");
        check(brokenError.contains(QStringLiteral("长度越界")),
              "the overrun error must be diagnosable");
        brokenFile.close();
    }

    // ── 14. extent 必须 4096 块对齐（参考 edl9008 superlp.cpp 的 targetData%8==0）
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        // extent 从 2049 扇区起 = 1046528 字节，不是 4096 的倍数
        const QVector<SyntheticExtent> misaligned{
            SyntheticExtent{2048, 0, 2049, 0},
        };
        check(package.write(QStringLiteral("META/super_meta.raw"),
                            buildSuperMeta(BlockDeviceSize,
                                           {SyntheticPartition{QStringLiteral("system"), 0, 1}},
                                           misaligned)),
              "misaligned super_meta must be writable");
        check(package.write(QStringLiteral("META/super_def.json"),
                            buildSuperDef(QStringLiteral("META/super_meta.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("image/system.img")}})),
              "misaligned super_def must be writable");
        check(package.write(QStringLiteral("image/system.img"),
                            QByteArray(SystemBytes, 'S')),
              "system image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        DynamicSuperPlan dynamicPlan;
        const bool created =
            DynamicSuperPlanner::discover(xml, &definition, &error) &&
            DynamicSuperPlanner::create(xml, definition, &dynamicPlan, &error);
        check(!created, "create must reject an extent that is not 4096-aligned");
        check(error.contains(QStringLiteral("4096")),
              "the rejection must name the alignment requirement");
    }

    // ── 15. 无指针文件时：挑最大的 super_meta.*.raw 反推 nvId ──────────────────
    // 参考 edl9008 superlp.cpp findSuperMeta：不挑目录位置，只挑「> 4096B 的最大者」。
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        // 诱饵：小的假 meta（< 4096，像指针文件），放在 IMAGES/
        check(package.write(QStringLiteral("IMAGES/super_meta.raw"),
                            QByteArray("super_meta.5.raw\n")),
              "the pointer-sized decoy must be writable");
        // 真正的 meta 放 image/，文件名带 nv 5，> 4096
        check(package.write(QStringLiteral("image/super_meta.5.raw"),
                            buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                           standardExtents())),
              "the real nv5 meta must be writable");
        check(package.write(QStringLiteral("META/super_def.5.json"),
                            buildSuperDef(QStringLiteral("image/super_meta.5.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("image/system.img")},
                                           {QStringLiteral("vendor"),
                                            QStringLiteral("image/vendor.img")}})),
              "the nv5 definition must be writable");
        // nv 全零的占位定义（无对应镜像）绝不能被选中
        check(package.write(QStringLiteral("META/super_def.00000000.json"),
                            buildSuperDef(QStringLiteral("image/super_meta.5.raw"),
                                          {{QStringLiteral("decoy"),
                                            QStringLiteral("image/decoy.img")}})),
              "the zero-nv decoy must be writable");
        check(package.write(QStringLiteral("image/system.img"),
                            QByteArray(SystemBytes, 'S')),
              "system image must be writable");
        check(package.write(QStringLiteral("image/vendor.img"),
                            QByteArray(VendorBytes, 'V')),
              "vendor image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        check(DynamicSuperPlanner::discover(xml, &definition, &error),
              "discover must fall back to the largest super_meta when no pointer names it");
        check(QFileInfo(definition).fileName() == QStringLiteral("super_def.5.json"),
              "the nv5 definition must win over the zero-nv placeholder");

        DynamicSuperPlan dynamicPlan;
        check(DynamicSuperPlanner::create(xml, definition, &dynamicPlan, &error),
              "create must succeed with the largest-meta fallback");
    }

    // ── 16. nv 全零占位兜底：只有 00000000 一个 def 时也放行 ─────────────────
    {
        Package package;
        check(package.directory.isValid(), "temp package directory must be valid");
        const QString xml = package.path(QStringLiteral("rawprogram0.xml"));
        check(package.write(QStringLiteral("META/super_meta.00000000.raw"),
                            buildSuperMeta(BlockDeviceSize, standardPartitions(),
                                           standardExtents())),
              "the zero-nv meta must be writable");
        check(package.write(QStringLiteral("META/super_def.00000000.json"),
                            buildSuperDef(QStringLiteral("super_meta.00000000.raw"),
                                          {{QStringLiteral("system"),
                                            QStringLiteral("image/system.img")},
                                           {QStringLiteral("vendor"),
                                            QStringLiteral("image/vendor.img")}})),
              "the zero-nv definition must be writable");
        check(package.write(QStringLiteral("image/system.img"),
                            QByteArray(SystemBytes, 'S')),
              "system image must be writable");
        check(package.write(QStringLiteral("image/vendor.img"),
                            QByteArray(VendorBytes, 'V')),
              "vendor image must be writable");
        check(package.write(xml, buildRawProgramXml({{QStringLiteral("super.img"),
                                                     QStringLiteral("super")}})),
              "rawprogram xml must be writable");

        QString definition;
        QString error;
        check(DynamicSuperPlanner::discover(xml, &definition, &error),
              "a lone zero-nv definition must still be discovered");
        check(QFileInfo(definition).fileName() == QStringLiteral("super_def.00000000.json"),
              "the zero-nv definition must be the fallback when nothing else exists");
    }

    if (failures != 0) {
        std::cerr << failures << " DynamicSuper assertion(s) failed\n";
        return 1;
    }
    std::cout << "DynamicSuper / META discovery tests passed\n";
    return 0;
}
