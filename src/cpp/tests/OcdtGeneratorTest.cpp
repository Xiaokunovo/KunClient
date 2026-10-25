// OCDT 生成/分析回归：TDCO 头、geyixue 配置块、两块平台的 OSIG 差异、三种变体。
// 参考实现 = YukiClient `Core/Violet/OcdtGenerator.cs`，这里按字节逐项核对。

#include "storage/OcdtGenerator.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <iostream>

namespace {

using swt::edl::OcdtGenerator;
using swt::edl::OcdtPlatform;
using swt::edl::OcdtVariant;

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

quint32 le32(const QByteArray& data, qsizetype offset)
{
    return qFromLittleEndian<quint32>(data.constData() + offset);
}

quint16 le16(const QByteArray& data, qsizetype offset)
{
    return qFromLittleEndian<quint16>(data.constData() + offset);
}

QByteArray configPlain(const QByteArray& image)
{
    return OcdtGenerator::geyixueDecrypt(image.mid(OcdtGenerator::ConfigOffset,
                                                  OcdtGenerator::ConfigSize));
}

} // namespace

int main()
{
    // ── geyixue 加解密可逆 ────────────────────────────────────────────────
    {
        QByteArray plain(52, '\0');
        for (int i = 0; i < plain.size(); ++i) plain[i] = static_cast<char>(i * 7 + 3);
        const QByteArray encrypted = OcdtGenerator::geyixueEncrypt(plain);
        check(OcdtGenerator::geyixueDecrypt(encrypted) == plain, "geyixue roundtrip");
        check(encrypted != plain, "geyixue actually transforms bytes");
        // 首字节手算：p=0x03, key='g'=0x67 → 0x64；ROL4 → 0x46
        check(static_cast<unsigned char>(encrypted[0]) == 0x46, "geyixue first byte = ROL4(0x03^'g')");
    }

    // ── 机型表 / 变体 / OSIG 判定 ─────────────────────────────────────────
    {
        int projectId = 0;
        check(OcdtGenerator::tryGetProjectId(QStringLiteral("PDEM10"), &projectId)
                  && projectId == 19065,
              "tryGetProjectId PDEM10");
        check(!OcdtGenerator::tryGetProjectId(QStringLiteral("pdem10"), &projectId),
              "model match is case-sensitive (same as reference)");
        check(!OcdtGenerator::tryGetProjectId(QString(), &projectId), "empty model rejected");

        check(OcdtGenerator::recommendedVariant(QStringLiteral("PGCM10")) == OcdtVariant::Fill02,
              "PGCM10 → fill02");
        check(OcdtGenerator::recommendedVariant(QStringLiteral("PJU110")) == OcdtVariant::GeyixueFill,
              "PJU110 → geyixue_fill");
        check(OcdtGenerator::recommendedVariant(QStringLiteral("PGZ110")) == OcdtVariant::MixedProjectId,
              "PGZ110 → mixed_projid");
        check(OcdtGenerator::recommendedVariant(QStringLiteral("LE2100")) == OcdtVariant::Standard,
              "unknown/standard model → standard");

        check(OcdtGenerator::needsOsigBackup(20001), "PDYM20 projId needs OSIG");
        check(!OcdtGenerator::needsOsigBackup(19065), "PDEM10 does not need OSIG");

        check(OcdtGenerator::defaultFileName(19065, OcdtPlatform::Mtk8Mb)
                  == QStringLiteral("ocdt_19065_8mb.img"),
              "default file name 8mb");
        check(OcdtGenerator::defaultFileName(19065, OcdtPlatform::Qualcomm128Kb)
                  == QStringLiteral("ocdt_19065_128kb.img"),
              "default file name 128kb");
    }

    // ── 8MB 标准镜像：TDCO 头 + 三处 projId + 无 OSIG ────────────────────
    {
        const QByteArray image =
            OcdtGenerator::buildImage(OcdtPlatform::Mtk8Mb, 19065, OcdtVariant::Standard,
                                      QStringLiteral("PDEM10"));
        check(image.size() == OcdtGenerator::FileSize8Mb, "8mb image length");
        check(image.left(4) == QByteArray("TDCO"), "8mb magic");
        check(le16(image, 4) == 1, "8mb version = 1");
        check(le16(image, 6) == 0, "8mb reserved = 0");
        check(le32(image, 8) == 0x10, "8mb config offset = 0x10");
        check(le32(image, 12) == 0x34, "8mb config length = 0x34");

        const QByteArray plain = configPlain(image);
        check(plain.size() == 52, "config size 52");
        const int p1 = static_cast<unsigned char>(plain[0]) | (static_cast<unsigned char>(plain[1]) << 8);
        const int p2 = static_cast<unsigned char>(plain[4]) | (static_cast<unsigned char>(plain[5]) << 8);
        const int p3 = static_cast<unsigned char>(plain[8]) | (static_cast<unsigned char>(plain[9]) << 8);
        check(p1 == 19065 && p2 == 19065 && p3 == 19065, "projId written three times");
        check(plain[2] == 0 && plain[6] == 0 && plain[10] == 0, "standard has no 0x02 mark");
        bool tailZero = true;
        for (int i = OcdtGenerator::ConfigTailOffset; i < plain.size(); ++i) {
            if (plain[i] != 0) tailZero = false;
        }
        check(tailZero, "standard tail is zero");

        // 非旧款联发科：整块 OSIG 留零（0x1000 处没有 "OSIG"）
        check(image.mid(OcdtGenerator::OsigOffset, 4) != QByteArray("OSIG"),
              "standard 8mb has no OSIG block");
    }

    // ── 8MB 旧款联发科：空 OSIG（Version=0 + 默认 Device ID）────────────
    {
        const QByteArray image =
            OcdtGenerator::buildImage(OcdtPlatform::Mtk8Mb, 20001, OcdtVariant::Standard,
                                      QStringLiteral("PDYM20"));
        check(image.mid(OcdtGenerator::OsigOffset, 4) == QByteArray("OSIG"),
              "legacy MTK gets OSIG magic");
        check(le32(image, OcdtGenerator::OsigOffset + 4) == 0, "empty OSIG version = 0");
        const QByteArray deviceId = image.mid(OcdtGenerator::OsigOffset + 0x10, 16);
        check(deviceId == QByteArray::fromHex("30000000000000000000000000000000"),
              "empty OSIG default device id");
        bool signatureZero = true;
        for (int i = 0; i < 0x100; ++i) {
            if (image[OcdtGenerator::OsigOffset + 0x100 + i] != 0) signatureZero = false;
        }
        check(signatureZero, "empty OSIG has no RSA signature");
        // 8MB 平台不写 0x2000 副本
        check(image.mid(OcdtGenerator::OsigBackupOffset, 4) != QByteArray("OSIG"),
              "8mb never writes backup OSIG");
    }

    // ── 8MB + 备份镜像：只搬 [0x1000,0x1200)；长度不足则整块留零 ─────────
    {
        QByteArray backup(OcdtGenerator::OsigBackupMinLength, '\0');
        backup[OcdtGenerator::OsigOffset] = 'O';
        backup[OcdtGenerator::OsigOffset + 1] = 'S';
        backup[OcdtGenerator::OsigOffset + 2] = 'I';
        backup[OcdtGenerator::OsigOffset + 3] = 'G';
        backup[OcdtGenerator::OsigOffset + 4] = 0x07;   // version = 7
        backup[OcdtGenerator::OsigOffset + 0x10] = 0x5A;

        const QByteArray withBackup =
            OcdtGenerator::buildImage(OcdtPlatform::Mtk8Mb, 20001, OcdtVariant::Standard,
                                      QStringLiteral("PDYM20"), backup);
        check(withBackup.mid(OcdtGenerator::OsigOffset, 4) == QByteArray("OSIG"),
              "backup OSIG magic copied");
        check(le32(withBackup, OcdtGenerator::OsigOffset + 4) == 7, "backup OSIG version copied");
        check(withBackup[OcdtGenerator::OsigOffset + 0x10] == 0x5A, "backup device id copied");

        // 长度不足 0x1200：按参考实现**不写**任何 OSIG（整块留零）
        const QByteArray shortBackup(0x800, '\x11');
        const QByteArray withShort =
            OcdtGenerator::buildImage(OcdtPlatform::Mtk8Mb, 20001, OcdtVariant::Standard,
                                      QStringLiteral("PDYM20"), shortBackup);
        check(withShort.mid(OcdtGenerator::OsigOffset, 4) != QByteArray("OSIG"),
              "short backup writes no OSIG at all");
    }

    // ── 128KB 高通：OSIG 写两份 + 内嵌 TDCO 副本 ─────────────────────────
    {
        const QByteArray image =
            OcdtGenerator::buildImage(OcdtPlatform::Qualcomm128Kb, 19065, OcdtVariant::Standard,
                                      QStringLiteral("PDEM10"));
        check(image.size() == OcdtGenerator::FileSize128Kb, "128kb image length");
        check(image.left(4) == QByteArray("TDCO"), "128kb magic");
        check(image.mid(OcdtGenerator::OsigOffset, 4) == QByteArray("OSIG"),
              "128kb OSIG at 0x1000");
        check(image.mid(OcdtGenerator::OsigBackupOffset, 4) == QByteArray("OSIG"),
              "128kb OSIG copy at 0x2000");
        check(image.mid(OcdtGenerator::OsigOffset + 0x50, 4) == QByteArray("TDCO"),
              "128kb embedded TDCO copy");
        check(le32(image, OcdtGenerator::OsigOffset + 0x54) == 0x0001, "embedded TDCO version");
        check(le32(image, OcdtGenerator::OsigOffset + 0x58) == 0x0010, "embedded TDCO config offset");
        check(le32(image, OcdtGenerator::OsigOffset + 0x5C) == 0x0034, "embedded TDCO config length");
    }

    // ── 变体：fill02 / geyixue_fill / mixed_projid ───────────────────────
    {
        const QByteArray fill02 =
            OcdtGenerator::buildImage(OcdtPlatform::Qualcomm128Kb, 4256, OcdtVariant::Fill02);
        const QByteArray fill02Plain = configPlain(fill02);
        check(fill02Plain[2] == 0x02 && fill02Plain[6] == 0x02 && fill02Plain[10] == 0x02,
              "fill02 marks 0x02/0x06/0x0A");

        const QByteArray keyFill =
            OcdtGenerator::buildImage(OcdtPlatform::Qualcomm128Kb, 23054, OcdtVariant::GeyixueFill);
        const QByteArray keyPlain = configPlain(keyFill);
        bool tailIsKey = true;
        for (int i = OcdtGenerator::ConfigTailOffset; i < keyPlain.size(); ++i) {
            if (keyPlain[i] != "geyixue"[i % 7]) tailIsKey = false;
        }
        check(tailIsKey, "geyixue_fill tail = key bytes");

        // PGZ110（projId 22801）的固定配置块：已是密文，原样落盘
        const QByteArray mixed =
            OcdtGenerator::buildImage(OcdtPlatform::Qualcomm128Kb, 22801,
                                      OcdtVariant::MixedProjectId);
        const QByteArray expected = QByteArray::fromHex(
            "67c3979696c256762fb296875756765697968757567656979687575676569796875756765697968"
            "7575676569796875756765697");
        check(expected.size() == 52, "special config hex decodes to 52 bytes");
        check(mixed.mid(OcdtGenerator::ConfigOffset, OcdtGenerator::ConfigSize) == expected,
              "mixed_projid uses fixed config from table");

        // mixed_projid 但表里没有该 projId → 回退标准公式
        const QByteArray fallback =
            OcdtGenerator::buildImage(OcdtPlatform::Qualcomm128Kb, 19065,
                                      OcdtVariant::MixedProjectId);
        const QByteArray fallbackPlain = configPlain(fallback);
        const int fallbackId =
            static_cast<unsigned char>(fallbackPlain[0])
            | (static_cast<unsigned char>(fallbackPlain[1]) << 8);
        check(fallbackId == 19065, "mixed_projid falls back to formula when model unknown");
    }

    // ── 写文件 + analyze 报告 ────────────────────────────────────────────
    {
        QTemporaryDir dir;
        check(dir.isValid(), "temp dir");
        const QString path = QDir(dir.path()).filePath(QStringLiteral("nested/ocdt.img"));
        QString error;
        check(OcdtGenerator::generate(OcdtPlatform::Qualcomm128Kb, 19065, OcdtVariant::Standard,
                                      path, QStringLiteral("PDEM10"), {}, &error),
              "generate creates parent directories");
        check(error.isEmpty(), "generate reports no error");
        check(QFile::exists(path), "generated file exists");

        const QString report = OcdtGenerator::analyze(path);
        check(report.contains(QStringLiteral("项目号非空，OCDT 有效。")), "analyze says valid");
        check(report.contains(QStringLiteral("[19065, 19065, 19065]")), "analyze reads projId");
        check(report.contains(QStringLiteral("PDEM10 (projId=19065)")), "analyze maps model");
        check(report.contains(QStringLiteral("128KB 高通")), "analyze detects 128kb platform");

        // 非 OCDT 文件
        const QString junk = QDir(dir.path()).filePath(QStringLiteral("junk.bin"));
        QFile junkFile(junk);
        check(junkFile.open(QIODevice::WriteOnly), "write junk");
        junkFile.write(QByteArray(200, '\x55'));
        junkFile.close();
        check(OcdtGenerator::analyze(junk).startsWith(QStringLiteral("[错误]")),
              "analyze rejects non-TDCO file");

        check(!OcdtGenerator::generate(OcdtPlatform::Mtk8Mb, 19065, OcdtVariant::Standard,
                                       QString(), {}, {}, &error),
              "empty output path rejected");
    }

    // ── 平台/变体字符串口径 ─────────────────────────────────────────────
    {
        OcdtPlatform platform = OcdtPlatform::Qualcomm128Kb;
        check(OcdtGenerator::parsePlatform(QStringLiteral("8MB"), &platform)
                  && platform == OcdtPlatform::Mtk8Mb,
              "parsePlatform case-insensitive");
        check(!OcdtGenerator::parsePlatform(QStringLiteral("4mb"), &platform),
              "parsePlatform rejects unknown");
        OcdtVariant variant = OcdtVariant::Fill02;
        check(OcdtGenerator::parseVariant(QString(), &variant) && variant == OcdtVariant::Standard,
              "empty variant = standard");
        check(OcdtGenerator::parseVariant(QStringLiteral("geyixue_fill"), &variant)
                  && variant == OcdtVariant::GeyixueFill,
              "parseVariant geyixue_fill");
        check(!OcdtGenerator::parseVariant(QStringLiteral("bogus"), &variant),
              "parseVariant rejects unknown");
    }

    if (failures != 0) {
        std::cerr << failures << " OCDT check(s) failed\n";
        return 1;
    }
    std::cout << "OCDT generator tests passed\n";
    return 0;
}
