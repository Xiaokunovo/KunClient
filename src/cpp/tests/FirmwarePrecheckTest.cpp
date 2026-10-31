#include "kc/security/Digest.h"
#include "kc/tools/FirmwareChecksum.h"
#include "kc/tools/FirmwarePrecheck.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

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

/// 写一个文件（父目录已存在）。失败直接算一条失败 —— 后面所有断言都依赖它。
bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        std::cerr << "FAIL: cannot write " << path.toStdString() << '\n';
        ++failures;
        return false;
    }
    const bool ok = file.write(data) == data.size();
    file.close();
    return ok;
}

/// 往目录里放一个非空镜像。
bool writeImage(const QString& dir, const QString& name, int bytes)
{
    QByteArray payload(bytes, Qt::Uninitialized);
    for (int i = 0; i < bytes; ++i) payload[i] = static_cast<char>(i % 251);
    return writeFile(QDir(dir).filePath(name), payload);
}

/// 在 path 旁边写一份 coreutils 风格的校验文件。
bool writeSidecar(const QString& targetPath, const QString& hash, bool sha256)
{
    const QString line =
        kc::tools::formatChecksumLine(hash, QFileInfo(targetPath).fileName());
    return writeFile(targetPath + (sha256 ? QStringLiteral(".sha256") : QStringLiteral(".md5")),
                     (line + QStringLiteral("\n")).toUtf8());
}

QString sha256Of(const QString& path)
{
    QString error;
    const QString digest = kc::crypto::hashFile(path, kc::crypto::HashAlgo::Sha256, &error);
    return error.isEmpty() ? digest : QString();
}

QString md5Of(const QString& path)
{
    QString error;
    const QString digest = kc::crypto::hashFile(path, kc::crypto::HashAlgo::Md5, &error);
    return error.isEmpty() ? digest : QString();
}

/// 在 result 的明细里找一条包含 needle 的记录。
bool hasDetail(const kc::tools::PrecheckResult& result, const QString& needle)
{
    for (const kc::tools::PrecheckDetail& detail : result.details) {
        if (detail.message.contains(needle, Qt::CaseInsensitive)) return true;
    }
    return false;
}

} // namespace

// 固件包完整性预检的纯逻辑回归。覆盖任务要求里的六种情形：
// 空文件 / 截断 / 缺校验文件 / 校验通过 / 校验不匹配 / XML 声明 size 对不上。
int main()
{
    using kc::tools::precheckFirmware;
    using kc::tools::PrecheckLevel;
    using kc::tools::PrecheckOptions;
    using kc::tools::PrecheckResult;

    // ── 路径本身不合法：不抛、不崩，直接给 Error ─────────────────────────
    {
        const PrecheckResult empty = precheckFirmware(QString());
        check(!empty.ok && empty.blocksFlashing, "empty path is an error and blocks flashing");
        check(empty.level == PrecheckLevel::Error, "empty path level is Error");
        check(!empty.summary.isEmpty(), "empty path still carries a summary line");

        const PrecheckResult missing =
            precheckFirmware(QDir::tempPath() + QStringLiteral("/kc-definitely-not-here"));
        check(!missing.ok && missing.blocksFlashing, "missing path blocks flashing");
        check(hasDetail(missing, QStringLiteral("路径不存在")), "missing path explained");
    }

    QTemporaryDir temp;
    check(temp.isValid(), "temporary directory created");
    const QString root = temp.path();

    // ── 1) 空文件 → Error ───────────────────────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("empty"));
        check(QDir().mkpath(dir), "empty-image fixture dir created");
        writeImage(dir, QStringLiteral("boot.img"), 0);
        writeImage(dir, QStringLiteral("system.img"), 512);

        const PrecheckResult result = precheckFirmware(dir);
        check(!result.ok, "0-byte image is not ok");
        check(result.blocksFlashing, "0-byte image blocks flashing");
        check(hasDetail(result, QStringLiteral("0 字节空文件")), "0-byte image is named in details");
        check(result.imageCount == 2, "image count includes the empty image");
    }

    // ── 2) 缺校验文件 → Warning，不拦刷机 ───────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("nosum"));
        check(QDir().mkpath(dir), "no-checksum fixture dir created");
        writeImage(dir, QStringLiteral("boot.img"), 256);
        writeImage(dir, QStringLiteral("init_boot.img"), 128);

        const PrecheckResult result = precheckFirmware(dir);
        check(result.ok, "missing checksum file does not block flashing");
        check(!result.blocksFlashing, "missing checksum file is not a block");
        check(result.level == PrecheckLevel::Warning, "missing checksum file downgrades to Warning");
        check(hasDetail(result, QStringLiteral("未找到")), "missing checksum file is reported");
        check(!result.checksumMismatch, "missing checksum file is not a mismatch");
        check(result.summary.contains(QStringLiteral("提醒")), "summary marks the warning");
    }

    // ── 3) 校验通过 → Info ──────────────────────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("sumok"));
        check(QDir().mkpath(dir), "checksum-ok fixture dir created");
        const QString boot = QDir(dir).filePath(QStringLiteral("boot.img"));
        writeImage(dir, QStringLiteral("boot.img"), 300);
        writeImage(dir, QStringLiteral("init_boot.img"), 200);

        // 批量清单：一条正确记录（boot）+ 一条正确记录（init_boot）。
        const QString manifest = QDir(dir).filePath(QStringLiteral("SHA256SUMS"));
        const QString text = kc::tools::formatChecksumLine(sha256Of(boot),
                                                          QStringLiteral("boot.img"))
                             + QStringLiteral("\n")
                             + kc::tools::formatChecksumLine(
                                   sha256Of(QDir(dir).filePath(QStringLiteral("init_boot.img"))),
                                   QStringLiteral("init_boot.img"))
                             + QStringLiteral("\n");
        check(writeFile(manifest, text.toUtf8()), "SHA256SUMS written");

        const PrecheckResult result = precheckFirmware(dir);
        check(result.ok, "matching checksums pass");
        check(!result.blocksFlashing, "matching checksums do not block");
        check(!result.checksumMismatch, "matching checksums are not a mismatch");
        check(result.level == PrecheckLevel::Info, "matching checksums stay at Info");
        check(result.checksumFile == manifest, "the manifest that was used is reported");
        check(hasDetail(result, QStringLiteral("校验一致")), "manifest verification is reported");
    }

    // ── 4) 校验不匹配 → Error + checksumMismatch + 拦刷机 ────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("sumbad"));
        check(QDir().mkpath(dir), "checksum-bad fixture dir created");
        const QString boot = QDir(dir).filePath(QStringLiteral("boot.img"));
        writeImage(dir, QStringLiteral("boot.img"), 400);

        // 故意写一个「结构合法但不对」的摘要。
        const QString wrong = QString(64, QLatin1Char('a'));
        check(writeFile(QDir(dir).filePath(QStringLiteral("SHA256SUMS")),
                        (kc::tools::formatChecksumLine(wrong, QStringLiteral("boot.img"))
                         + QStringLiteral("\n")).toUtf8()),
              "bad SHA256SUMS written");

        const PrecheckResult result = precheckFirmware(dir);
        check(!result.ok, "mismatched checksum is not ok");
        check(result.blocksFlashing, "mismatched checksum blocks flashing");
        check(result.checksumMismatch, "mismatched checksum sets checksumMismatch");
        check(result.level == PrecheckLevel::Error, "mismatched checksum is Error");
        check(hasDetail(result, QStringLiteral("摘要不一致")), "mismatch is explained");
        Q_UNUSED(boot);
    }

    // ── 4b) 同名旁挂 .md5 不匹配也要拦住 ────────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("sidecarmd5"));
        check(QDir().mkpath(dir), "md5 sidecar fixture dir created");
        const QString system = QDir(dir).filePath(QStringLiteral("system.img"));
        writeImage(dir, QStringLiteral("system.img"), 320);
        writeImage(dir, QStringLiteral("boot.img"), 64);
        check(writeSidecar(system, QString(32, QLatin1Char('b')), false), "bad .md5 written");

        const PrecheckResult result = precheckFirmware(dir);
        check(!result.ok && result.checksumMismatch, "bad .md5 sidecar blocks flashing");
        check(hasDetail(result, QStringLiteral("system.img")), "sidecar mismatch names the file");
    }

    // ── 4c) 同名旁挂 .md5 一致 → 通过 ───────────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("sidecarok"));
        check(QDir().mkpath(dir), "md5-ok fixture dir created");
        const QString boot = QDir(dir).filePath(QStringLiteral("boot.img"));
        writeImage(dir, QStringLiteral("boot.img"), 96);
        check(writeSidecar(boot, md5Of(boot), false), "good .md5 written");

        const PrecheckResult result = precheckFirmware(dir);
        check(result.ok, "matching .md5 sidecar passes");
        check(!result.checksumMismatch, "matching .md5 is not a mismatch");
        check(hasDetail(result, QStringLiteral("一致")), "sidecar match is reported");
    }

    // ── 5) XML 声明 size 对不上（截断）→ Error ───────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("truncated"));
        check(QDir().mkpath(dir), "truncated fixture dir created");
        const QString boot = QDir(dir).filePath(QStringLiteral("boot.img"));
        writeImage(dir, QStringLiteral("boot.img"), 100);

        // XML 声明 4096 字节，磁盘只有 100 字节。
        const QString xml = QStringLiteral(
            "<?xml version=\"1.0\" ?>\n<data>\n"
            "  <program SECTOR_SIZE_IN_BYTES=\"512\" filename=\"boot.img\" label=\"boot\" "
            "num_partition_sectors=\"8\" physical_partition_number=\"0\" "
            "size_in_bytes=\"4096\" sparse=\"false\" start_sector=\"0\" />\n"
            "</data>\n");
        check(writeFile(QDir(dir).filePath(QStringLiteral("rawprogram0.xml")), xml.toUtf8()),
              "rawprogram xml written");

        const PrecheckResult result = precheckFirmware(dir);
        check(!result.ok, "truncated image is not ok");
        check(result.blocksFlashing, "truncated image blocks flashing");
        check(hasDetail(result, QStringLiteral("被截断")), "truncation is reported");
        check(hasDetail(result, QStringLiteral("4096")), "declared size is quoted in the message");
        Q_UNUSED(boot);
    }

    // ── 5b) XML 声明 size 与磁盘一致 → 通过 ─────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("xmlok"));
        check(QDir().mkpath(dir), "xml-ok fixture dir created");
        writeImage(dir, QStringLiteral("boot.img"), 1024);
        const QString xml = QStringLiteral(
            "<?xml version=\"1.0\" ?>\n<data>\n"
            "  <program SECTOR_SIZE_IN_BYTES=\"512\" filename=\"boot.img\" label=\"boot\" "
            "num_partition_sectors=\"2\" physical_partition_number=\"0\" "
            "size_in_bytes=\"1024\" sparse=\"false\" start_sector=\"0\" />\n"
            "</data>\n");
        check(writeFile(QDir(dir).filePath(QStringLiteral("rawprogram0.xml")), xml.toUtf8()),
              "matching rawprogram xml written");

        const PrecheckResult result = precheckFirmware(dir);
        check(result.ok, "matching XML size passes");
        check(!result.blocksFlashing, "matching XML size does not block");
        check(hasDetail(result, QStringLiteral("与磁盘一致")), "XML size agreement is reported");
    }

    // ── 5c) XML 引用的镜像整个缺失 → Error ──────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("xmlmissing"));
        check(QDir().mkpath(dir), "xml-missing fixture dir created");
        writeImage(dir, QStringLiteral("boot.img"), 64);
        const QString xml = QStringLiteral(
            "<?xml version=\"1.0\" ?>\n<data>\n"
            "  <program SECTOR_SIZE_IN_BYTES=\"512\" filename=\"boot.img\" label=\"boot\" "
            "num_partition_sectors=\"1\" physical_partition_number=\"0\" "
            "size_in_bytes=\"64\" sparse=\"false\" start_sector=\"0\" />\n"
            "  <program SECTOR_SIZE_IN_BYTES=\"512\" filename=\"system.img\" label=\"system\" "
            "num_partition_sectors=\"1\" physical_partition_number=\"0\" "
            "size_in_bytes=\"512\" sparse=\"false\" start_sector=\"8\" />\n"
            "</data>\n");
        check(writeFile(QDir(dir).filePath(QStringLiteral("rawprogram0.xml")), xml.toUtf8()),
              "xml referencing a missing image written");

        const PrecheckResult result = precheckFirmware(dir);
        check(!result.ok, "missing image referenced by XML is an error");
        check(hasDetail(result, QStringLiteral("不存在")), "missing image is reported");
    }

    // ── 6) 目录里一个镜像都没有 → Error ─────────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("notfirmware"));
        check(QDir().mkpath(dir), "not-firmware fixture dir created");
        check(writeFile(QDir(dir).filePath(QStringLiteral("readme.txt")),
                        QByteArrayLiteral("hello")),
              "readme written");

        const PrecheckResult result = precheckFirmware(dir);
        check(!result.ok && result.blocksFlashing, "directory without images blocks flashing");
        check(hasDetail(result, QStringLiteral("没有 .img")), "empty image set is reported");
    }

    // ── 7) 关键镜像缺失 → Warning（有镜像但不含引导镜像）────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("noboot"));
        check(QDir().mkpath(dir), "no-boot fixture dir created");
        writeImage(dir, QStringLiteral("system.img"), 128);
        writeImage(dir, QStringLiteral("vendor.img"), 128);

        const PrecheckResult result = precheckFirmware(dir);
        check(result.ok, "missing boot images alone do not block");
        check(result.level == PrecheckLevel::Warning, "missing boot images raise a warning");
        check(hasDetail(result, QStringLiteral("boot / init_boot / vendor_boot")),
              "missing boot images are named");
    }

    // ── 8) 压缩包：只核整包摘要，没有校验文件时是 Warning ───────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("archive"));
        check(QDir().mkpath(dir), "archive fixture dir created");
        const QString zip = QDir(dir).filePath(QStringLiteral("rom.zip"));
        check(writeFile(zip, QByteArrayLiteral("PK\x03\x04 not really a zip")), "fake zip written");

        const PrecheckResult without = precheckFirmware(zip);
        check(without.ok, "archive without sidecar does not block");
        check(without.level == PrecheckLevel::Warning, "archive without sidecar is a warning");
        check(hasDetail(without, QStringLiteral("不解压")), "archive limitation is stated");

        check(writeSidecar(zip, sha256Of(zip), true), "archive .sha256 written");
        const PrecheckResult with = precheckFirmware(zip);
        check(with.ok, "archive with matching sidecar passes");
        check(!with.checksumMismatch, "archive with matching sidecar is not a mismatch");
    }

    // ── 9) 压缩包摘要不匹配 → Error ─────────────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("archivebad"));
        check(QDir().mkpath(dir), "bad-archive fixture dir created");
        const QString zip = QDir(dir).filePath(QStringLiteral("rom.7z"));
        check(writeFile(zip, QByteArrayLiteral("7z fake payload")), "fake 7z written");
        check(writeSidecar(zip, QString(64, QLatin1Char('c')), true), "bad archive .sha256 written");

        const PrecheckResult result = precheckFirmware(zip);
        check(!result.ok && result.blocksFlashing, "archive checksum mismatch blocks");
        check(result.checksumMismatch, "archive checksum mismatch flag set");
    }

    // ── 10) 直接传 rawprogram XML 也能预检 ──────────────────────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("xmlonly"));
        check(QDir().mkpath(dir), "xml-only fixture dir created");
        writeImage(dir, QStringLiteral("boot.img"), 128);
        const QString xmlPath = QDir(dir).filePath(QStringLiteral("rawprogram0.xml"));
        const QString xml = QStringLiteral(
            "<?xml version=\"1.0\" ?>\n<data>\n"
            "  <program SECTOR_SIZE_IN_BYTES=\"512\" filename=\"boot.img\" label=\"boot\" "
            "num_partition_sectors=\"1\" physical_partition_number=\"0\" "
            "size_in_bytes=\"128\" sparse=\"false\" start_sector=\"0\" />\n"
            "</data>\n");
        check(writeFile(xmlPath, xml.toUtf8()), "standalone xml written");

        const PrecheckResult result = precheckFirmware(xmlPath);
        check(result.ok, "standalone rawprogram xml passes when sizes agree");
        check(result.imageCount == 1, "standalone xml counts sibling images");
    }

    // ── 11) verifyChecksums=false：不做摘要比对，但仍解析清单 ────────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("noverify"));
        check(QDir().mkpath(dir), "no-verify fixture dir created");
        const QString boot = QDir(dir).filePath(QStringLiteral("boot.img"));
        writeImage(dir, QStringLiteral("boot.img"), 256);
        // 故意写错的摘要：关掉比对后就不该再判不一致。
        check(writeFile(QDir(dir).filePath(QStringLiteral("SHA256SUMS")),
                        (kc::tools::formatChecksumLine(QString(64, QLatin1Char('d')),
                                                       QStringLiteral("boot.img"))
                         + QStringLiteral("\n")).toUtf8()),
              "wrong SHA256SUMS written for the no-verify case");

        PrecheckOptions options;
        options.verifyChecksums = false;
        const PrecheckResult result = precheckFirmware(dir, options);
        check(result.ok, "verifyChecksums=false skips digest comparison");
        check(!result.checksumMismatch, "verifyChecksums=false never reports a mismatch");
        check(hasDetail(result, QStringLiteral("未做摘要比对")), "skipped comparison is stated");
        Q_UNUSED(boot);
    }

    // ── 12) 级别文案 ────────────────────────────────────────────────────
    check(kc::tools::precheckLevelText(PrecheckLevel::Info) == QStringLiteral("通过"),
          "Info level text");
    check(kc::tools::precheckLevelText(PrecheckLevel::Warning) == QStringLiteral("提醒"),
          "Warning level text");
    check(kc::tools::precheckLevelText(PrecheckLevel::Error) == QStringLiteral("失败"),
          "Error level text");

    // ── 13) 0 字节 super.img 是动态分区包的合法占位 → 只提醒不拦 ─────────
    {
        const QString dir = QDir(root).filePath(QStringLiteral("dynsuper"));
        check(QDir().mkpath(dir), "dynamic-super fixture dir created");
        writeImage(dir, QStringLiteral("super.img"), 0);
        writeImage(dir, QStringLiteral("boot.img"), 64);

        const PrecheckResult result = precheckFirmware(dir);
        check(result.ok, "0-byte super.img placeholder does not block");
        check(result.level == PrecheckLevel::Warning, "0-byte super.img is a warning");
        check(hasDetail(result, QStringLiteral("0 字节占位")), "super placeholder is explained");
    }

    if (failures == 0) {
        std::cout << "firmware-precheck-tests: all checks passed\n";
        return 0;
    }
    std::cerr << "firmware-precheck-tests: " << failures << " check(s) failed\n";
    return 1;
}
