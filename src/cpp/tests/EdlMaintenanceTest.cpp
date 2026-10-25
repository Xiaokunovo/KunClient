#include "core/EdlMaintenance.h"
#include "core/PartitionProtection.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

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

} // namespace

// EDL 维护操作纯逻辑回归：竞品功能补齐（重启到 Rec/FastbootD、恢复出厂、
// 强开 OEM 解锁、基带备份、存储信息、LUN 擦除、GPT 读写）背后的可单测内核。
// 这些值一旦写错就是「设备起不来 / 擦错分区」级别的事故，必须有测试兜底。
int main()
{
    using swt::edl::applyDevinfoUnlock;
    using swt::edl::basebandBackupPartitionNames;
    using swt::edl::BcbBlockBytes;
    using swt::edl::buildBcbPayload;
    using swt::edl::DevinfoUnlockByteOffset;
    using swt::edl::DevinfoUnlockResult;
    using swt::edl::factoryResetPartitionNames;
    using swt::edl::gptFileName;

    // ── buildBcbPayload ────────────────────────────────────────────────────
    const QByteArray recovery = buildBcbPayload(QStringLiteral("boot-recovery"));
    check(recovery.size() == static_cast<qsizetype>(BcbBlockBytes),
          "BCB payload is exactly 2048 bytes");
    check(recovery.startsWith(QByteArrayLiteral("boot-recovery\0")),
          "BCB command sits at offset 0, NUL-terminated");
    check(recovery.mid(15).count('\0') == recovery.size() - 15,
          "everything after the command is zeroed");
    const QByteArray fastbootd = buildBcbPayload(QStringLiteral("boot-fastboot"));
    check(fastbootd.size() == static_cast<qsizetype>(BcbBlockBytes) &&
              fastbootd.startsWith(QByteArrayLiteral("boot-fastboot\0")),
          "boot-fastboot payload is well formed");
    check(buildBcbPayload(QString()).isEmpty(),
          "empty command must be rejected (no silent zero block)");
    check(buildBcbPayload(QString(32, QLatin1Char('x'))).isEmpty(),
          "32-char command exceeds command[32] and must be rejected");
    check(buildBcbPayload(QString(31, QLatin1Char('x'))).size() ==
              static_cast<qsizetype>(BcbBlockBytes),
          "31-char command still fits command[32]");

    // ── applyDevinfoUnlock ─────────────────────────────────────────────────
    QByteArray sector(4096, '\0');
    sector[static_cast<qsizetype>(DevinfoUnlockByteOffset)] = '\0';
    QString error;
    check(applyDevinfoUnlock(&sector, &error) == DevinfoUnlockResult::Changed &&
              error.isEmpty(),
          "locked devinfo byte (0x00) flips to unlocked");
    check(static_cast<uchar>(sector.at(static_cast<qsizetype>(DevinfoUnlockByteOffset))) ==
              0x01,
          "unlock byte is now 0x01");
    // 第二次应用必须是「已解锁」而不是再次改写
    check(applyDevinfoUnlock(&sector, &error) == DevinfoUnlockResult::AlreadyUnlocked,
          "already-unlocked byte is reported, not rewritten");
    check(static_cast<uchar>(sector.at(static_cast<qsizetype>(DevinfoUnlockByteOffset))) ==
              0x01,
          "already-unlocked byte stays 0x01");

    QByteArray odd(4096, '\0');
    odd[static_cast<qsizetype>(DevinfoUnlockByteOffset)] = '\x02';
    check(applyDevinfoUnlock(&odd, &error) == DevinfoUnlockResult::Rejected &&
              !error.isEmpty() && error.contains(QStringLiteral("02")),
          "unexpected byte value (0x02) is rejected with the value in the message");
    check(static_cast<uchar>(odd.at(static_cast<qsizetype>(DevinfoUnlockByteOffset))) ==
              0x02,
          "rejected devinfo byte is left untouched (no partial write)");

    QByteArray tiny(static_cast<qsizetype>(DevinfoUnlockByteOffset), '\0');
    check(applyDevinfoUnlock(&tiny, &error) == DevinfoUnlockResult::Rejected &&
              error.contains(QString::number(DevinfoUnlockByteOffset)),
          "buffer that cannot reach the unlock offset is rejected");
    check(applyDevinfoUnlock(nullptr, &error) == DevinfoUnlockResult::Rejected,
          "null buffer is rejected");

    // 只动那一个字节：扇区其余内容必须原样保留（写回时不能带上意外改动）
    QByteArray rich(4096, 'A');
    rich[static_cast<qsizetype>(DevinfoUnlockByteOffset)] = '\0';
    applyDevinfoUnlock(&rich, &error);
    bool onlyOneByteChanged = true;
    for (qsizetype index = 0; index < rich.size(); ++index) {
        const char expected = index == static_cast<qsizetype>(DevinfoUnlockByteOffset)
            ? '\x01' : 'A';
        if (rich.at(index) != expected) onlyOneByteChanged = false;
    }
    check(onlyOneByteChanged, "applyDevinfoUnlock touches exactly one byte");

    // ── 预设名单 ───────────────────────────────────────────────────────────
    const QStringList factory = factoryResetPartitionNames();
    check(factory == QStringList({QStringLiteral("userdata"), QStringLiteral("metadata")}),
          "factory reset preset is exactly userdata + metadata");
    // 恢复出厂走的是擦除链路：预设分区绝不能在保护名单里，否则策略会静默跳过。
    for (const QString& name : factory) {
        check(!swt::edl::isProtectedPartitionName(name),
              "factory reset target must not be a protected partition");
    }

    const QStringList baseband = basebandBackupPartitionNames();
    check(baseband.size() == 6, "baseband preset has 6 entries");
    for (const QString& name : baseband) {
        check(name == name.toLower(), "baseband preset entries are lowercase");
        check(baseband.count(name) == 1, "baseband preset has no duplicates");
    }
    check(baseband.contains(QStringLiteral("modem")) &&
              baseband.contains(QStringLiteral("modemst1")) &&
              baseband.contains(QStringLiteral("modemst2")) &&
              baseband.contains(QStringLiteral("fsg")) &&
              baseband.contains(QStringLiteral("fsc")) &&
              baseband.contains(QStringLiteral("bluetooth")),
          "baseband preset covers modem / NV / SIM / bluetooth");

    // 预设匹配按归一化名（忽略 _a/_b）：这是 EdlWorker::matchPartitionsByPreset
    // 依赖的契约，A/B 设备两个槽位都要能命中。
    for (const QString& name : baseband) {
        check(swt::edl::normalizedPartitionName(name + QStringLiteral("_a")) == name,
              "slot-suffixed baseband name normalizes onto the preset entry");
        check(swt::edl::normalizedPartitionName(name + QStringLiteral("_b")) == name,
              "slot-suffixed baseband name (B) normalizes onto the preset entry");
    }
    check(swt::edl::normalizedPartitionName(QStringLiteral("userdata_a")) ==
              QStringLiteral("userdata"),
          "userdata_a normalizes onto the factory reset preset");
    check(swt::edl::normalizedPartitionName(QStringLiteral("metadata_b")) ==
              QStringLiteral("metadata"),
          "metadata_b normalizes onto the factory reset preset");

    // ── gptFileName ────────────────────────────────────────────────────────
    check(gptFileName(0, false) == QStringLiteral("gpt_main0.bin"),
          "primary GPT file name for LUN 0");
    check(gptFileName(3, true) == QStringLiteral("gpt_backup3.bin"),
          "backup GPT file name for LUN 3");

    if (failures == 0) {
        std::cout << "maintenance-tests: all checks passed\n";
        return 0;
    }
    std::cerr << "maintenance-tests: " << failures << " check(s) failed\n";
    return 1;
}
