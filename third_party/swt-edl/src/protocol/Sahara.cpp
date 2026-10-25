#include "protocol/Sahara.h"

#include "core/DeviceIdentity.h"
#include "transport/SerialTransport.h"

#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QtEndian>

#include <cstring>

namespace swt::edl {
namespace {

constexpr quint32 SaharaHello = 0x01;
constexpr quint32 SaharaHelloResponse = 0x02;
constexpr quint32 SaharaReadData = 0x03;
constexpr quint32 SaharaEndTransfer = 0x04;
constexpr quint32 SaharaDone = 0x05;
constexpr quint32 SaharaDoneResponse = 0x06;
constexpr quint32 SaharaCommandReady = 0x0b;
constexpr quint32 SaharaSwitchMode = 0x0c;
constexpr quint32 SaharaExecuteRequest = 0x0d;
constexpr quint32 SaharaExecuteResponse = 0x0e;
constexpr quint32 SaharaExecuteData = 0x0f;
constexpr quint32 SaharaReadData64 = 0x12;
constexpr quint32 SaharaResetStateMachine = 0x13;

constexpr quint32 SerialNumberRead = 0x01;
constexpr quint32 MsmHardwareIdRead = 0x02;
constexpr quint32 OemPkHashRead = 0x03;
constexpr quint32 SblSoftwareVersionRead = 0x07;
constexpr quint32 ChipIdV3Read = 0x0a;

quint32 readU32(const QByteArray& data, qsizetype offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint64 readU64(const QByteArray& data, qsizetype offset)
{
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

void appendU32(QByteArray& data, quint32 value)
{
    const quint32 le = qToLittleEndian(value);
    data.append(reinterpret_cast<const char*>(&le), sizeof(le));
}

QString hexBytes(QByteArrayView data)
{
    return QString::fromLatin1(QByteArray(data.data(), data.size()).toHex());
}

QString cpuName(const QString& msmId)
{
    const QString chip = qualcommChipFromMsmId(msmId);
    if (!chip.isEmpty()) return qualcommChipDisplayName(chip);
    return msmId.isEmpty() ? QStringLiteral("Qualcomm") : msmId.toUpper();
}

QString vendorName(quint32 oemId)
{
    switch (oemId) {
    case 0x0072: return QStringLiteral("Xiaomi");
    // EDL exposes the signing/OEM family rather than the consumer sub-brand.
    // OPPO, OnePlus and realme programmers therefore use the OPLUS family name.
    case 0x0050:
    case 0x0051:
    case 0x0052: return QStringLiteral("OPLUS");
    case 0x0001: return QStringLiteral("Qualcomm");
    case 0x0017: return QStringLiteral("Samsung");
    case 0x0019: return QStringLiteral("LG");
    case 0x001a: return QStringLiteral("Motorola");
    case 0x001b: return QStringLiteral("Lenovo");
    case 0x001e: return QStringLiteral("Huawei");
    case 0x0021: return QStringLiteral("ZTE");
    case 0x0022: return QStringLiteral("Sony");
    case 0x0023: return QStringLiteral("ASUS");
    case 0x0025: return QStringLiteral("vivo");
    case 0x0030: return QStringLiteral("Nokia");
    case 0x0031: return QStringLiteral("Google");
    default: return QStringLiteral("OEM 0x%1").arg(oemId, 4, 16, QLatin1Char('0')).toUpper();
    }
}

QString saharaStatusText(quint32 status)
{
    switch (status) {
    case 0x01: return QStringLiteral("INVALID_CMD");
    case 0x02: return QStringLiteral("PROTOCOL_MISMATCH");
    case 0x05: return QStringLiteral("INVALID_PACKET_SIZE");
    case 0x06: return QStringLiteral("UNEXPECTED_IMAGE_ID");
    case 0x07: return QStringLiteral("INVALID_HEADER_SIZE");
    case 0x08: return QStringLiteral("INVALID_DATA_SIZE");
    case 0x09: return QStringLiteral("INVALID_IMAGE_TYPE");
    case 0x0c: return QStringLiteral("GENERAL_TX_RX_ERROR");
    case 0x14: return QStringLiteral("INVALID_ELF_HEADER");
    case 0x16: return QStringLiteral("TIMEOUT_RX");
    case 0x17: return QStringLiteral("TIMEOUT_TX");
    case 0x21: return QStringLiteral("HASH_TABLE_AUTH_FAILURE（Programmer 签名链与设备不匹配）");
    case 0x22: return QStringLiteral("HASH_VERIFICATION_FAILURE（Programmer 数据校验失败）");
    case 0x23: return QStringLiteral("HASH_TABLE_NOT_FOUND（Programmer 缺少哈希表）");
    default: return QStringLiteral("UNKNOWN_STATUS");
    }
}

} // namespace

SaharaClient::SaharaClient(SerialTransport& transport, std::atomic_bool& cancelled,
                           LogCallback log, ProgressCallback progress)
    : m_transport(transport), m_cancelled(cancelled), m_log(std::move(log)),
      m_progress(std::move(progress))
{
}

bool SaharaClient::readPacket(QByteArray* packet, quint32* command, int timeoutMs, QString* error)
{
    QByteArray header;
    if (!m_transport.readExactly(8, &header, timeoutMs, error)) return false;

    const quint32 packetCommand = readU32(header, 0);
    const quint32 packetLength = readU32(header, 4);
    if (packetLength < 8 || packetLength > 1024 * 1024) {
        if (error) *error = QStringLiteral("Sahara 包长度异常：%1").arg(packetLength);
        return false;
    }

    QByteArray body;
    if (packetLength > 8 &&
        !m_transport.readExactly(packetLength - 8, &body, timeoutMs, error)) {
        return false;
    }

    *packet = header + body;
    *command = packetCommand;
    return true;
}

bool SaharaClient::readHelloPacket(QByteArray* packet, int timeoutMs, QString* error)
{
    if (!packet || timeoutMs <= 0) {
        if (error) *error = QStringLiteral("Sahara Hello 读取参数错误");
        return false;
    }

    packet->clear();
    QElapsedTimer timer;
    timer.start();
    qsizetype skippedBytes = 0;
    QString receiveError;

    // A reopened USB serial handle can expose a few stale bytes or split the
    // Sahara header across completions. Scan for a valid HELLO header so one
    // stray byte does not leave every later packet permanently misaligned.
    while (timer.elapsed() < timeoutMs) {
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        QByteArray header;
        if (!m_transport.peekExactly(8, &header, qMax(1, remaining), &receiveError)) {
            if (error) *error = receiveError;
            return false;
        }

        const quint32 command = readU32(header, 0);
        const quint32 length = readU32(header, 4);
        if (command == SaharaHello && length >= 48 && length <= 4096) {
            const int packetRemaining = timeoutMs - static_cast<int>(timer.elapsed());
            if (packetRemaining <= 0 ||
                !m_transport.readExactly(length, packet, packetRemaining, &receiveError)) {
                if (error) *error = receiveError;
                return false;
            }
            if (skippedBytes > 0 && m_log) {
                m_log(QStringLiteral("[Sahara][DEBUG] Hello 重同步：跳过 %1 个前导字节")
                          .arg(skippedBytes));
            }
            return true;
        }

        QByteArray discarded;
        if (!m_transport.readExactly(1, &discarded, qMax(1, remaining), &receiveError)) {
            if (error) *error = receiveError;
            return false;
        }
        ++skippedBytes;
        if (skippedBytes >= 64 * 1024) {
            if (error) *error = QStringLiteral("Sahara Hello 重同步超过 64 KiB");
            return false;
        }
    }

    if (error) {
        *error = QStringLiteral("等待 Sahara Hello 超时（已跳过 %1 个前导字节）")
                     .arg(skippedBytes);
    }
    return false;
}

bool SaharaClient::sendHelloResponse(quint32 mode, QString* error, quint32 version,
                                     quint32 minimumVersion)
{
    QByteArray response;
    response.reserve(48);
    appendU32(response, SaharaHelloResponse);
    appendU32(response, 48);
    appendU32(response, version);
    appendU32(response, minimumVersion);
    appendU32(response, 0);
    appendU32(response, mode);
    // Qualcomm's command-mode handshake uses the six reserved dwords as the
    // canonical 1..6 sequence. Several Sahara v3 boot ROMs accept CMD_READY
    // with zeroes but then return empty EXECUTE responses.
    for (quint32 i = 0; i < 6; ++i) {
        appendU32(response, mode == 3 ? i + 1 : 0);
    }
    return m_transport.writeAll(response, 3000, error);
}

bool SaharaClient::executeCommand(quint32 commandId, QByteArray* data, QString* error)
{
    if (!data) {
        if (error) *error = QStringLiteral("Sahara 命令输出参数为空");
        return false;
    }

    QByteArray request;
    request.reserve(12);
    appendU32(request, SaharaExecuteRequest);
    appendU32(request, 12);
    appendU32(request, commandId);
    if (!m_transport.writeAll(request, 1500, error)) return false;

    QByteArray response;
    quint32 responseCommand = 0;
    if (!readPacket(&response, &responseCommand, 1200, error)) return false;
    if (responseCommand != SaharaExecuteResponse || response.size() < 16) {
        if (error) {
            *error = QStringLiteral("Sahara EXECUTE 0x%1 收到异常响应 0x%2")
                         .arg(commandId, 0, 16).arg(responseCommand, 0, 16);
        }
        return false;
    }
    const quint32 responseId = readU32(response, 8);
    const quint32 dataLength = readU32(response, 12);
    if (responseId != commandId || dataLength == 0 || dataLength > 1024 * 1024) {
        if (error) {
            *error = QStringLiteral("Sahara EXECUTE 0x%1 返回长度异常：%2")
                         .arg(commandId, 0, 16).arg(dataLength);
        }
        return false;
    }

    QByteArray dataRequest;
    dataRequest.reserve(12);
    appendU32(dataRequest, SaharaExecuteData);
    appendU32(dataRequest, 12);
    appendU32(dataRequest, commandId);
    if (!m_transport.writeAll(dataRequest, 1500, error)) return false;
    return m_transport.readExactly(dataLength, data, 1800, error);
}

bool SaharaClient::readDeviceInfo(const QByteArray& hello, DeviceInfo* deviceInfo,
                                  QByteArray* nextHello, QString* error)
{
    if (!deviceInfo || !nextHello || hello.size() < 48) {
        if (error) *error = QStringLiteral("Sahara Hello 或设备信息参数异常");
        return false;
    }

    const quint32 deviceVersion = readU32(hello, 8);
    const quint32 negotiatedVersion = qBound<quint32>(1, deviceVersion, 3);
    deviceInfo->saharaVersion = QString::number(deviceVersion);
    if (m_log) {
        m_log(QStringLiteral("Sahara 版本 %1，进入命令模式读取设备信息")
                  .arg(deviceVersion));
    }
    if (!sendHelloResponse(3, error, negotiatedVersion, negotiatedVersion)) return false;

    QByteArray packet;
    quint32 command = 0;
    if (!readPacket(&packet, &command, 1800, error) || command != SaharaCommandReady) {
        if (error && command != SaharaCommandReady && !packet.isEmpty()) {
            *error = QStringLiteral("Sahara 命令模式期待 CMD_READY，收到 0x%1")
                         .arg(command, 0, 16);
        }
        return false;
    }

    const bool version3 = deviceVersion >= 3;
    auto query = [this](quint32 commandId) {
        QByteArray result;
        QString commandError;
        if (!executeCommand(commandId, &result, &commandError)) {
            if (m_log) {
                m_log(QStringLiteral("[Sahara][DEBUG] 命令 0x%1 跳过：%2")
                          .arg(commandId, 0, 16).arg(commandError));
            }
            result.clear();
        }
        return result;
    };

    // Execute command ids are independent of the 64-bit Sahara packet ids.
    // Serial remains 0x01 on V3; 0x14 is not a serial command.
    const QByteArray serial = query(SerialNumberRead);
    if (serial.size() >= 4) {
        deviceInfo->serial = QStringLiteral("%1")
                                 .arg(readU32(serial, 0), 8, 16, QLatin1Char('0'));
    }

    quint32 oemId = 0;
    if (version3) {
        const QByteArray chip = query(ChipIdV3Read);
        if (chip.size() >= 46) {
            const quint32 msmId = readU32(chip, 36);
            const quint16 modelId = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar*>(chip.constData() + 42));
            deviceInfo->msmId = QStringLiteral("%1")
                                    .arg(msmId, 8, 16, QLatin1Char('0'));
            oemId = qFromLittleEndian<quint16>(
                reinterpret_cast<const uchar*>(chip.constData() + 40));
            if (oemId == 0) {
                oemId = qFromLittleEndian<quint16>(
                    reinterpret_cast<const uchar*>(chip.constData() + 44));
            }
            deviceInfo->hwid = deviceInfo->msmId +
                QStringLiteral("%1").arg(oemId, 4, 16, QLatin1Char('0')) +
                QStringLiteral("%1").arg(modelId, 4, 16, QLatin1Char('0'));
        } else {
            const QByteArray hardware = query(MsmHardwareIdRead);
            if (hardware.size() >= 8) {
                const QString hardwareId = QStringLiteral("%1")
                    .arg(readU64(hardware, 0), 16, 16, QLatin1Char('0'));
                deviceInfo->hwid = hardwareId;
                deviceInfo->msmId = hardwareId.left(8);
                bool ok = false;
                oemId = hardwareId.mid(8, 4).toUInt(&ok, 16);
                if (!ok) oemId = 0;
            }
        }
    } else {
        const QByteArray hardware = query(MsmHardwareIdRead);
        if (hardware.size() >= 8) {
            const QString hardwareId = QStringLiteral("%1")
                .arg(readU64(hardware, 0), 16, 16, QLatin1Char('0'));
            deviceInfo->hwid = hardwareId;
            deviceInfo->msmId = hardwareId.left(8);
            bool ok = false;
            oemId = hardwareId.mid(8, 4).toUInt(&ok, 16);
            if (!ok) oemId = 0;
        }
    }

    if (!deviceInfo->msmId.isEmpty()) deviceInfo->model = cpuName(deviceInfo->msmId);
    if (oemId != 0) deviceInfo->brand = vendorName(oemId);

    const QByteArray pkHash = query(OemPkHashRead);
    if (pkHash.size() >= 32) deviceInfo->pkHash = hexBytes(QByteArrayView(pkHash).first(32));
    const QByteArray sblVersion = query(SblSoftwareVersionRead);
    if (sblVersion.size() >= 4) {
        deviceInfo->sblVersion = QStringLiteral("%1")
                                     .arg(readU32(sblVersion, 0), 8, 16, QLatin1Char('0'));
    }

    QString switchError;
    // Sahara v3 returns to the image-transfer handshake when SWITCH_MODE is
    // sent with Command (3). Keep ImageTxPending (0) as a compatibility path
    // for older boot ROM revisions.
    if (!sendSwitchMode(version3 ? 3U : 0U, &switchError)) {
        if (error) *error = switchError;
        return false;
    }
    if (readHelloPacket(nextHello, 2500, error)) {
        return true;
    }

    // A few boot ROM revisions use the literal ImageTxPending value instead.
    nextHello->clear();
    if (!sendSwitchMode(0, &switchError)) {
        if (error) *error = switchError;
        return false;
    }
    if (readHelloPacket(nextHello, 2500, error)) {
        return true;
    }
    if (error && error->isEmpty()) {
        *error = QStringLiteral("Sahara 命令模式退出后未收到新的 Hello");
    }
    return false;
}

bool SaharaClient::sendSwitchMode(quint32 mode, QString* error)
{
    QByteArray packet;
    packet.reserve(12);
    appendU32(packet, SaharaSwitchMode);
    appendU32(packet, 12);
    appendU32(packet, mode);
    return m_transport.writeAll(packet, 3000, error);
}

bool SaharaClient::sendResetStateMachine(QString* error)
{
    QByteArray packet;
    packet.reserve(8);
    appendU32(packet, SaharaResetStateMachine);
    appendU32(packet, 8);
    return m_transport.writeAll(packet, 3000, error);
}

bool SaharaClient::waitForHello(QByteArray* packet, QString* error)
{
    QString receiveError;
    // connectDevice() already performed a passive state probe. A second long
    // wait here only delays BootROMs which emit HELLO after SWITCH_MODE.
    if (readHelloPacket(packet, 600, &receiveError)) {
        return true;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            if (error) *error = QStringLiteral("操作已停止");
            return false;
        }
        if (m_log) {
            m_log(QStringLiteral("Sahara 唤醒 %1/3：切换命令模式并重置状态机").arg(attempt + 1));
        }

        // Some qcusbser sessions retain the target in command mode. Send both
        // parts of the kick back-to-back; waiting between them costs a full
        // serial timeout on BootROMs which react only to SWITCH_MODE.
        QString writeError;
        if (!sendHelloResponse(3, &writeError)) {
            receiveError = writeError;
            continue;
        }
        QThread::msleep(40);
        if (!sendSwitchMode(0, &writeError)) {
            receiveError = writeError;
            continue;
        }
        QThread::msleep(40);
        const int switchTimeout = attempt == 0 ? 1200 : 2200;
        if (readHelloPacket(packet, switchTimeout, &receiveError)) {
            return true;
        }

        if (!sendResetStateMachine(&writeError)) {
            receiveError = writeError;
            continue;
        }
        QThread::msleep(60);
        const int resetTimeout = attempt == 0 ? 1200 : 2500;
        if (readHelloPacket(packet, resetTimeout, &receiveError)) {
            return true;
        }
        QThread::msleep(attempt == 0 ? 80 : 180);
    }

    if (error) {
        *error = QStringLiteral("Sahara 状态机唤醒后仍未收到 Hello：%1").arg(receiveError);
    }
    return false;
}

bool SaharaClient::sendProgrammerRange(QFile& file, quint64 offset, quint64 length,
                                       quint64* transferred, QString* error)
{
    if (offset > static_cast<quint64>(file.size()) ||
        length > static_cast<quint64>(file.size()) - offset) {
        if (error) {
            *error = QStringLiteral("设备请求的 Programmer 范围越界：offset=%1, length=%2")
                         .arg(offset).arg(length);
        }
        return false;
    }
    if (!file.seek(static_cast<qint64>(offset))) {
        if (error) *error = QStringLiteral("定位 Programmer 失败：%1").arg(file.errorString());
        return false;
    }

    quint64 remaining = length;
    while (remaining > 0) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            if (error) *error = QStringLiteral("操作已停止");
            return false;
        }
        const qint64 request = static_cast<qint64>(qMin<quint64>(remaining, 1024 * 1024));
        const QByteArray block = file.read(request);
        if (block.size() != request) {
            if (error) *error = QStringLiteral("读取 Programmer 失败：%1").arg(file.errorString());
            return false;
        }
        if (!m_transport.writeAll(block, 10000, error)) return false;
        remaining -= static_cast<quint64>(block.size());
        *transferred += static_cast<quint64>(block.size());
        if (m_progress) m_progress(qMin(*transferred, static_cast<quint64>(file.size())), file.size());
    }
    return true;
}

bool SaharaClient::uploadProgrammer(const QString& filePath, DeviceInfo* deviceInfo,
                                    bool querySaharaIdentity, QString* error,
                                    quint32* endTransferStatus)
{
    if (endTransferStatus) *endTransferStatus = 0;
    QFile programmer(filePath);
    if (!programmer.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("打开 Programmer 失败：%1").arg(programmer.errorString());
        return false;
    }
    if (programmer.size() <= 0) {
        if (error) *error = QStringLiteral("Programmer 文件为空");
        return false;
    }

    if (m_log) m_log(QStringLiteral("等待 Sahara Hello 数据包…"));
    QByteArray packet;
    quint32 command = SaharaHello;
    if (!waitForHello(&packet, error)) return false;

    QByteArray imageHello = packet;
    if (querySaharaIdentity) {
        QString infoError;
        if (!readDeviceInfo(packet, deviceInfo, &imageHello, &infoError)) {
            if (m_log) {
                m_log(QStringLiteral("Sahara 设备信息读取未完整完成，恢复映像传输模式：%1")
                          .arg(infoError));
            }
            if (!waitForHello(&imageHello, error)) return false;
        }
    } else {
        const quint32 version = readU32(packet, 8);
        if (deviceInfo) deviceInfo->saharaVersion = QString::number(version);
        if (m_log) {
            m_log(QStringLiteral("Sahara 版本 %1，VIP 模式跳过命令态查询，直接传输 Programmer")
                      .arg(version));
        }
    }
    packet = imageHello;

    if (m_log && deviceInfo &&
        (!deviceInfo->serial.isEmpty() || !deviceInfo->hwid.isEmpty() ||
         !deviceInfo->msmId.isEmpty())) {
        const QString version = deviceInfo->saharaVersion.isEmpty()
            ? QStringLiteral("—") : deviceInfo->saharaVersion;
        const QString model = deviceInfo->model.isEmpty()
            ? QStringLiteral("Qualcomm") : deviceInfo->model;
        const QString brand = deviceInfo->brand.isEmpty()
            ? QStringLiteral("Qualcomm") : deviceInfo->brand;
        const QString identity = deviceInfo->hwid.isEmpty()
            ? deviceInfo->serial : deviceInfo->hwid;
        m_log(QStringLiteral("Sahara 设备信息：版本 %1，%2，%3，ID %4")
                  .arg(version, brand, model,
                       identity.isEmpty() ? QStringLiteral("—") : identity));
    } else if (m_log && deviceInfo && querySaharaIdentity) {
        m_log(QStringLiteral("Sahara 版本 %1，命令查询未返回身份字段，将从 Firehose 日志补全")
                  .arg(deviceInfo->saharaVersion.isEmpty()
                           ? QStringLiteral("—") : deviceInfo->saharaVersion));
    }

    if (m_log) {
        m_log(QStringLiteral("Sahara 版本 %1，发送 Hello Response")
                  .arg(readU32(packet, 8)));
    }
    if (!sendHelloResponse(0, error)) return false;

    quint64 transferred = 0;
    for (;;) {
        if (!readPacket(&packet, &command, 10000, error)) return false;
        if (command == SaharaReadData) {
            if (packet.size() < 20) {
                if (error) *error = QStringLiteral("Sahara READ_DATA 包过短");
                return false;
            }
            const quint64 offset = readU32(packet, 12);
            const quint64 length = readU32(packet, 16);
            if (!sendProgrammerRange(programmer, offset, length, &transferred, error)) return false;
            continue;
        }
        if (command == SaharaReadData64) {
            if (packet.size() < 32) {
                if (error) *error = QStringLiteral("Sahara READ_DATA_64 包过短");
                return false;
            }
            const quint64 offset = readU64(packet, 16);
            const quint64 length = readU64(packet, 24);
            if (!sendProgrammerRange(programmer, offset, length, &transferred, error)) return false;
            continue;
        }
        if (command == SaharaEndTransfer) {
            if (packet.size() < 16) {
                if (error) *error = QStringLiteral("Sahara END_TRANSFER 包过短");
                return false;
            }
            const quint32 status = readU32(packet, 12);
            if (status != 0) {
                if (endTransferStatus) *endTransferStatus = status;
                if (error) {
                    *error = QStringLiteral("Programmer 传输失败，Sahara 状态：%1 / 0x%2 %3")
                                 .arg(status).arg(status, 2, 16, QLatin1Char('0'))
                                 .arg(saharaStatusText(status));
                }
                return false;
            }

            QByteArray done;
            appendU32(done, SaharaDone);
            appendU32(done, 8);
            if (!m_transport.writeAll(done, 3000, error)) return false;

            if (!readPacket(&packet, &command, 5000, error)) return false;
            if (command != SaharaDoneResponse) {
                if (error) *error = QStringLiteral("预期 Sahara DONE_RESPONSE，收到 0x%1")
                                         .arg(command, 0, 16);
                return false;
            }
            if (m_progress) m_progress(programmer.size(), programmer.size());
            if (m_log) m_log(QStringLiteral("Programmer 传输完成"));
            return true;
        }

        if (error) *error = QStringLiteral("未处理的 Sahara 命令：0x%1").arg(command, 0, 16);
        return false;
    }
}

bool SaharaClient::finishPendingTransfer(QString* error)
{
    QByteArray packet;
    quint32 command = 0;
    if (!readPacket(&packet, &command, 2500, error)) return false;
    if (command != SaharaEndTransfer || packet.size() < 16) {
        if (error) *error = QStringLiteral("期待 Sahara END_TRANSFER，收到 0x%1")
                                .arg(command, 0, 16);
        return false;
    }
    const quint32 status = readU32(packet, 12);
    if (status != 0) {
        if (error) *error = QStringLiteral("Sahara END_TRANSFER 状态异常：%1").arg(status);
        return false;
    }

    QByteArray done;
    appendU32(done, SaharaDone);
    appendU32(done, 8);
    if (!m_transport.writeAll(done, 3000, error)) return false;
    if (!readPacket(&packet, &command, 5000, error)) return false;
    if (command != SaharaDoneResponse) {
        if (error) *error = QStringLiteral("期待 Sahara DONE_RESPONSE，收到 0x%1")
                                .arg(command, 0, 16);
        return false;
    }
    if (m_log) m_log(QStringLiteral("检测到已完成的 Programmer 传输，Sahara DONE 已确认"));
    return true;
}

} // namespace swt::edl
