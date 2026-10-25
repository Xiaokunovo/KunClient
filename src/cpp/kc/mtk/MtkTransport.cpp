#include "kc/mtk/MtkTransport.h"

#include "kc/config/AppSettings.h"
#include "kc/mtk/MtkTypes.h"

#include <QDeadlineTimer>
#include <QSerialPort>
#include <QThread>

namespace kc::mtk {

struct SerialTransport::Private {
    QSerialPort* port = nullptr;
};

SerialTransport::SerialTransport(const QString& portName, int baudRate)
    : d(new Private)
    , m_portName(portName)
    , m_baudRate(baudRate > 0 ? baudRate : kc::AppSettings::current().engineMtkBaud)
{
}

SerialTransport::~SerialTransport()
{
    close();
    delete d;
    d = nullptr;
}

bool SerialTransport::open()
{
    close();

    d->port = new QSerialPort();
    d->port->setPortName(m_portName);
    // Qt 6.11 的 QSerialPort::setBaudRate 仍收 qint32（QBaudRate 类不在此版本）。
    d->port->setBaudRate(m_baudRate);
    d->port->setDataBits(QSerialPort::Data8);
    d->port->setParity(QSerialPort::NoParity);
    d->port->setStopBits(QSerialPort::OneStop);
    d->port->setFlowControl(QSerialPort::NoFlowControl);

    if (!d->port->open(QSerialPort::ReadWrite)) {
        delete d->port;
        d->port = nullptr;
        return false;
    }
    d->port->clear();
    return true;
}

void SerialTransport::close()
{
    if (d->port) {
        if (d->port->isOpen()) d->port->close();
        delete d->port;
        d->port = nullptr;
    }
}

bool SerialTransport::isOpen() const
{
    return d->port && d->port->isOpen();
}

QString SerialTransport::lastError() const
{
    return d->port ? d->port->errorString() : QStringLiteral("端口未打开");
}

int SerialTransport::maxWritePacket() const
{
    // 串口没有 EP 概念，取 mtkclient 在拿不到 EP_IN.wMaxPacketSize 时的回退值。
    return kBromFallbackMaxPacket;
}

void SerialTransport::clear()
{
    if (d->port && d->port->isOpen()) d->port->clear(QSerialPort::Input);
}

bool SerialTransport::write(const QByteArray& data)
{
    if (!d->port || !d->port->isOpen()) return false;
    if (data.isEmpty()) {
        // 空包在 DA 上传里是「flush 边界」，必须真的发一次 0 字节写（usbser 下即等一拍）。
        d->port->flush();
        return true;
    }

    qint64 written = 0;
    while (written < data.size()) {
        const qint64 n = d->port->write(data.constData() + written, data.size() - written);
        if (n < 0) return false;
        written += n;
        if (!d->port->waitForBytesWritten(2000) && d->port->error() != QSerialPort::TimeoutError) {
            return false;
        }
    }
    // 兼容模式：每个传输包之后留一拍。老 BROM / 劣质线材在连续满速写入时会丢包，
    // 症状是「进度卡在某个百分比然后校验失败」，加 3 ms 就能过（代价是整体慢一些）。
    const int delayMs = kc::AppSettings::current().mtkPacketDelayMs();
    if (delayMs > 0) QThread::msleep(static_cast<unsigned long>(delayMs));
    return d->port->flush();
}

QByteArray SerialTransport::readExact(int bytes, int timeoutMs)
{
    QByteArray out;
    if (!d->port || !d->port->isOpen() || bytes <= 0) return out;
    out.reserve(bytes);

    QDeadlineTimer deadline(timeoutMs);
    while (out.size() < bytes) {
        const qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) break;

        if (!d->port->waitForReadyRead(static_cast<int>(remaining))) {
            // waitForReadyRead 超时会置 TimeoutError —— 期限未到就继续等；
            // 设备被拔掉等真实错误直接中断（返回已读部分，由上层判定）。
            if (d->port->error() != QSerialPort::TimeoutError) break;
            continue;
        }
        out.append(d->port->read(bytes - out.size()));
    }
    return out;
}

} // namespace kc::mtk
