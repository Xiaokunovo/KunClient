// ScatterParser 单测 —— MTK scatter 双格式解析（脱机可验，不需要真机）。
// 口径：经典 .txt（含「字段全带 - 前缀」变体）+ v6 XML + 错误路径。
#include "kc/mtk/ScatterParser.h"

#include <QTemporaryDir>
#include <QTextStream>

#include <cstdio>
#include <iostream>

namespace {

int g_failures = 0;

void check(bool ok, const QString& what)
{
    std::printf("  [%s] %s\n", ok ? "OK  " : "FAIL", what.toUtf8().constData());
    if (!ok) ++g_failures;
}

void checkEq(const QString& actual, const QString& expected, const QString& what)
{
    const bool ok = actual == expected;
    if (!ok) {
        std::printf("       actual=%s expected=%s\n",
                    actual.toUtf8().constData(), expected.toUtf8().constData());
    }
    check(ok, what);
}

void writeFile(const QString& path, const QString& content)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << content;
    }
}

// ── 经典 scatter（字段缩进、无 - 前缀）───────────────────────────────────────
void testClassicScatter()
{
    std::printf("== classic scatter (.txt) ==\n");
    QTemporaryDir dir;
    writeFile(dir.filePath(QStringLiteral("preloader_mt6768.bin")), QStringLiteral("x"));

    const QString text = QStringLiteral(
        "############################################################################################################\n"
        "# General settings\n"
        "############################################################################################################\n"
        "- platform: MT6768\n"
        "- storage: EMMC\n"
        "- boot_channel: MSDC_0\n"
        "############################################################################################################\n"
        "# Partition settings\n"
        "############################################################################################################\n"
        "- partition_index: 0x0\n"
        "  partition_name: PRELOADER\n"
        "  file_name: preloader_mt6768.bin\n"
        "  is_download: true\n"
        "  type: SV5_1\n"
        "  linear_start_addr: 0x0\n"
        "  physical_start_addr: 0x0\n"
        "  partition_size: 0x80000\n"
        "  region: EMMC_BOOT_1\n"
        "  storage: HW_STORAGE_EMMC\n"
        "  operation_type: BOOTLOADERS\n"
        "  is_reserved: false\n"
        "- partition_index: 0x1\n"
        "  partition_name: boot\n"
        "  file_name: boot.img\n"
        "  is_download: true\n"
        "  physical_start_addr: 0x80000\n"
        "  partition_size: 0x2000000\n"
        "  region: EMMC_USER\n"
        "  storage: HW_STORAGE_EMMC\n"
        "  operation_type: UPDATE\n"
        "- partition_index: 0x2\n"
        "  partition_name: nvram\n"
        "  file_name: nvram.bin\n"
        "  is_download: false\n"
        "  physical_start_addr: 0x2800000\n"
        "  partition_size: 0x400000\n"
        "  region: EMMC_USER\n"
        "  storage: HW_STORAGE_EMMC\n"
        "  operation_type: BINREGION\n"
        "  is_reserved: true\n");

    const kc::mtk::ScatterInfo info = kc::mtk::ScatterParser::parseClassicText(text, dir.path());
    check(info.valid, "valid");
    checkEq(info.chipName, QStringLiteral("MT6768"), "chip name");
    check(info.storage == kc::mtk::StorageType::Emmc, "storage=EMMC");
    check(info.partitions.size() == 3, "partition count=3");

    const kc::mtk::ScatterPartition& preloader = info.partitions[0];
    checkEq(preloader.name, QStringLiteral("PRELOADER"), "preloader name");
    checkEq(preloader.fileName, QStringLiteral("preloader_mt6768.bin"), "preloader file");
    check(preloader.isDownload && preloader.enabled, "preloader is_download");
    check(preloader.startAddress == 0x0, "preloader start=0");
    check(preloader.size == 0x80000, "preloader size=0x80000");
    checkEq(preloader.region, QStringLiteral("EMMC_BOOT_1"), "preloader region");
    checkEq(preloader.operationType, QStringLiteral("BOOTLOADERS"), "preloader op_type");
    check(preloader.storage == kc::mtk::StorageType::Emmc, "preloader storage");
    check(preloader.fileExists, "preloader file exists (temp)");

    const kc::mtk::ScatterPartition& boot = info.partitions[1];
    check(boot.startAddress == 0x80000, "boot start=0x80000");
    check(boot.size == 0x2000000, "boot size=0x2000000");
    check(!boot.fileExists, "boot.img missing -> fileExists=false");

    const kc::mtk::ScatterPartition& nvram = info.partitions[2];
    check(!nvram.isDownload && !nvram.enabled, "nvram is_download=false -> 不勾选");
    check(nvram.isReserved, "nvram is_reserved");
    check(info.protectedParts.contains(QStringLiteral("nvram")), "nvram 进保护名单(BINREGION)");
}

// ── 变体：每个字段都带 "- " 前缀 ────────────────────────────────────────────
void testDashVariant()
{
    std::printf("== classic scatter (dash-prefixed variant) ==\n");
    const QString text = QStringLiteral(
        "- platform: MT6833\n"
        "- storage: UFS\n"
        "- partition_index: 0x0\n"
        "- partition_name: metadata\n"
        "- file_name: metadata.img\n"
        "- is_download: true\n"
        "- physical_start_addr: 0x100000\n"
        "- partition_size: 0x1000000\n"
        "- region: UFS_LU0\n"
        "- storage: HW_STORAGE_UFS\n"
        "- operation_type: UPDATE\n");

    const kc::mtk::ScatterInfo info = kc::mtk::ScatterParser::parseClassicText(text, {});
    check(info.valid, "valid");
    checkEq(info.chipName, QStringLiteral("MT6833"), "chip name");
    check(info.storage == kc::mtk::StorageType::Ufs, "storage=UFS");
    check(info.partitions.size() == 1, "partition count=1");
    if (info.partitions.size() == 1) {
        checkEq(info.partitions[0].name, QStringLiteral("metadata"), "name");
        check(info.partitions[0].startAddress == 0x100000, "start=0x100000");
        check(info.partitions[0].size == 0x1000000, "size");
    }
}

// ── v6 XML scatter（蓝本格式）───────────────────────────────────────────────
void testXmlScatter()
{
    std::printf("== v6 XML scatter ==\n");
    const QString xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<scatter>\n"
        "  <general>\n"
        "    <platform>MT6785</platform>\n"
        "    <skip_pt_operate>TRUE</skip_pt_operate>\n"
        "  </general>\n"
        "  <storage_type name=\"EMMC\">\n"
        "    <partition_index name=\"0x0\">\n"
        "      <is_download>true</is_download>\n"
        "      <partition_name>PRELOADER</partition_name>\n"
        "      <physical_start_addr>0x0</physical_start_addr>\n"
        "      <partition_size>0x80000</partition_size>\n"
        "      <region>EMMC_BOOT_1</region>\n"
        "      <file_name>preloader_mt6785.bin</file_name>\n"
        "      <operation_type>BOOTLOADERS</operation_type>\n"
        "    </partition_index>\n"
        "    <partition_index name=\"0x1\">\n"
        "      <is_download>false</is_download>\n"
        "      <partition_name>sec1</partition_name>\n"
        "      <physical_start_addr>0x80000</physical_start_addr>\n"
        "      <partition_size>0x10000</partition_size>\n"
        "      <file_name>sec1.img</file_name>\n"
        "      <operation_type>PROTECTED</operation_type>\n"
        "    </partition_index>\n"
        "  </storage_type>\n"
        "</scatter>\n");

    const kc::mtk::ScatterInfo info = kc::mtk::ScatterParser::parseXmlText(xml, {});
    check(info.valid, "valid");
    checkEq(info.chipName, QStringLiteral("MT6785"), "chip name");
    check(info.skipPtOperation, "skip_pt_operate=TRUE");
    check(info.storage == kc::mtk::StorageType::Emmc, "storage from name attr");
    check(info.partitions.size() == 2, "partition count=2");
    if (info.partitions.size() == 2) {
        checkEq(info.partitions[0].name, QStringLiteral("PRELOADER"), "name[0]");
        check(info.partitions[0].enabled, "partition[0] enabled");
        checkEq(info.partitions[1].name, QStringLiteral("sec1"), "name[1]");
        check(!info.partitions[1].enabled, "partition[1] not enabled");
        check(info.protectedParts.contains(QStringLiteral("sec1")), "sec1 进保护名单(PROTECTED)");
    }
}

// ── 错误路径 ────────────────────────────────────────────────────────────────
void testErrors()
{
    std::printf("== error paths ==\n");
    const kc::mtk::ScatterInfo empty = kc::mtk::ScatterParser::parseClassicText({}, {});
    check(!empty.valid && !empty.errorMessage.isEmpty(), "empty text -> invalid+message");

    const kc::mtk::ScatterInfo noPart =
        kc::mtk::ScatterParser::parseClassicText(QStringLiteral("- platform: MT6768\n"), {});
    check(!noPart.valid, "no partitions -> invalid");

    const kc::mtk::ScatterInfo badXml =
        kc::mtk::ScatterParser::parseXmlText(QStringLiteral("<scatter><general>"), {});
    check(!badXml.valid && badXml.errorMessage.contains(QStringLiteral("XML")),
          "broken xml -> invalid+XML message");

    const kc::mtk::ScatterInfo missing =
        kc::mtk::ScatterParser::parseFile(QStringLiteral("Z:/no/such/scatter.txt"));
    check(!missing.valid && !missing.errorMessage.isEmpty(), "missing file -> invalid+message");
}

// ── 无名收尾块（`- partition_index: SYS0` 不带 partition_name）────────────────
//  真实线刷包常见写法：不能因此把整包判死，只能跳过该块 + 留警告。
void testNamelessTrailer()
{
    std::printf("== nameless trailer block ==\n");
    const QString text = QStringLiteral(
        "- platform: MT6768\n"
        "- storage: EMMC\n"
        "- partition_index: 0x0\n"
        "  partition_name: boot\n"
        "  file_name: boot.img\n"
        "  is_download: true\n"
        "  physical_start_addr: 0x80000\n"
        "  partition_size: 0x2000000\n"
        "  region: EMMC_USER\n"
        "  storage: HW_STORAGE_EMMC\n"
        "  operation_type: UPDATE\n"
        "- partition_index: SYS0\n"
        "  file_name: NONE\n"
        "  is_download: false\n"
        "  region: EMMC_USER\n"
        "  storage: HW_STORAGE_EMMC\n"
        "  operation_type: INVISIBLE\n");

    const kc::mtk::ScatterInfo info = kc::mtk::ScatterParser::parseClassicText(text, {});
    check(info.valid, "nameless trailer -> still valid（不判死整包）");
    check(info.partitions.size() == 1, "只保留 1 个命名分区");
    if (info.partitions.size() == 1) {
        checkEq(info.partitions[0].name, QStringLiteral("boot"), "保留的是 boot");
    }
    check(info.warnings.size() == 1, "留 1 条警告");
}

// ── 数值解析 ────────────────────────────────────────────────────────────────
void testParseNumber()
{
    std::printf("== parseNumber ==\n");
    uint64_t v = 0;
    check(kc::mtk::ScatterParser::parseNumber(QStringLiteral("0x1F"), &v) && v == 0x1F, "0x1F");
    check(kc::mtk::ScatterParser::parseNumber(QStringLiteral("0X1f"), &v) && v == 0x1F, "0X1f");
    check(kc::mtk::ScatterParser::parseNumber(QStringLiteral("31"), &v) && v == 31, "decimal 31");
    check(kc::mtk::ScatterParser::parseNumber(QStringLiteral(" 0x20 "), &v) && v == 0x20, "padded");
    check(!kc::mtk::ScatterParser::parseNumber(QStringLiteral("zz"), &v), "invalid -> false");
    check(!kc::mtk::ScatterParser::parseNumber(QStringLiteral(""), &v), "empty -> false");
}

} // namespace

int main(int, char*[])
{
    testClassicScatter();
    testDashVariant();
    testXmlScatter();
    testErrors();
    testNamelessTrailer();
    testParseNumber();

    if (g_failures > 0) {
        std::printf("\n%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\nALL SCATTER PARSER TESTS PASSED\n");
    return 0;
}
