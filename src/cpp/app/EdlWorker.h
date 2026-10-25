#pragma once

#include "core/Types.h"

#include <QObject>
#include <QHash>
#include <QStringList>

#include <atomic>
#include <memory>

namespace swt::edl {

class FirehoseClient;
class SerialTransport;

} // namespace swt::edl

class QFile;
class QIODevice;

namespace swt::edl {

class EdlWorker final : public QObject {
    Q_OBJECT

public:
    explicit EdlWorker(QObject* parent = nullptr);
    ~EdlWorker() override;

public slots:
    void connectDevice(const QString& portName, const QString& programmerPath,
                       StorageType storage, const OplusVipPackage& vipPackage);
    void disconnectDevice();
    void readPartitionTable();
    void readSystemVersionInfo();
    void backupPartition(const PartitionInfo& partition, const QString& outputPath);
    void backupPartitions(const QVector<PartitionInfo>& partitions,
                          const QString& outputDirectory, bool generateXml);
    void flashPartition(const PartitionInfo& partition, const QString& imagePath);
    void flashPartitions(const QVector<PartitionInfo>& partitions,
                         const QStringList& imagePaths, bool allowProtected = false);
    void flashXml(const QStringList& xmlPaths, bool allowProtected = false,
                  bool keepUserData = false);
    void erasePartition(const PartitionInfo& partition);
    void erasePartitions(const QVector<PartitionInfo>& partitions,
                         bool allowProtected = false);
    /// 写 BCB 到 misc 分区后 reset —— bcbCommand 取 "boot-recovery" /
    /// "boot-fastboot"（MainWindow 重启菜单的 Recovery / FastbootD 入口）。
    void rebootWithBcb(const QString& bcbCommand);
    /// 通用恢复出厂：擦除 userdata + metadata（仅这两个，不动其余分区）。
    void factoryReset();
    /// 强开 OEM 解锁：devinfo 分区偏移 0x1F4 的字节 0x00→0x01 后重启到系统。
    /// 2024+ 机型可能因 AR fuse 已烧写而无效（调用方负责事先警示）。
    void enableOemUnlock();
    /// 备份基带相关分区（modem / modemst1 / modemst2 / fsg / fsc / bluetooth，
    /// A/B 槽位全收）到指定目录。
    void backupBasebandPartitions(const QString& outputDirectory);
    /// 逐 LUN getstorageinfo：上报存储类型、产品型号与各 LUN 容量（字库信息）。
    void readStorageInfo();
    /// 整盘擦除指定 LUN（0 扇区起擦到容量末尾，分片执行 + 回读校验）。
    void eraseLun(quint32 lun);
    /// 把选定的 GPT 文件（gpt_mainN.bin）写入 LUN 起始扇区。
    void writeGptFile(const QString& path, quint32 lun);
    /// 从 LUN 起始扇区读回 GPT 并存为 .bin 文件。
    void readGptFile(const QString& path, quint32 lun);
    void rebootDevice(bool toEdl = false);
    void requestStop();

signals:
    void logMessage(const QString& message);
    /// TMF 风格操作行（2026-10-18）：先打一行「写入 boot（1/12）....」，完成后在
    /// 同一处补 OK / Error 并带上耗时。对应 LogView 的 beginOperation / appendOperation。
    /// 为什么单开信号而不复用 logMessage：LogView 需要一个「开始 → 收尾」的配对
    /// （耗时是它自己按配对算的），纯文本行里塞不进这个状态。
    void logOperationBegin(const QString& body);
    void logOperationEnd(const QString& body, bool ok);
    void stateChanged(swt::edl::SessionState state, const QString& detail);
    void progressChanged(quint64 completed, quint64 total);
    void overallProgressChanged(quint64 completed, quint64 total);
    void deviceInfoChanged(const swt::edl::DeviceInfo& info);
    void partitionsReady(const QVector<swt::edl::PartitionInfo>& partitions);
    void operationFinished(bool success, const QString& message);

private:
    bool ensureReady(const QString& operation);
    void setState(SessionState state, const QString& detail = {});
    void fail(const QString& message, bool closeTransport = true);
    void finish(const QString& message);
    void restoreProgressCallback();
    quint32 sectorSize() const;
    /// 按归一化名（忽略 _a/_b 槽位）找分区；找不到返回 nullptr。
    const PartitionInfo* findPartition(const QStringList& names) const;
    /// 从当前 GPT 分区表里挑出归一化名命中预设名单的分区（A/B 两槽位全收）。
    QVector<PartitionInfo> matchPartitionsByPreset(const QStringList& preset) const;
    /// 解析某 LUN 的总扇区数（NUM_DISK_SECTORS 的分母）。
    /// 优先用读分区表时从 GPT 备份头拿到的值；该 LUN 没有有效 GPT（空设备 /
    /// 分区表损坏 / 还没读过分区表）时向 loader 逐 LUN 问一次 getstorageinfo。
    /// 两条路都拿不到返回 false —— 调用方必须 fail-closed，不允许把 0 当容量。
    bool resolveLunCapacity(quint32 lun, quint64* sectors);
    /// 读分区表期间给「GPT 读不出来的 LUN」补一次容量探测（只在还没有 GPT
    /// 容量时发问，成功才记日志，失败静默 —— 刷写时会给出明确错误）。
    void harvestLunCapacity(quint32 lun);
    /// 擦除前校验 GPT 给出的 [firstLba, firstLba+sectorCount) 是否落在该 LUN 的
    /// 真实容量内。GPT 主/备不一致或被截断时 lastLba 可能指向别的分区甚至越界，
    /// 而 loader 对越界 erase 仍可能回 ACK —— 擦错范围在日志里看不出来，所以这里
    /// fail-closed：容量拿不到或范围越界一律拒绝执行。
    bool validateEraseRange(const PartitionInfo& partition, QString* error);
    /// 批量版：一次性校验完再动手（擦到一半发现越界会留下半成品状态）。
    bool validateEraseRanges(const QVector<PartitionInfo>& partitions, QString* error);
    /// 擦除循环本体（进度 + 逐分区 + 假擦除判定 + 汇总）。入参已校验过范围。
    void runErasePartitions(const QVector<PartitionInfo>& partitions);
    /// 回读循环本体（进度 + 逐分区落盘 + 可选 XML 生成）。
    void runBackupPartitions(const QVector<PartitionInfo>& partitions,
                             const QString& outputDirectory, bool generateXml);
    /// 重启类命令成功后统一收尾：拆 Firehose 会话、清设备态、置 Disconnected。
    void closeSessionAfterReboot(const QString& destination, bool acknowledged);
    bool programFileRange(QFile& input, quint64 fileOffset, quint64 byteCount,
                          quint64 targetStartSector, quint32 lun, quint32 xmlSectorSize,
                          const QString& label, const QString& filename,
                          quint64* completed, quint64 total, QString* error);
    bool programDeviceRange(QIODevice& input, quint64 byteCount,
                            quint64 targetStartSector, quint32 lun,
                            quint32 xmlSectorSize, const QString& label,
                            const QString& filename, quint64* completed,
                            quint64 total, QString* error);
    bool programFillRange(quint32 fillValue, quint64 byteCount,
                          quint64 targetStartSector, quint32 lun, quint32 xmlSectorSize,
                          const QString& label, const QString& filename,
                          quint64* completed, quint64 total, QString* error);

    std::atomic_bool m_cancelled{false};
    bool m_busy = false;
    StorageType m_storage = StorageType::Ufs;
    DeviceInfo m_deviceInfo;
    std::unique_ptr<SerialTransport> m_transport;
    std::unique_ptr<FirehoseClient> m_firehose;
    QHash<quint32, quint64> m_lunSectorCounts;
    QVector<PartitionInfo> m_currentPartitions;
};

} // namespace swt::edl
