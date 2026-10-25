#include "kc/devices/StorageBrandProbe.h"

#include <QDir>
#include <QFile>
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

void touchFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write("stub");
}

} // namespace

// 「小米字库牌子」解析回归（移植自 MiUfsProbe 的 StorageProbeResult.FromText）：
//   · loader 打印 `INFO: ufs: <vendor>` 是字库厂商唯一的文本来源；
//   · 只有 `INFO: emmc`（没有厂商）时不能凭空编一个厂商出来；
//   · `Chip serial num (…)` 单独成行也要收到；
//   · 厂牌表要覆盖 loader 的各种后缀写法（SAMSUNG ELECTRONICS / SK HYNIX INC.）；
//   · 固件目录里挑引导文件要按 MiUfsProbe 的优先级（lite > devprg > 其他）。
int main()
{
    using namespace kc::devices;

    // ── parseStorageBrand ────────────────────────────────────────────────────
    {
        const auto r = parseStorageBrand(
            QStringLiteral("<response value=\"ACK\" rawmode=\"false\">INFO: ufs: SAMSUNG\n"));
        check(r.storageType == QStringLiteral("UFS"), "ufs line sets storage type");
        check(r.vendor == QStringLiteral("SAMSUNG"), "ufs line sets vendor");
        check(r.rawLine.contains(QStringLiteral("INFO: ufs: SAMSUNG")),
              "matched line is preserved for the UI");
    }
    {
        const auto r = parseStorageBrand(QStringLiteral("INFO: emmc: micron "));
        check(r.storageType == QStringLiteral("EMMC"), "emmc line sets storage type");
        check(r.vendor == QStringLiteral("MICRON"), "vendor is trimmed and uppercased");
    }
    {
        // 只有类型、没有厂商 —— 必须留空，不能把 "INFO" 当厂商。
        const auto r = parseStorageBrand(QStringLiteral("<data><log value=\"INFO: emmc\"/>"));
        check(r.storageType == QStringLiteral("EMMC"), "bare storage line sets type");
        check(r.vendor.isEmpty(), "bare storage line must not invent a vendor");
    }
    {
        const auto r = parseStorageBrand(
            QStringLiteral("INFO: ufs: SK HYNIX\nChip serial num (0x1a2b3c4d)"));
        check(r.vendor == QStringLiteral("SK HYNIX"), "multi-word vendor survives cleaning");
        check(r.chipSerial == QStringLiteral("0x1a2b3c4d"), "chip serial is captured");
    }
    {
        const auto r = parseStorageBrand(
            QStringLiteral("INFO: ufs: TOSHIBA.\nChip serial num (0xabc)"));
        check(r.vendor == QStringLiteral("TOSHIBA"), "trailing dot is stripped from vendor");
    }
    check(parseStorageBrand(QString()).isEmpty(), "empty text yields empty result");
    check(parseStorageBrand(QStringLiteral("no storage info here")).isEmpty(),
          "unrelated text yields empty result");

    // ── 厂牌表 ──────────────────────────────────────────────────────────────
    check(storageBrandChineseName(QStringLiteral("SAMSUNG")) == QStringLiteral("三星"),
          "SAMSUNG maps to 三星");
    check(storageBrandEnglishName(QStringLiteral("SAMSUNG ELECTRONICS")) == QStringLiteral("Samsung"),
          "vendor suffix still matches the brand table");
    check(storageBrandChineseName(QStringLiteral("SK HYNIX")) == QStringLiteral("SK 海力士"),
          "SK HYNIX maps to SK 海力士");
    check(storageBrandChineseName(QStringLiteral("TOSHIBA")) == QStringLiteral("铠侠"),
          "TOSHIBA maps to its current brand 铠侠");
    check(storageBrandChineseName(QStringLiteral("SOMETHING NEW")).isEmpty(),
          "unknown vendor has no Chinese name");
    {
        StorageBrandResult r;
        r.vendor = QStringLiteral("MICRON");
        check(storageBrandTitle(r) == QStringLiteral("美光 Micron"), "title joins zh + en");
        StorageBrandResult unknown;
        unknown.vendor = QStringLiteral("ACME");
        check(storageBrandTitle(unknown) == QStringLiteral("ACME"),
              "unknown vendor falls back to the raw string");
    }

    // ── findProgrammerInDirectory ────────────────────────────────────────────
    {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary directory is available");
        const QString root = temp.path();
        const QString images = QDir(root).filePath(QStringLiteral("images"));
        check(QDir().mkpath(images), "images directory is created");

        // 只有 devprg 时选它。
        touchFile(QDir(images).filePath(QStringLiteral("xbl_s_devprg_ns.melf")));
        check(findProgrammerInDirectory(root) ==
                  QDir(images).filePath(QStringLiteral("xbl_s_devprg_ns.melf")),
              "ROM root drills into images and finds the devprg loader");

        // 加了 lite 之后 lite 优先。
        touchFile(QDir(images).filePath(QStringLiteral("prog_firehose_lite.elf")));
        check(findProgrammerInDirectory(images) ==
                  QDir(images).filePath(QStringLiteral("prog_firehose_lite.elf")),
              "prog_firehose_lite.elf outranks devprg");

        // 无关文件不能入选。
        touchFile(QDir(images).filePath(QStringLiteral("boot.img")));
        check(findProgrammerInDirectory(images).endsWith(QStringLiteral("prog_firehose_lite.elf")),
              "unrelated images are ignored");
    }
    {
        QTemporaryDir temp;
        check(temp.isValid(), "temporary directory is available (empty case)");
        check(findProgrammerInDirectory(temp.path()).isEmpty(),
              "empty directory has no programmer");
        check(findProgrammerInDirectory(QStringLiteral("Z:/definitely/not/here")).isEmpty(),
              "missing directory has no programmer");
    }

    if (failures != 0) {
        std::cerr << failures << " StorageBrand assertion(s) failed\n";
        return 1;
    }
    std::cout << "Storage brand probe tests passed\n";
    return 0;
}
