#pragma once

#include "core/Types.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <atomic>
#include <utility>

class QIODevice;

namespace swt::edl {

class SerialTransport;

struct FirehoseResponse {
    bool ack = false;
    bool nak = false;
    bool rawMode = false;
    quint64 negotiatedPayload = 0;
    QStringList messages;
    QString xml;
};

class FirehoseClient final {
public:
    FirehoseClient(SerialTransport& transport, std::atomic_bool& cancelled,
                   LogCallback log, ProgressCallback progress);

    bool configure(StorageType storage, QString* error);
    bool initializeOplusVip(const QString& digestPath, const QString& signaturePath,
                            StorageType storage, QString* error);
    void resumeOplusVip(const QString& digestPath);
    bool ping(QString* error, int timeoutMs = 5000);
    bool getStorageInfo(quint32 lun, QString* error);
    /// 同上，并输出该 LUN 的字节容量（loader 未返回时为 0）。
    /// 空设备（LUN 上没有 GPT）时，这是 NUM_DISK_SECTORS 唯一的合法来源。
    bool getStorageInfo(quint32 lun, quint64* storageBytes, QString* error);
    bool readToDevice(QIODevice& output, quint64 startSector, quint64 sectorCount,
                      quint32 lun, quint32 sectorSize, QString* error,
                      const QString& label = QString(),
                      const QString& filename = QString());
    bool readToByteArray(QByteArray* output, quint64 startSector, quint64 sectorCount,
                         quint32 lun, quint32 sectorSize, QString* error,
                         const QString& label = QString(),
                         const QString& filename = QString());
    bool program(QIODevice& input, quint64 inputSize, quint64 startSector,
                 quint64 maximumSectors, quint32 lun, quint32 sectorSize, QString* error,
                 const QString& label = QString(),
                 const QString& filename = QString());
    /// Firehose `<erase>` 是设备固件层动作（UFS UNMAP / eMMC TRIM）：loader 回 ACK
    /// 只代表「命令被接受」，不代表数据真的被清掉（实测存在静默 ACK 却不落地的
    /// loader）。因此擦除前后各回读首/尾哨兵扇区比对，把「假擦除」从「报成功」里
    /// 分离出来。返回 Failed 之外的结果时 error 会是给用户看的完整说明。
    enum class EraseOutcome {
        Failed,     ///< 命令层失败（写失败 / NAK / 超时）—— 通道可能已不可用
        NotLanded,  ///< loader 回 ACK，但回读证明数据仍在 => 假擦除，调用方必须报失败
        Unverified, ///< loader 回 ACK，但哨兵基线缺失或回读失败，结果未校验
        Verified,   ///< loader 回 ACK，首尾哨兵内容变化或为零，不保证整段已清空
    };
    EraseOutcome erase(quint64 startSector, quint64 sectorCount, quint32 lun,
                       quint32 sectorSize, QString* error,
                       const QString& label = QString(),
                       const QString& filename = QString());

    bool patch(quint32 sectorSize, quint64 byteOffset, quint32 lun,
               quint64 sizeInBytes, const QString& startSector,
               const QString& value, QString* error);
    bool setBootableStorageDrive(quint32 value, QString* error);
    /// Power 命令是 fire-and-close：设备常常在命令被接受后立刻拆 USB，ACK 到不了
    /// 主机。因此「写入成功但没收到最终 ACK」按已提交处理并返回 true，同时用
    /// acknowledged 区分「设备已确认」与「仅提交、未确认」——调用方不该把后者
    /// 当成「设备一定会重启」的前提。
    bool reboot(bool toEdl, QString* error, bool* acknowledged = nullptr);

    void setProgressCallback(ProgressCallback progress) { m_progress = std::move(progress); }
    void enrichDeviceInfo(DeviceInfo* info) const;

    /// 最近一次 getstorageinfo 观察到的存储类型（UFS/eMMC）与产品型号。
    /// 逐 LUN 查询后立刻取用 —— 值随每次查询刷新，跨 LUN 只反映最后一次。
    [[nodiscard]] QString detectedStorageType() const { return m_detectedStorageType; }
    [[nodiscard]] QString detectedStorageProduct() const { return m_detectedStorageProduct; }
    /// 存储厂商（UFS/eMMC 的 vendor 行，如 SAMSUNG / MICRON / SKHYNIX）。
    /// 来源是 loader 运行时打印的 `INFO: ufs: <vendor>` 文本行。
    [[nodiscard]] QString detectedStorageVendor() const { return m_detectedStorageVendor; }
    /// 字库芯片序列号（loader 打印的 `Chip serial num (0x…)`）。
    /// 「小米字库牌子」页拿它区分同一厂商的不同批次/颗粒；空 = loader 没打这行。
    [[nodiscard]] QString detectedChipSerial() const { return m_detectedSerial; }

    [[nodiscard]] quint64 maximumPayload() const { return m_maximumPayload; }

private:
    bool transact(const QString& xml, FirehoseResponse* response, int timeoutMs, QString* error);
    bool receiveResponse(FirehoseResponse* response, int timeoutMs, QString* error);
    bool waitForVipPartitionRequest(quint64* requestedCapacity, int timeoutMs, QString* error);
    bool receiveVipPartitionResult(FirehoseResponse* response, int timeoutMs, QString* error);
    static FirehoseResponse parseResponse(const QByteArray& frame);
    bool checkAck(const FirehoseResponse& response, const QString& operation, QString* error) const;
    bool sendVipFile(const QString& path, const QString& label, QString* error);
    bool sendInitialVipPartition(const QString& path, QString* error);
    void observeMessages(const QStringList& messages);

    SerialTransport& m_transport;
    std::atomic_bool& m_cancelled;
    LogCallback m_log;
    ProgressCallback m_progress;
    quint64 m_maximumPayload = 1024 * 1024;
    bool m_oplusVip = false;
    /// getstorageinfo 正在查询的 LUN（-1 = 当前不在查询）。
    /// storage_info 里的标量形态（total_blocks / lun_size 单值）只有靠这个
    /// 上下文才知道归属哪个 LUN；查询期外一律不猜。
    int m_storageInfoQueryLun = -1;
    /// 各 LUN 容量（字节），来自 getstorageinfo 响应。
    QHash<quint32, quint64> m_lunStorageBytes;
    QHash<QString, QString> m_vipSignedFilenames;
    QString m_detectedSerial;
    QString m_detectedChipId;
    QString m_detectedModel;
    QString m_detectedStorageType;
    QString m_detectedStorageProduct;
    QString m_detectedStorageVendor;
    quint64 m_detectedStorageBytes = 0;
};

} // namespace swt::edl
