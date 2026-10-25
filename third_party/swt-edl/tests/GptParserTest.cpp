#include "core/DeviceIdentity.h"
#include "core/PartitionProtection.h"
#include "storage/GptParser.h"
#include "storage/RawProgramParser.h"
#include "storage/RawProgramWriter.h"
#include "storage/SparseImage.h"
#include "storage/SystemVersionReader.h"
#include "oplus/OplusVipSession.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>
#include <iostream>

namespace {

template <typename T>
void writeLe(QByteArray& data, qsizetype offset, T value)
{
    const T little = qToLittleEndian(value);
    std::memcpy(data.data() + offset, &little, sizeof(little));
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 3 && QString::fromLatin1(argv[1]) == QStringLiteral("--detect-chip")) {
        const QString programmer = QString::fromLocal8Bit(argv[2]);
        const QString chip = swt::edl::detectProgrammerChip(programmer);
        std::cout << chip.toStdString() << '\n';
        return chip.isEmpty() ? 101 : 0;
    }
    if (argc > 1) {
        QStringList requestedXml;
        for (int index = 1; index < argc; ++index) {
            requestedXml.push_back(QString::fromLocal8Bit(argv[index]));
        }
        swt::edl::RawProgramPlan externalPlan;
        QString externalError;
        if (!swt::edl::RawProgramParser::parse(requestedXml, &externalPlan, &externalError)) {
            std::cerr << externalError.toStdString() << '\n';
            return 100;
        }
        std::cout << "xml=" << externalPlan.xmlPaths.size()
                  << " entries=" << externalPlan.entries.size()
                  << " super=" << externalPlan.superEntryCount
                  << " dynamic_super=" << externalPlan.dynamicSuperCount
                  << " bytes=" << externalPlan.totalSourceBytes << '\n';
        for (const QString& warning : externalPlan.warnings) {
            std::cout << warning.toStdString() << '\n';
        }
        return 0;
    }
    constexpr quint32 sectorSize = 512;
    QByteArray fixture(34 * sectorSize, '\0');
    constexpr qsizetype header = sectorSize;
    constexpr qsizetype entry = sectorSize * 2;

    std::memcpy(fixture.data() + header, "EFI PART", 8);
    writeLe<quint32>(fixture, header + 12, 92);
    writeLe<quint64>(fixture, header + 72, 2);
    writeLe<quint32>(fixture, header + 80, 128);
    writeLe<quint32>(fixture, header + 84, 128);

    fixture[entry] = 0x28;
    fixture[entry + 16] = 0x44;
    writeLe<quint64>(fixture, entry + 32, 2048);
    writeLe<quint64>(fixture, entry + 40, 4095);
    const QString name = QStringLiteral("boot");
    for (qsizetype index = 0; index < name.size(); ++index) {
        writeLe<quint16>(fixture, entry + 56 + index * 2, name.at(index).unicode());
    }

    QVector<swt::edl::PartitionInfo> partitions;
    QString error;
    if (!swt::edl::GptParser::parse(fixture, sectorSize, 0, &partitions, &error)) {
        std::cerr << "GPT parse failed: " << error.toStdString() << '\n';
        return 1;
    }
    if (partitions.size() != 1 || partitions.front().name != QStringLiteral("boot") ||
        partitions.front().firstLba != 2048 || partitions.front().sectorCount() != 2048) {
        std::cerr << "Unexpected GPT partition result\n";
        return 2;
    }

    QByteArray invalid(1024, '\0');
    partitions.clear();
    if (swt::edl::GptParser::parse(invalid, sectorSize, 0, &partitions, &error)) {
        std::cerr << "Invalid GPT fixture was accepted\n";
        return 3;
    }

    if (swt::edl::qualcommChipFromMsmId(QStringLiteral("000c30e1")) !=
            QStringLiteral("SM8250") ||
        swt::edl::qualcommChipFromMsmId(QStringLiteral("001350e1")) !=
            QStringLiteral("SM8350") ||
        swt::edl::qualcommChipFromMsmId(QStringLiteral("0028c0e1")) !=
            QStringLiteral("SM8550")) {
        std::cerr << "Qualcomm MSM_ID mapping regression\n";
        return 40;
    }
    QTemporaryDir identityFixture;
    if (!identityFixture.isValid()) return 41;
    const QString identityProgrammer =
        QDir(identityFixture.path()).filePath(QStringLiteral("DevprgProgrammer.melf"));
    QFile identityFile(identityProgrammer);
    const QByteArray identityBytes = QByteArrayLiteral(
        "\0UNKNOWN\0_IMAGE_VERSION_STRING=BOOT.XF.3.2-00304-SM8250-2\0");
    if (!identityFile.open(QIODevice::WriteOnly) ||
        identityFile.write(identityBytes) != identityBytes.size()) {
        return 42;
    }
    identityFile.close();
    if (swt::edl::detectProgrammerChip(identityProgrammer) != QStringLiteral("SM8250") ||
        swt::edl::detectProgrammerChip(identityProgrammer,
                                       {QStringLiteral("D:/loaders/SM8550_8gen2")}) !=
            QStringLiteral("SM8550")) {
        std::cerr << "Programmer chipset metadata detection regression\n";
        return 43;
    }

    QTemporaryDir vipFixture;
    if (!vipFixture.isValid()) return 4;
    QDir().mkpath(QDir(vipFixture.path()).filePath(QStringLiteral("nested")));
    const QStringList vipFiles{
        QStringLiteral("nested/DevprgProgrammer2.melf"),
        QStringLiteral("nested/DevprgProgrammer1.melf"),
        QStringLiteral("nested/DevprgProgrammer3.melf"),
        QStringLiteral("Digest_DDR.bin"),
        QStringLiteral("Digest2_DevprgProgrammer4.elf"),
        QStringLiteral("Sign2_DevprgProgrammer4.bin"),
        QStringLiteral("Sign_DDR.bin")
    };
    for (const QString& relative : vipFiles) {
        QFile file(QDir(vipFixture.path()).filePath(relative));
        const QByteArray contents = relative.contains(QStringLiteral("Programmer3"))
            ? QByteArrayLiteral("different-size") : QByteArrayLiteral("fixture");
        if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size()) return 5;
    }
    swt::edl::OplusVipPackage vipPackage;
    if (!swt::edl::OplusVipSession::discoverPackage(
            vipFixture.path(), swt::edl::StorageType::Ufs, &vipPackage, &error)) {
        std::cerr << "VIP package discovery failed: " << error.toStdString() << '\n';
        return 6;
    }
    if (!vipPackage.programmerPath.endsWith(QStringLiteral("DevprgProgrammer1.melf")) ||
        !vipPackage.digestPath.endsWith(QStringLiteral("Digest_DDR.bin")) ||
        !vipPackage.signaturePath.endsWith(QStringLiteral("Sign_DDR.bin")) ||
        vipPackage.programmerCandidates.size() != 2 ||
        !vipPackage.programmerCandidates.at(0).endsWith(QStringLiteral("DevprgProgrammer1.melf")) ||
        !vipPackage.programmerCandidates.at(1).endsWith(QStringLiteral("DevprgProgrammer2.melf"))) {
        std::cerr << "Unexpected VIP package discovery result: "
                  << vipPackage.programmerPath.toStdString() << " / "
                  << vipPackage.digestPath.toStdString() << " / "
                  << vipPackage.signaturePath.toStdString() << " candidates="
                  << vipPackage.programmerCandidates.size() << '\n';
        return 7;
    }

    // Explicit programmer selection is a bounded sibling-only lookup. This is
    // the UI path used when a file is double-clicked from a very large folder.
    const QString directDir = QDir(vipFixture.path()).filePath(QStringLiteral("direct"));
    QDir().mkpath(directDir);
    for (const QString& relative : {QStringLiteral("DevprgProgrammer1.melf"),
                                    QStringLiteral("DevprgProgrammer2.melf"),
                                    QStringLiteral("Digest.elf"),
                                    QStringLiteral("Sign.bin")}) {
        QFile file(QDir(directDir).filePath(relative));
        if (!file.open(QIODevice::WriteOnly) || file.write("direct") != 6) return 70;
    }
    swt::edl::OplusVipPackage selectedVipPackage;
    const QString selectedProgrammer =
        QDir(directDir).filePath(QStringLiteral("DevprgProgrammer2.melf"));
    if (!swt::edl::OplusVipSession::discoverPackage(
            directDir, swt::edl::StorageType::Ufs, &selectedVipPackage, &error,
            false, selectedProgrammer)) {
        std::cerr << "Bounded VIP package discovery failed: " << error.toStdString() << '\n';
        return 71;
    }
    if (selectedVipPackage.programmerPath.compare(selectedProgrammer, Qt::CaseInsensitive) != 0 ||
        selectedVipPackage.programmerCandidates.size() != 2 ||
        selectedVipPackage.programmerCandidates.front().compare(
            selectedProgrammer, Qt::CaseInsensitive) != 0) {
        std::cerr << "Explicit VIP programmer preference was not preserved\n";
        return 72;
    }

    QTemporaryDir xmlFixture;
    if (!xmlFixture.isValid()) return 8;
    for (const QString& filename : {QStringLiteral("super.0.img"),
                                    QStringLiteral("super.1.img")}) {
        QFile image(QDir(xmlFixture.path()).filePath(filename));
        if (!image.open(QIODevice::WriteOnly) || image.write(QByteArray(4096, '\x5a')) != 4096) {
            return 9;
        }
    }
    const QString xmlPath = QDir(xmlFixture.path()).filePath(QStringLiteral("rawprogram0.xml"));
    QFile xml(xmlPath);
    if (!xml.open(QIODevice::WriteOnly | QIODevice::Text)) return 10;
    const QByteArray xmlText =
        "<?xml version=\"1.0\"?><data>"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" file_sector_offset=\"0\" "
        "filename=\"super.0.img\" label=\"super\" num_partition_sectors=\"1\" "
        "physical_partition_number=\"0\" sparse=\"false\" start_sector=\"100\"/>"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" file_sector_offset=\"0\" "
        "filename=\"super.1.img\" label=\"super\" num_partition_sectors=\"1\" "
        "physical_partition_number=\"0\" sparse=\"false\" start_sector=\"101\"/>"
        "</data>";
    if (xml.write(xmlText) != xmlText.size()) return 11;
    xml.close();

    swt::edl::RawProgramPlan plan;
    if (!swt::edl::RawProgramParser::parse({xmlPath}, &plan, &error)) {
        std::cerr << "Rawprogram parse failed: " << error.toStdString() << '\n';
        return 12;
    }
    if (plan.entries.size() != 2 || !plan.hasSplitSuper() ||
        plan.entries.at(0).startSector != 100 || plan.entries.at(1).startSector != 101 ||
        plan.totalSourceBytes != 8192) {
        std::cerr << "Unexpected rawprogram plan\n";
        return 13;
    }

    QByteArray sparseBytes(28 + (12 + 4096) + 16 + 12, '\0');
    writeLe<quint32>(sparseBytes, 0, swt::edl::SparseImage::Magic);
    writeLe<quint16>(sparseBytes, 4, 1);
    writeLe<quint16>(sparseBytes, 6, 0);
    writeLe<quint16>(sparseBytes, 8, 28);
    writeLe<quint16>(sparseBytes, 10, 12);
    writeLe<quint32>(sparseBytes, 12, 4096);
    writeLe<quint32>(sparseBytes, 16, 3);
    writeLe<quint32>(sparseBytes, 20, 3);
    qsizetype cursor = 28;
    writeLe<quint16>(sparseBytes, cursor, 0xCAC1);
    writeLe<quint32>(sparseBytes, cursor + 4, 1);
    writeLe<quint32>(sparseBytes, cursor + 8, 12 + 4096);
    std::memset(sparseBytes.data() + cursor + 12, 0x33, 4096);
    cursor += 12 + 4096;
    writeLe<quint16>(sparseBytes, cursor, 0xCAC2);
    writeLe<quint32>(sparseBytes, cursor + 4, 1);
    writeLe<quint32>(sparseBytes, cursor + 8, 16);
    writeLe<quint32>(sparseBytes, cursor + 12, 0xAABBCCDD);
    cursor += 16;
    writeLe<quint16>(sparseBytes, cursor, 0xCAC3);
    writeLe<quint32>(sparseBytes, cursor + 4, 1);
    writeLe<quint32>(sparseBytes, cursor + 8, 12);

    const QString sparsePath = QDir(xmlFixture.path()).filePath(QStringLiteral("super.sparse.img"));
    QFile sparseFile(sparsePath);
    if (!sparseFile.open(QIODevice::WriteOnly) || sparseFile.write(sparseBytes) != sparseBytes.size()) {
        return 14;
    }
    sparseFile.close();
    if (!sparseFile.open(QIODevice::ReadOnly)) return 15;
    swt::edl::SparseImageInfo sparseInfo;
    if (!swt::edl::SparseImage::inspect(sparseFile, &sparseInfo, &error)) {
        std::cerr << "Sparse inspect failed: " << error.toStdString() << '\n';
        return 16;
    }
    if (sparseInfo.blockSize != 4096 || sparseInfo.totalBlocks != 3 ||
        sparseInfo.extents.size() != 3 || sparseInfo.expandedSize() != 12288 ||
        sparseInfo.extents.at(0).type != swt::edl::SparseExtentType::Raw ||
        sparseInfo.extents.at(1).type != swt::edl::SparseExtentType::Fill ||
        sparseInfo.extents.at(1).fillValue != 0xAABBCCDD ||
        sparseInfo.extents.at(2).type != swt::edl::SparseExtentType::Skip) {
        std::cerr << "Unexpected sparse image plan\n";
        return 17;
    }

    const QString sparseXmlPath = QDir(xmlFixture.path()).filePath(
        QStringLiteral("rawprogram_sparse.xml"));
    QFile sparseXml(sparseXmlPath);
    if (!sparseXml.open(QIODevice::WriteOnly | QIODevice::Text)) return 18;
    const QByteArray sparseXmlText =
        "<?xml version=\"1.0\"?><data>"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" file_sector_offset=\"0\" "
        "filename=\"super.sparse.img\" label=\"userdata\" num_partition_sectors=\"0\" "
        "physical_partition_number=\"0\" sparse=\"true\" start_sector=\"200\"/>"
        "</data>";
    if (sparseXml.write(sparseXmlText) != sparseXmlText.size()) return 19;
    sparseXml.close();

    swt::edl::RawProgramPlan sparsePlan;
    if (!swt::edl::RawProgramParser::parse({sparseXmlPath}, &sparsePlan, &error)) {
        std::cerr << "Zero-sector sparse rawprogram parse failed: "
                  << error.toStdString() << '\n';
        return 20;
    }
    if (sparsePlan.entries.size() != 1 || !sparsePlan.entries.front().sparse ||
        sparsePlan.entries.front().sectorCount != 3) {
        std::cerr << "Zero-sector sparse entry was not expanded from its image header\n";
        return 21;
    }

    QByteArray ocdtBytes(68, '\0');
    std::memcpy(ocdtBytes.data(), "TDCO", 4);
    const QString ocdtPath = QDir(xmlFixture.path()).filePath(
        QStringLiteral("oplus22851.bin"));
    QFile ocdtFile(ocdtPath);
    if (!ocdtFile.open(QIODevice::WriteOnly) ||
        ocdtFile.write(ocdtBytes) != ocdtBytes.size()) return 22;
    ocdtFile.close();
    QFile decoratedNv(QDir(xmlFixture.path()).filePath(
        QStringLiteral("dynamic_nvbk.22851.bin")));
    if (!decoratedNv.open(QIODevice::WriteOnly) ||
        decoratedNv.write("decorated") != 9) return 23;
    decoratedNv.close();
    QFile exactImage(QDir(xmlFixture.path()).filePath(QStringLiteral("guard.img")));
    if (!exactImage.open(QIODevice::WriteOnly) ||
        exactImage.write(QByteArray(4096, '\x6a')) != 4096) return 24;
    exactImage.close();

    const QString ocdtXmlPath = QDir(xmlFixture.path()).filePath(
        QStringLiteral("rawprogram_ocdt.xml"));
    QFile ocdtXml(ocdtXmlPath);
    if (!ocdtXml.open(QIODevice::WriteOnly | QIODevice::Text)) return 25;
    const QByteArray ocdtXmlText =
        "<?xml version=\"1.0\"?><data>"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" file_sector_offset=\"0\" "
        "filename=\"guard.img\" label=\"guard\" num_partition_sectors=\"1\" "
        "physical_partition_number=\"3\" sparse=\"false\" start_sector=\"700\"/>"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" file_sector_offset=\"0\" "
        "filename=\"oplus\" label=\"ocdt\" num_partition_sectors=\"32\" "
        "physical_partition_number=\"3\" sparse=\"false\" start_sector=\"576\"/>"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" file_sector_offset=\"0\" "
        "filename=\"dynamic_nvbk.bin\" label=\"oplusdycnvbk\" num_partition_sectors=\"32\" "
        "physical_partition_number=\"5\" sparse=\"false\" start_sector=\"32\"/>"
        "</data>";
    if (ocdtXml.write(ocdtXmlText) != ocdtXmlText.size()) return 26;
    ocdtXml.close();

    swt::edl::RawProgramPlan ocdtPlan;
    if (!swt::edl::RawProgramParser::parse({ocdtXmlPath}, &ocdtPlan, &error)) {
        std::cerr << "Strict XML parse failed: " << error.toStdString() << '\n';
        return 27;
    }
    if (ocdtPlan.entries.size() != 1 ||
        ocdtPlan.entries.front().label != QStringLiteral("guard") ||
        ocdtPlan.totalSourceBytes != 4096 ||
        !ocdtPlan.warnings.join(QLatin1Char('\n')).contains(
            QStringLiteral("跳过缺失镜像：ocdt → oplus")) ||
        !ocdtPlan.warnings.join(QLatin1Char('\n')).contains(
            QStringLiteral("跳过缺失镜像：oplusdycnvbk → dynamic_nvbk.bin"))) {
        std::cerr << "Strict XML resolver accepted a decorated lookalike\n";
        return 28;
    }

    const QString overflowXmlPath = QDir(xmlFixture.path()).filePath(
        QStringLiteral("rawprogram_overflow.xml"));
    QFile overflowXml(overflowXmlPath);
    if (!overflowXml.open(QIODevice::WriteOnly | QIODevice::Text)) return 29;
    const QByteArray overflowXmlText =
        "<?xml version=\"1.0\"?><data>"
        "<program SECTOR_SIZE_IN_BYTES=\"4096\" "
        "file_sector_offset=\"4503599627370496\" filename=\"guard.img\" "
        "label=\"guard\" num_partition_sectors=\"1\" "
        "physical_partition_number=\"0\" sparse=\"false\" start_sector=\"1\"/>"
        "</data>";
    if (overflowXml.write(overflowXmlText) != overflowXmlText.size()) return 30;
    overflowXml.close();
    swt::edl::RawProgramPlan overflowPlan;
    if (swt::edl::RawProgramParser::parse({overflowXmlPath}, &overflowPlan, &error) ||
        !error.contains(QStringLiteral("溢出"))) {
        std::cerr << "Overflowing rawprogram file offset was accepted\n";
        return 31;
    }

    QTemporaryDir backupFixture;
    if (!backupFixture.isValid()) return 32;
    const QStringList backupNames{QStringLiteral("boot.img"), QStringLiteral("persist.img")};
    for (const QString& imageName : backupNames) {
        QFile image(QDir(backupFixture.path()).filePath(imageName));
        if (!image.open(QIODevice::WriteOnly) || image.write(QByteArray(4096, '\x55')) != 4096) {
            return 33;
        }
    }
    QVector<swt::edl::PartitionInfo> backupPartitions(2);
    backupPartitions[0].name = QStringLiteral("boot");
    backupPartitions[0].lun = 0;
    backupPartitions[0].firstLba = 64;
    backupPartitions[0].lastLba = 65;
    backupPartitions[1].name = QStringLiteral("persist");
    backupPartitions[1].lun = 3;
    backupPartitions[1].firstLba = 2048;
    backupPartitions[1].lastLba = 2050;
    QStringList generatedXml;
    if (!swt::edl::RawProgramWriter::writeBackup(
            backupFixture.path(), backupPartitions, backupNames, 4096,
            &generatedXml, &error) || generatedXml.size() != 2) {
        std::cerr << "Backup rawprogram generation failed: " << error.toStdString() << '\n';
        return 34;
    }
    swt::edl::RawProgramPlan backupPlan;
    if (!swt::edl::RawProgramParser::parse(generatedXml, &backupPlan, &error) ||
        backupPlan.entries.size() != 2 || backupPlan.entries.at(0).label != QStringLiteral("boot") ||
        backupPlan.entries.at(0).startSector != 64 || backupPlan.entries.at(0).sectorCount != 2 ||
        backupPlan.entries.at(1).label != QStringLiteral("persist") ||
        backupPlan.entries.at(1).lun != 3 || backupPlan.entries.at(1).startSector != 2048 ||
        backupPlan.entries.at(1).sectorCount != 3) {
        std::cerr << "Generated backup rawprogram did not round-trip\n";
        return 35;
    }

    const QStringList protectedFixture{
        QStringLiteral("persist_a"), QStringLiteral("persistbak"),
        QStringLiteral("ocdt"), QStringLiteral("modemst1_b"),
        QStringLiteral("modemst2"), QStringLiteral("fsg"), QStringLiteral("fsc"),
        QStringLiteral("oplusdycnvbk"), QStringLiteral("oplusstanvbk.img"),
        QStringLiteral("oplusreserve4"), QStringLiteral("oplusreserve5"),
        QStringLiteral("param"), QStringLiteral("preisp_otp"),
        QStringLiteral("preisp_dt"), QStringLiteral("preisp_dt_bk")};
    for (const QString& protectedName : protectedFixture) {
        if (!swt::edl::isProtectedPartitionName(protectedName)) {
            std::cerr << "Protected partition policy missed " << protectedName.toStdString() << '\n';
            return 36;
        }
    }
    if (swt::edl::isProtectedPartitionName(QStringLiteral("boot_a")) ||
        swt::edl::isProtectedPartitionName(QStringLiteral("userdata"))) {
        std::cerr << "Protected partition policy included a regular flash partition\n";
        return 37;
    }

    QByteArray propertyFixture(1024 * 1024, '\0');
    const QByteArray propertyText =
        "ro.product.brand=OPLUS\n"
        "ro.vendor.oplus.market.name=Find X8\n"
        "ro.build.version.release=16\n"
        "ro.build.display.id=CPH9999_16.0.1\n"
        "ro.build.version.security_patch=2026-08-05\n";
    std::memcpy(propertyFixture.data(), propertyText.constData(), propertyText.size());
    swt::edl::SystemVersionInfo version;
    const swt::edl::PartitionByteReader propertyReader =
        [&propertyFixture](quint64 offset, quint32 length, QString*) {
            if (offset + length > static_cast<quint64>(propertyFixture.size())) return QByteArray{};
            return propertyFixture.mid(static_cast<qsizetype>(offset), length);
        };
    if (!swt::edl::SystemVersionReader::readPhysical(
            propertyReader, propertyFixture.size(), &version, &error) ||
        version.brand != QStringLiteral("OPLUS") ||
        version.marketName != QStringLiteral("Find X8") ||
        version.androidVersion != QStringLiteral("16") ||
        version.systemVersion != QStringLiteral("CPH9999_16.0.1") ||
        version.securityPatch != QStringLiteral("2026-08-05")) {
        std::cerr << "System version property extraction failed: " << error.toStdString() << '\n';
        return 38;
    }

    std::cout << "GPT, VIP, rawprogram, protection, version and sparse image tests passed\n";
    return 0;
}
