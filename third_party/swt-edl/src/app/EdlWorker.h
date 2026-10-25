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
    void flashXml(const QStringList& xmlPaths, bool allowProtected = false);
    void erasePartition(const PartitionInfo& partition);
    void erasePartitions(const QVector<PartitionInfo>& partitions,
                         bool allowProtected = false);
    void rebootDevice(bool toEdl = false);
    void requestStop();

signals:
    void logMessage(const QString& message);
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
