#include "transport/SerialTransport.h"

#include <QElapsedTimer>
#include <QThread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace swt::edl {

SerialTransport::SerialTransport(std::atomic_bool* cancelled)
    : m_cancelled(cancelled)
{
}

SerialTransport::~SerialTransport()
{
    close();
}

bool SerialTransport::open(const QString& portName, QString* error)
{
    close();
    m_port.setPortName(portName);
    // Match the Qualcomm/.NET host configuration. qcusbser normally treats the baud
    // rate as virtual, but some Windows driver revisions do gate their USB receive
    // path on the complete 115200 8N1 + DTR configuration.
    m_port.setBaudRate(QSerialPort::Baud115200);
    m_port.setDataBits(QSerialPort::Data8);
    m_port.setParity(QSerialPort::NoParity);
    m_port.setStopBits(QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);
    m_port.setReadBufferSize(16 * 1024 * 1024);

    if (!m_port.open(QIODevice::ReadWrite)) {
        if (error) {
            *error = QStringLiteral("打开 %1 失败：%2").arg(portName, m_port.errorString());
        }
        return false;
    }

    bool driverQueueConfigured = false;
#ifdef Q_OS_WIN
    const HANDLE handle = reinterpret_cast<HANDLE>(m_port.handle());
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        // QSerialPort::setReadBufferSize limits Qt's userspace buffer. SetupComm also
        // grows qcusbser's kernel queues, matching the behavior of SerialPort on the
        // C# implementation and preventing a delayed HELLO from being back-pressured.
        driverQueueConfigured = ::SetupComm(handle, 4 * 1024 * 1024, 1024 * 1024) != FALSE;
        DWORD communicationErrors = 0;
        COMSTAT status{};
        ::ClearCommError(handle, &communicationErrors, &status);
    }
#endif

    // QSerialPort initializes NoFlowControl with both control lines deasserted.
    // Several qcusbser builds do not arm the bulk-IN endpoint until DTR is asserted.
    const bool dtrConfigured = m_port.setDataTerminalReady(true);
    if (!dtrConfigured) m_port.clearError();
    const bool rtsConfigured = m_port.setRequestToSend(false);
    if (!rtsConfigured) m_port.clearError();

    // Qualcomm targets may emit Sahara HELLO immediately when the handle opens.
    // Clearing the input queue here drops that one-shot 48-byte packet on some devices.
    m_buffer.clear();
    m_openDiagnostics = QStringLiteral("115200 8N1，DTR=%1，RTS=0，驱动接收队列=%2")
                            .arg(dtrConfigured ? QStringLiteral("1")
                                               : QStringLiteral("驱动未确认"),
                                 driverQueueConfigured ? QStringLiteral("4 MiB")
                                                       : QStringLiteral("系统默认"));
    return true;
}

void SerialTransport::close()
{
    if (m_port.isOpen()) {
        m_port.close();
    }
    m_buffer.clear();
    m_openDiagnostics.clear();
}

bool SerialTransport::isOpen() const
{
    return m_port.isOpen();
}

QString SerialTransport::portName() const
{
    return m_port.portName();
}

QString SerialTransport::openDiagnostics() const
{
    return m_openDiagnostics;
}

void SerialTransport::clear()
{
    m_buffer.clear();
    if (m_port.isOpen()) {
        m_port.clear(QSerialPort::AllDirections);
    }
}

bool SerialTransport::wasCancelled(QString* error) const
{
    if (m_cancelled && m_cancelled->load(std::memory_order_relaxed)) {
        if (error) {
            *error = QStringLiteral("操作已停止");
        }
        return true;
    }
    return false;
}

bool SerialTransport::writeAll(QByteArrayView data, int timeoutMs, QString* error)
{
    if (!m_port.isOpen()) {
        if (error) *error = QStringLiteral("串口尚未打开");
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    qsizetype written = 0;
    while (written < data.size()) {
        if (wasCancelled(error)) return false;
        const qint64 count = m_port.write(data.data() + written, data.size() - written);
        if (count < 0) {
            if (error) *error = QStringLiteral("串口写入失败：%1").arg(m_port.errorString());
            return false;
        }
        written += count;

        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !m_port.waitForBytesWritten(qMin(remaining, 250))) {
            if (m_port.bytesToWrite() > 0 && timer.elapsed() >= timeoutMs) {
                if (error) *error = QStringLiteral("串口写入超时（%1/%2 字节）")
                                         .arg(written).arg(data.size());
                return false;
            }
        }
    }
    while (m_port.bytesToWrite() > 0) {
        if (wasCancelled(error)) return false;
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0 || !m_port.waitForBytesWritten(qMin(remaining, 250))) {
            if (timer.elapsed() >= timeoutMs) {
                if (error) *error = QStringLiteral("等待串口发送完成超时");
                return false;
            }
        }
    }
    return true;
}

bool SerialTransport::fillBuffer(int timeoutMs, QString* error)
{
    if (wasCancelled(error)) return false;
    if (m_port.bytesAvailable() == 0 && !m_port.waitForReadyRead(qMin(timeoutMs, 250))) {
        if (m_port.error() != QSerialPort::NoError && m_port.error() != QSerialPort::TimeoutError) {
            if (error) *error = QStringLiteral("串口读取失败：%1").arg(m_port.errorString());
            return false;
        }
        return true;
    }
    m_buffer += m_port.readAll();
    while (m_port.waitForReadyRead(2)) {
        m_buffer += m_port.readAll();
    }
    return true;
}

bool SerialTransport::readExactly(qsizetype size, QByteArray* output, int timeoutMs, QString* error)
{
    if (!output || size < 0) {
        if (error) *error = QStringLiteral("读取参数错误");
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (m_buffer.size() < size) {
        if (timer.elapsed() >= timeoutMs) {
            if (error) *error = QStringLiteral("串口读取超时（%1/%2 字节）")
                                     .arg(m_buffer.size()).arg(size);
            return false;
        }
        if (!fillBuffer(timeoutMs - static_cast<int>(timer.elapsed()), error)) return false;
    }

    *output = m_buffer.first(size);
    m_buffer.remove(0, size);
    return true;
}

bool SerialTransport::peekExactly(qsizetype size, QByteArray* output, int timeoutMs, QString* error)
{
    if (!output || size < 0) {
        if (error) *error = QStringLiteral("预读参数错误");
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    while (m_buffer.size() < size) {
        if (timer.elapsed() >= timeoutMs) {
            if (error) *error = QStringLiteral("串口预读超时（%1/%2 字节）")
                                     .arg(m_buffer.size()).arg(size);
            return false;
        }
        if (!fillBuffer(timeoutMs - static_cast<int>(timer.elapsed()), error)) return false;
    }

    *output = m_buffer.first(size);
    return true;
}

bool SerialTransport::readUntil(QByteArrayView marker, QByteArray* output, qsizetype maximumSize,
                                int timeoutMs, QString* error)
{
    if (!output || marker.isEmpty()) {
        if (error) *error = QStringLiteral("帧读取参数错误");
        return false;
    }

    QElapsedTimer timer;
    timer.start();
    for (;;) {
        const qsizetype position = m_buffer.indexOf(marker);
        if (position >= 0) {
            const qsizetype frameSize = position + marker.size();
            *output = m_buffer.first(frameSize);
            m_buffer.remove(0, frameSize);
            // Keep every byte after the XML terminator. Firehose may coalesce the
            // ACK and the beginning of a RAW read into one USB transfer. GPT data
            // commonly starts with hundreds of zero bytes; treating those bytes as
            // XML padding truncates the RAW payload and makes the host wait forever
            // for a tail that the target already sent.
            return true;
        }
        if (m_buffer.size() > maximumSize) {
            if (error) *error = QStringLiteral("接收帧超过限制：%1 字节").arg(maximumSize);
            return false;
        }
        if (timer.elapsed() >= timeoutMs) {
            if (error) *error = QStringLiteral("等待协议帧超时");
            return false;
        }
        if (!fillBuffer(timeoutMs - static_cast<int>(timer.elapsed()), error)) return false;
    }
}

} // namespace swt::edl
