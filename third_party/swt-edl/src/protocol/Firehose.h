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
    bool erase(quint64 startSector, quint64 sectorCount, quint32 lun,
               quint32 sectorSize, QString* error,
               const QString& label = QString(),
               const QString& filename = QString());
    bool patch(quint32 sectorSize, quint64 byteOffset, quint32 lun,
               quint64 sizeInBytes, const QString& startSector,
               const QString& value, QString* error);
    bool setBootableStorageDrive(quint32 value, QString* error);
    bool reboot(bool toEdl, QString* error);

    void setProgressCallback(ProgressCallback progress) { m_progress = std::move(progress); }
    void enrichDeviceInfo(DeviceInfo* info) const;

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
    QHash<QString, QString> m_vipSignedFilenames;
    QString m_detectedSerial;
    QString m_detectedChipId;
    QString m_detectedModel;
    QString m_detectedStorageType;
    QString m_detectedStorageProduct;
    quint64 m_detectedStorageBytes = 0;
};

} // namespace swt::edl
