#include "kc/mtk/MtkTypes.h"

#include <QRegularExpression>

namespace kc::mtk {
namespace {

/// 从 region 串里抠出 "LU<digit>" 的下标；没有则返回 -1。
/// 🔴 不能写成 "\\bLU"：region 串是 UFS_LU0 / EMMC_BOOT_1 这种下划线风格，
///    而 `_` 属于 \w，`_` 与 `L` 之间**没有词边界** → 正则永远不匹配 → 静默回退成 LU0。
int ufsLuIndex(const QString& upperRegion)
{
    static const QRegularExpression re(QStringLiteral("LU\\s*([0-9])"));
    const auto m = re.match(upperRegion);
    if (!m.hasMatch()) return -1;
    bool ok = false;
    const int idx = m.captured(1).toInt(&ok);
    return ok ? idx : -1;
}

} // namespace

bool regionToTarget(const QString& region, StorageType storageHint, DaTarget* out)
{
    if (!out) return false;

    const QString r = region.trimmed().toUpper();

    // region 串里通常自带存储类型（EMMC_USER / UFS_LU0 / NAND …）；
    // 认不出来（空串 / 纯 "user")时才回退到 scatter 顶层 storage 字段。
    StorageType st = storageTypeFromName(r);
    if (st == StorageType::None) st = storageHint;
    if (st == StorageType::None) return false;

    switch (st) {
    case StorageType::Emmc:
    case StorageType::Sdmmc: {
        out->storage = (st == StorageType::Sdmmc) ? DaStorage::Sdmmc : DaStorage::Emmc;
        if (r.contains(QStringLiteral("BOOT_1")) || r.contains(QStringLiteral("BOOT1"))) {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::Boot1);
            out->label = QStringLiteral("EMMC-BOOT1");
        } else if (r.contains(QStringLiteral("BOOT_2")) || r.contains(QStringLiteral("BOOT2"))) {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::Boot2);
            out->label = QStringLiteral("EMMC-BOOT2");
        } else if (r.contains(QStringLiteral("RPMB"))) {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::Rpmb);
            out->label = QStringLiteral("EMMC-RPMB");
        } else if (r.contains(QStringLiteral("GP1"))) {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::Gp1);
            out->label = QStringLiteral("EMMC-GP1");
        } else if (r.contains(QStringLiteral("GP2"))) {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::Gp2);
            out->label = QStringLiteral("EMMC-GP2");
        } else if (r.contains(QStringLiteral("GP3"))) {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::Gp3);
            out->label = QStringLiteral("EMMC-GP3");
        } else if (r.contains(QStringLiteral("GP4"))) {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::Gp4);
            out->label = QStringLiteral("EMMC-GP4");
        } else {
            out->partType = static_cast<uint32_t>(EmmcPartitionType::User);
            out->label = QStringLiteral("EMMC-USER");
        }
        return true;
    }

    case StorageType::Ufs: {
        out->storage = DaStorage::Ufs;
        // 🔴 LU→parttype 沿用 mtkclient（storage.py partitiontype_and_size）：
        //    LU0=user / LU1=boot1 / LU2=boot2 / LU3=rpmb。不同 DA 版本口径可能不同。
        const int lu = ufsLuIndex(r);
        switch (lu) {
        case 1:
            out->partType = static_cast<uint32_t>(UfsPartitionType::Boot1);
            out->label = QStringLiteral("UFS-BOOT1");
            break;
        case 2:
            out->partType = static_cast<uint32_t>(UfsPartitionType::Boot2);
            out->label = QStringLiteral("UFS-BOOT2");
            break;
        case 3:
            out->partType = static_cast<uint32_t>(UfsPartitionType::Rpmb);
            out->label = QStringLiteral("UFS-RPMB");
            break;
        case 0:
        default:
            out->partType = static_cast<uint32_t>(UfsPartitionType::User);
            out->label = QStringLiteral("UFS-LU0");
            break;
        }
        return true;
    }

    case StorageType::Nand:
        out->storage = DaStorage::Nand;
        out->partType = 0; // cellusage（uni）
        out->label = QStringLiteral("NAND");
        return true;

    case StorageType::Nor:
        out->storage = DaStorage::Nor;
        out->partType = 0;
        out->label = QStringLiteral("NOR");
        return true;

    case StorageType::None:
        break;
    }
    return false;
}

} // namespace kc::mtk
