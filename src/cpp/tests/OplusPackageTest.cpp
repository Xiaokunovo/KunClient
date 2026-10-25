// OFP / OPS 解包回归：AES-128（ECB + CFB128 NIST 向量）、密钥派生、MTK/QC OFP 合成样本、
// OPS 三套 MBox、路径穿越拦截。合成样本由本测试自己按容器格式拼出来，再走正式解包路径。

#include "oplus/AesCfb.h"
#include "oplus/OfpExtractor.h"
#include "oplus/OpsExtractor.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>
#include <algorithm>
#include <iostream>

namespace {

using swt::edl::AesCfb;
using swt::edl::OfpExtractor;
using swt::edl::OfpExtractResult;
using swt::edl::OpsExtractor;
using swt::edl::OpsExtractResult;

int failures = 0;

void check(bool condition, const char* what)
{
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++failures;
}

QByteArray hex(const char* text)
{
    return QByteArray::fromHex(QByteArray(text));
}

template <typename T>
void writeLe(QByteArray& data, qsizetype offset, T value)
{
    const T little = qToLittleEndian(value);
    std::memcpy(data.data() + offset, &little, sizeof(T));
}

quint8 swapNibbles(quint8 value)
{
    return static_cast<quint8>(((value & 0x0F) << 4) | ((value & 0xF0) >> 4));
}

/// MTK 文件头/文件表的**编码**方向：in = swap(plain ^ key)（解码是 plain = key ^ swap(in)）。
QByteArray mtkEncode(const QByteArray& key, const QByteArray& plain)
{
    QByteArray out = plain;
    for (qsizetype i = 0; i < plain.size(); ++i) {
        out[i] = static_cast<char>(swapNibbles(static_cast<quint8>(plain[i])
                                               ^ static_cast<quint8>(key[i % key.size()])));
    }
    return out;
}

QByteArray writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(data);
    file.close();
    return data;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

// ── 合成 MTK OFP：[镜像数据][文件表 0x60][文件头 0x6C] ─────────────────────
QByteArray buildMtkOfp(const QByteArray& image, int encryptedLength, const QByteArray& key,
                       const QByteArray& iv, const char* fileName = "boot.img")
{
    const QByteArray encrypted =
        AesCfb::encrypt(image.left(encryptedLength), key, iv);
    QByteArray file = encrypted + image.mid(encryptedLength);

    QByteArray table(0x60, '\0');
    writeLe<quint64>(table, 32, 0);                                  // start
    writeLe<quint64>(table, 40, static_cast<quint64>(image.size()));  // length
    writeLe<quint64>(table, 48, static_cast<quint64>(encryptedLength));  // encrypted length
    std::memcpy(table.data() + 56, fileName, std::min<size_t>(std::strlen(fileName), 31));

    QByteArray header(0x6C, '\0');
    writeLe<quint16>(header, 72, 1);   // entryCount

    const QByteArray shuffleKey("geyixue");
    file += mtkEncode(shuffleKey, table);
    file += mtkEncode(shuffleKey, header);
    return file;
}

// ── 合成 QC OFP：[数据页...][xml 密文][元数据页] ───────────────────────────
QByteArray buildQcOfp(const QByteArray& saharaPlain, const QByteArray& firmwarePlain,
                      const QByteArray& key, const QByteArray& iv, const QByteArray& xml)
{
    constexpr int kPage = 0x1000;
    QByteArray file(kPage, '\0');                        // page 0：填充
    file += AesCfb::encrypt(saharaPlain, key, iv);       // page 1：Sahara（整段加密）
    file += firmwarePlain;                               // page 2：Firmware（明文）

    const QByteArray encryptedXml = AesCfb::encrypt(xml, key, iv);
    file += encryptedXml;                                // page 3：ProFile.xml 密文

    QByteArray metadata(kPage, '\0');                    // 末页：元数据
    writeLe<quint32>(metadata, 0x10, 0x7CEF);
    writeLe<quint32>(metadata, 0x14, 3);                             // xml 起始页号
    writeLe<quint32>(metadata, 0x18, static_cast<quint32>(encryptedXml.size()));
    file += metadata;
    return file;
}

// ── 合成 OPS：[数据区][settings.xml 密文+补齐][0x200 页脚] ─────────────────
QByteArray buildOps(const QByteArray& saharaPlain, const QByteArray& programPlain,
                    const QByteArray& xml, int mboxVersion)
{
    constexpr int kSector = OpsExtractor::SectorSize;
    const QByteArray mbox = OpsExtractor::mboxConstants(mboxVersion);
    if (mbox.isEmpty()) return {};

    QByteArray file(0x800, '\0');
    // Sahara 段用 OPS 自己的密钥流（不是 AES）。
    file.replace(0x200, saharaPlain.size(), OpsExtractor::encryptCustom(saharaPlain, mbox));
    file.replace(0x600, programPlain.size(), programPlain);

    const QByteArray encryptedXml = OpsExtractor::encryptCustom(xml, mbox);
    const qint64 padding = kSector - (xml.size() % kSector);
    file += encryptedXml;
    file += QByteArray(static_cast<qsizetype>(padding), '\0');

    QByteArray footer(kSector, '\0');
    writeLe<quint32>(footer, 0x18, static_cast<quint32>(xml.size()));
    file += footer;
    return file;
}

} // namespace

int main()
{
    // ── AES-128 单块（FIPS-197 附录 C.1）─────────────────────────────────
    {
        const QByteArray key = hex("000102030405060708090a0b0c0d0e0f");
        const QByteArray plain = hex("00112233445566778899aabbccddeeff");
        quint8 out[16];
        AesCfb::encryptBlock(reinterpret_cast<const quint8*>(plain.constData()), out,
                             reinterpret_cast<const quint8*>(key.constData()));
        check(QByteArray(reinterpret_cast<char*>(out), 16).toHex()
                  == QByteArray("69c4e0d86a7b0430d8cdb78070b4c55a"),
              "AES-128 FIPS-197 block vector");
    }

    // ── AES-128-CFB128（NIST SP 800-38A F.3.13 / F.3.14）────────────────
    {
        const QByteArray key = hex("2b7e151628aed2a6abf7158809cf4f3c");
        const QByteArray iv = hex("000102030405060708090a0b0c0d0e0f");
        const QByteArray plain = hex("6bc1bee22e409f96e93d7e117393172a"
                                     "ae2d8a571e03ac9c9eb76fac45af8e51"
                                     "30c81c46a35ce411e5fbc1191a0a52ef"
                                     "f69f2445df4f9b17ad2b417be66c3710");
        const QByteArray expected = hex("3b3fd92eb72dad20333449f8e83cfb4a"
                                        "c8a64537a0b3a93fcde3cdad9f1ce58b"
                                        "26751f67a3cbb140b1808cf187a4f4df"
                                        "c04b05357c5d1c0eeac4c66f9ff7f2e6");
        const QByteArray encrypted = AesCfb::encrypt(plain, key, iv);
        check(encrypted.toHex() == expected.toHex(), "AES-128-CFB128 NIST vector");
        check(AesCfb::decrypt(expected, key, iv) == plain, "AES-128-CFB128 decrypt NIST vector");

        // 任意长度（含不足一块的尾巴）往返
        bool roundTripOk = true;
        for (int length = 1; length <= 40; ++length) {
            const QByteArray sample = plain.left(length);
            const QByteArray cipher = AesCfb::encrypt(sample, key, iv);
            if (cipher.size() != sample.size()) roundTripOk = false;
            if (AesCfb::decrypt(cipher, key, iv) != sample) roundTripOk = false;
        }
        check(roundTripOk, "AES-CFB roundtrip for 1..40 bytes");

        // 参数不合法 → 空
        check(AesCfb::decrypt(plain, key.left(15), iv).isEmpty(), "reject short key");
        check(AesCfb::decrypt(plain, key, iv.left(8)).isEmpty(), "reject short iv");
    }

    // ── 密钥派生（mask + 混淆串 → MD5 十六进制前 16 字符）─────────────────
    {
        check(OfpExtractor::deriveKeyMaterial(hex("67657963787565E837D226B69A495D21"),
                                              hex("F6C50203515A2CE7D8C3E1F938B7E94C"))
                  == QByteArray("94d62e831cf1a1a0"),
              "derive MTK key");
        check(OfpExtractor::deriveKeyMaterial(hex("67657963787565E837D226B69A495D21"),
                                              hex("42F2D5399137E2B2813CD8ECDF2F4D72"))
                  == QByteArray("7ab5e33bd50d81ca"),
              "derive MTK iv");
        check(OfpExtractor::deriveKeyMaterial(hex("27827963787265EF89D126B69A495A21"),
                                              hex("82C50203285A2CE7D8C3E198383CE94C"))
                  == QByteArray("d154afeeaafa958f"),
              "derive QC key");
    }

    // ── MTK 文件头混淆解码（非自反）───────────────────────────────────────
    {
        QByteArray data(1, '\0');
        OfpExtractor::mtkShuffleDecode(QByteArray("geyixue"), &data);
        check(static_cast<unsigned char>(data.at(0)) == 0x67, "mtkShuffleDecode: 0x00 ^ 'g'");
        const QByteArray once = data;
        OfpExtractor::mtkShuffleDecode(QByteArray("geyixue"), &data);
        check(data != once, "mtkShuffleDecode is NOT self-inverse");
    }

    const QByteArray mtkKey = OfpExtractor::deriveKeyMaterial(hex("67657963787565E837D226B69A495D21"),
                                                              hex("F6C50203515A2CE7D8C3E1F938B7E94C"));
    const QByteArray mtkIv = OfpExtractor::deriveKeyMaterial(hex("67657963787565E837D226B69A495D21"),
                                                             hex("42F2D5399137E2B2813CD8ECDF2F4D72"));
    const QByteArray qcKey = OfpExtractor::deriveKeyMaterial(hex("27827963787265EF89D126B69A495A21"),
                                                             hex("82C50203285A2CE7D8C3E198383CE94C"));
    const QByteArray qcIv = OfpExtractor::deriveKeyMaterial(hex("27827963787265EF89D126B69A495A21"),
                                                            hex("422DD5399181E223813CD8ECDF2E4D72"));

    QTemporaryDir temp;
    check(temp.isValid(), "temp dir");

    // ── MTK OFP 解包 ─────────────────────────────────────────────────────
    {
        QByteArray image = QByteArray("MMM") + QByteArray(4093, '\x5A');
        const QByteArray ofp = buildMtkOfp(image, 1024, mtkKey, mtkIv);
        const QString source = QDir(temp.path()).filePath(QStringLiteral("mtk.ofp"));
        writeFile(source, ofp);

        const QString outDir = QDir(temp.path()).filePath(QStringLiteral("mtk-out"));
        OfpExtractResult result;
        QString error;
        const bool ok = OfpExtractor::extract(source, outDir, &result, &error);
        check(ok, "MTK OFP extract succeeds");
        if (!ok) std::cerr << "  error: " << error.toStdString() << '\n';
        check(result.format == OfpExtractResult::Format::Mtk, "MTK OFP format detected");
        check(result.fileCount == 1, "MTK OFP one file");
        check(readFile(QDir(outDir).filePath(QStringLiteral("boot.img"))) == image,
              "MTK OFP image bytes match");
    }

    // ── MTK OFP：路径穿越必须被拦 ────────────────────────────────────────
    {
        const QByteArray image = QByteArray("MMM") + QByteArray(4093, '\x11');
        const QByteArray ofp = buildMtkOfp(image, 1024, mtkKey, mtkIv, "../evil.bin");
        const QString source = QDir(temp.path()).filePath(QStringLiteral("evil.ofp"));
        writeFile(source, ofp);

        OfpExtractResult result;
        QString error;
        const bool ok = OfpExtractor::extract(source, QDir(temp.path()).filePath(
                                                         QStringLiteral("evil-out")),
                                              &result, &error);
        check(!ok, "path traversal is rejected");
        check(error.contains(QStringLiteral("不安全")), "path traversal error message");
    }

    // ── QC OFP 解包 ─────────────────────────────────────────────────────
    {
        const QByteArray saharaPlain(0x1000, '\x22');
        const QByteArray firmwarePlain(0x1000, '\x33');
        const QByteArray xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<ProFile>\n"
            "  <Sahara>\n"
            "    <File Path=\"prog_firehose.elf\" FileOffsetInSrc=\"1\" SizeInByteInSrc=\"4096\"/>\n"
            "  </Sahara>\n"
            "  <Firmware>\n"
            "    <File filename=\"firmware.bin\" FileOffsetInSrc=\"2\" SizeInByteInSrc=\"4096\"/>\n"
            "  </Firmware>\n"
            "</ProFile>\n";

        const QByteArray ofp = buildQcOfp(saharaPlain, firmwarePlain, qcKey, qcIv, xml);
        const QString source = QDir(temp.path()).filePath(QStringLiteral("qc.ofp"));
        writeFile(source, ofp);

        const QString outDir = QDir(temp.path()).filePath(QStringLiteral("qc-out"));
        OfpExtractResult result;
        QString error;
        const bool ok = OfpExtractor::extract(source, outDir, &result, &error);
        check(ok, "QC OFP extract succeeds");
        if (!ok) std::cerr << "  error: " << error.toStdString() << '\n';
        check(result.format == OfpExtractResult::Format::Qualcomm, "QC OFP format detected");
        check(result.fileCount == 2, "QC OFP two files");
        check(!result.profileXmlPath.isEmpty()
                  && QFileInfo::exists(result.profileXmlPath),
              "QC OFP writes ProFile.xml");
        check(readFile(QDir(outDir).filePath(QStringLiteral("prog_firehose.elf"))) == saharaPlain,
              "QC OFP Sahara segment decrypted");
        check(readFile(QDir(outDir).filePath(QStringLiteral("firmware.bin"))) == firmwarePlain,
              "QC OFP Firmware segment copied verbatim");
    }

    // ── OPS：三套 MBox 都能识别并解包 ───────────────────────────────────
    for (const int version : { 5, 6, 4 }) {
        const QByteArray saharaPlain(512, '\x44');
        const QByteArray programPlain(256, '\x55');
        const QByteArray xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<OPS>\n"
            "  <SAHARA>\n"
            "    <File Path=\"prog_firehose.elf\" FileOffsetInSrc=\"1\" SizeInByteInSrc=\"512\"/>\n"
            "  </SAHARA>\n"
            "  <Program>\n"
            "    <File Path=\"boot.img\" FileOffsetInSrc=\"3\" SizeInByteInSrc=\"256\"/>\n"
            "  </Program>\n"
            "</OPS>\n";

        const QByteArray ops = buildOps(saharaPlain, programPlain, xml, version);
        check(!ops.isEmpty(), "build synthetic OPS");

        const QString source =
            QDir(temp.path()).filePath(QStringLiteral("mbox%1.ops").arg(version));
        writeFile(source, ops);

        const QString outDir =
            QDir(temp.path()).filePath(QStringLiteral("ops-out%1").arg(version));
        OpsExtractResult result;
        QString error;
        const bool ok = OpsExtractor::extract(source, outDir, &result, &error);
        if (!ok) std::cerr << "  MBox " << version << " error: " << error.toStdString() << '\n';
        check(ok, "OPS extract succeeds");
        check(result.mboxVersion == version, "OPS MBox version detected");
        check(result.fileCount == 2, "OPS two files");
        check(readFile(QDir(outDir).filePath(QStringLiteral("prog_firehose.elf"))) == saharaPlain,
              "OPS Sahara segment decrypted");
        check(readFile(QDir(outDir).filePath(QStringLiteral("boot.img"))) == programPlain,
              "OPS Program segment copied verbatim");
        check(readFile(result.settingsPath).contains("FileOffsetInSrc"),
              "OPS settings.xml written in clear");
    }

    // ── 邮箱常量表首 16 字节（防手滑改坏）────────────────────────────────
    {
        check(OpsExtractor::mboxConstants(5).left(16).toHex()
                  == QByteArray("608a3f2d686bd423510cd095bb40e976"),
              "MBox 5 constants");
        check(OpsExtractor::mboxConstants(6).left(16).toHex()
                  == QByteArray("aa69829e5ddeb13d30bb81a34665a3e1"),
              "MBox 6 constants");
        check(OpsExtractor::mboxConstants(4).left(16).toHex()
                  == QByteArray("c45d057199ddbbee29a16dc7adbfa43f"),
              "MBox 4 constants");
        check(OpsExtractor::mboxConstants(7).isEmpty(), "unknown MBox version rejected");
    }

    if (failures != 0) {
        std::cerr << failures << " OFP/OPS check(s) failed\n";
        return 1;
    }
    std::cout << "OFP / OPS extractor tests passed\n";
    return 0;
}
