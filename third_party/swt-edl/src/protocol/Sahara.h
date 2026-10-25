#pragma once

#include "core/Types.h"

#include <QByteArray>
#include <QString>

#include <atomic>

class QFile;

namespace swt::edl {

class SerialTransport;

class SaharaClient final {
public:
    SaharaClient(SerialTransport& transport, std::atomic_bool& cancelled,
                 LogCallback log, ProgressCallback progress);

    bool uploadProgrammer(const QString& filePath, DeviceInfo* deviceInfo,
                          bool querySaharaIdentity, QString* error,
                          quint32* endTransferStatus = nullptr);
    bool finishPendingTransfer(QString* error);

private:
    bool readPacket(QByteArray* packet, quint32* command, int timeoutMs, QString* error);
    bool readHelloPacket(QByteArray* packet, int timeoutMs, QString* error);
    bool waitForHello(QByteArray* packet, QString* error);
    bool sendHelloResponse(quint32 mode, QString* error, quint32 version = 2,
                           quint32 minimumVersion = 1);
    bool sendSwitchMode(quint32 mode, QString* error);
    bool sendResetStateMachine(QString* error);
    bool executeCommand(quint32 commandId, QByteArray* data, QString* error);
    bool readDeviceInfo(const QByteArray& hello, DeviceInfo* deviceInfo,
                        QByteArray* nextHello, QString* error);
    bool sendProgrammerRange(QFile& file, quint64 offset, quint64 length,
                             quint64* transferred, QString* error);

    SerialTransport& m_transport;
    std::atomic_bool& m_cancelled;
    LogCallback m_log;
    ProgressCallback m_progress;
};

} // namespace swt::edl
