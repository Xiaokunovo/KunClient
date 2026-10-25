#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

namespace swt::edl {

enum class StorageType {
    Emmc,
    Ufs
};

inline QString storageName(StorageType type)
{
    return type == StorageType::Ufs ? QStringLiteral("ufs") : QStringLiteral("emmc");
}

enum class SessionState {
    Disconnected,
    OpeningPort,
    SaharaHandshake,
    UploadingProgrammer,
    OplusVipVerification,
    WaitingForFirehose,
    Configuring,
    Ready,
    Reading,
    Writing,
    Erasing,
    Rebooting,
    Failed
};

inline QString stateText(SessionState state)
{
    switch (state) {
    case SessionState::Disconnected: return QStringLiteral("未连接");
    case SessionState::OpeningPort: return QStringLiteral("正在打开端口");
    case SessionState::SaharaHandshake: return QStringLiteral("Sahara 握手");
    case SessionState::UploadingProgrammer: return QStringLiteral("传输 Programmer");
    case SessionState::OplusVipVerification: return QStringLiteral("OPPO VIP 验证");
    case SessionState::WaitingForFirehose: return QStringLiteral("等待 Firehose");
    case SessionState::Configuring: return QStringLiteral("配置存储");
    case SessionState::Ready: return QStringLiteral("EDL 已就绪");
    case SessionState::Reading: return QStringLiteral("正在读取");
    case SessionState::Writing: return QStringLiteral("正在写入");
    case SessionState::Erasing: return QStringLiteral("正在擦除");
    case SessionState::Rebooting: return QStringLiteral("正在重启");
    case SessionState::Failed: return QStringLiteral("操作失败");
    }
    return QStringLiteral("未知状态");
}

/// 状态的**稳定机器标识**（小驼峰 ASCII），供外部程序（headless CLI、脚本）判断用。
///
/// ⚠️ 与 [`stateText`] 分开是有意的：`stateText` 是给人看的中文，随时可能改措辞；
/// 而这套 id 会被外部程序当协议值比对，**改动等于破坏兼容**。加状态可以，改已有 id 不行。
inline QString stateKey(SessionState state)
{
    switch (state) {
    case SessionState::Disconnected: return QStringLiteral("disconnected");
    case SessionState::OpeningPort: return QStringLiteral("openingPort");
    case SessionState::SaharaHandshake: return QStringLiteral("saharaHandshake");
    case SessionState::UploadingProgrammer: return QStringLiteral("uploadingProgrammer");
    case SessionState::OplusVipVerification: return QStringLiteral("oplusVipVerification");
    case SessionState::WaitingForFirehose: return QStringLiteral("waitingForFirehose");
    case SessionState::Configuring: return QStringLiteral("configuring");
    case SessionState::Ready: return QStringLiteral("ready");
    case SessionState::Reading: return QStringLiteral("reading");
    case SessionState::Writing: return QStringLiteral("writing");
    case SessionState::Erasing: return QStringLiteral("erasing");
    case SessionState::Rebooting: return QStringLiteral("rebooting");
    case SessionState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("unknown");
}

struct PartitionInfo {
    QString name;
    QString typeGuid;
    QString uniqueGuid;
    quint32 lun = 0;
    quint64 firstLba = 0;
    quint64 lastLba = 0;
    quint64 attributes = 0;

    [[nodiscard]] quint64 sectorCount() const
    {
        return lastLba >= firstLba ? lastLba - firstLba + 1 : 0;
    }
};

struct DeviceInfo {
    QString portName;
    QString storage;
    QString programmer;
    QString serial;
    QString hwid;
    QString pkHash;
    QString vipMode;
    QString brand;
    QString model;
    QString msmId;
    QString saharaVersion;
    QString sblVersion;
    QString storageProduct;
    QString productName;
    QString androidVersion;
    QString systemVersion;
    QString securityPatch;
    QString buildFingerprint;
    quint64 storageBytes = 0;
};

struct OplusVipPackage {
    bool enabled = false;
    QString packageDirectory;
    QString programmerPath;
    // Some OPPO packages contain the same Firehose payload signed by several
    // OEM roots (for example DevprgProgrammer1/2.melf).  Discovery keeps the
    // deterministic preferred file first and exposes the compatible siblings
    // so Sahara can fall through after an authentication NAK.
    QStringList programmerCandidates;
    QString digestPath;
    QString signaturePath;
};

using LogCallback = std::function<void(const QString&)>;
using ProgressCallback = std::function<void(quint64, quint64)>;

} // namespace swt::edl

Q_DECLARE_METATYPE(swt::edl::StorageType)
Q_DECLARE_METATYPE(swt::edl::SessionState)
Q_DECLARE_METATYPE(swt::edl::PartitionInfo)
Q_DECLARE_METATYPE(QVector<swt::edl::PartitionInfo>)
Q_DECLARE_METATYPE(swt::edl::DeviceInfo)
Q_DECLARE_METATYPE(swt::edl::OplusVipPackage)
