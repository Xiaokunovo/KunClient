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

namespace {

/// 单次 fillBuffer 最多攒多少字节。超出就交回上层交付，让调用方拿到数据、
/// 更新进度、检查取消，再回来继续读。4 MiB 足以让一次 waitForReadyRead
/// 捞到整条 USB 批量管道的内容，又不至于把一次 RAW 传输拖成不可中断的长读。
constexpr qsizetype kFillBudget = 4 * 1024 * 1024;

/// 每次交付后允许残留在缓冲里的最大已交付前缀。低于这个量级不值得搬。
constexpr qsizetype kCompactThreshold = 64 * 1024;

/// 阻塞等待的单片时长。只决定「取消操作最晚多久被感知」，不再人为限制
/// 一次等待能覆盖的总时长（原来固定 250ms 会让每条命令的 ACK 多绕几轮）。
constexpr int kWaitSlice = 50;

} // namespace

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
        // 发送队列与接收队列取同量级：分片提到 8 MB 后，1 MB 的发送队列装不下
        // 一片，writeAll 会被拆成多次「写一点→等驱动」的往返，分片变大带来的
        // 收益会被这部分抵消掉。
        driverQueueConfigured = ::SetupComm(handle, 4 * 1024 * 1024, 4 * 1024 * 1024) != FALSE;
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
    m_readOffset = 0;
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
    m_readOffset = 0;
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
    m_readOffset = 0;
    if (m_port.isOpen()) {
        m_port.clear(QSerialPort::AllDirections);
    }
}

void SerialTransport::compactBuffer()
{
    if (m_readOffset == 0) return;
    if (m_readOffset >= m_buffer.size()) {
        m_buffer.clear();
        m_readOffset = 0;
        return;
    }
    if (m_readOffset < kCompactThreshold) return;
    m_buffer.remove(0, m_readOffset);
    m_readOffset = 0;
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
    if (data.isEmpty()) return true;

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
        // 驱动队列足够大时 write() 会把整片直接收下，此时再 waitForBytesWritten
        // 只是白等一个 USB 帧的时间。只有真的没写完才进等待。
        if (written >= data.size()) break;
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            if (error) *error = QStringLiteral("串口写入超时（%1/%2 字节）")
                                     .arg(written).arg(data.size());
            return false;
        }
        m_port.waitForBytesWritten(qMin(remaining, 250));
    }
    // 收尾排空：bytesToWrite() 归零才代表整片已交给驱动。没排空就返回，
    // 后续命令等不到 ACK，这一步不能省；但只有真有残留时才进等待，
    // 正常分片（内核队列装得下）在这里是零成本直通。
    while (m_port.bytesToWrite() > 0) {
        if (wasCancelled(error)) return false;
        const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) {
            if (error) *error = QStringLiteral("等待串口发送完成超时");
            return false;
        }
        // 单片等待压到 kWaitSlice：这条命令的 ACK 还在读侧等着，排空只是为了让
        // 数据真正离开主机。用 1s 长片会让每条小命令白等一个整片。
        if (m_port.waitForBytesWritten(qMin(remaining, kWaitSlice))) continue;
        if (m_port.error() != QSerialPort::NoError &&
            m_port.error() != QSerialPort::TimeoutError) {
            if (error) *error = QStringLiteral("串口写入失败：%1").arg(m_port.errorString());
            return false;
        }
    }
    return true;
}

bool SerialTransport::fillBuffer(int timeoutMs, qsizetype budget, QString* error)
{
    if (wasCancelled(error)) return false;
    compactBuffer();
    // 等待首字节：单次等到超时为止，但按 kWaitSlice 分片循环，好让取消操作
    // 仍然能在百毫秒级被感知到。原来的 250ms 固定切片既限制了取消响应速度，
    // 又让每条命令的 ACK 多绕几轮外层循环 —— 刷写过程有大量小命令
    // （getstorageinfo / patch / erase），这部分开销会累加成可观的固定延迟。
    QElapsedTimer waitClock;
    waitClock.start();
    while (m_port.bytesAvailable() == 0) {
        if (wasCancelled(error)) return false;
        const int left = timeoutMs - static_cast<int>(waitClock.elapsed());
        if (left <= 0) break;
        if (m_port.waitForReadyRead(qMin(left, kWaitSlice))) break;
        if (m_port.error() != QSerialPort::NoError && m_port.error() != QSerialPort::TimeoutError) {
            if (error) *error = QStringLiteral("串口读取失败：%1").arg(m_port.errorString());
            return false;
        }
    }
    if (m_port.bytesAvailable() == 0) return true;
    m_buffer += m_port.readAll();
    // 攒够一个预算就回去交付，别在这里把设备还在吐的数据无限吞下去。
    if (buffered() >= budget) return true;
    // 驱动侧还有货就继续取：这一步不等待（bytesAvailable 已经是就绪量），
    // 只是把一次唤醒能拿到的数据一次取干净，避免每个 USB 帧都往返一次循环。
    while (buffered() < budget && m_port.bytesAvailable() > 0) {
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
    if (size == 0) {
        output->clear();
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    while (buffered() < size) {
        if (timer.elapsed() >= timeoutMs) {
            if (error) *error = QStringLiteral("串口读取超时（%1/%2 字节）")
                                     .arg(buffered()).arg(size);
            return false;
        }
        if (!fillBuffer(timeoutMs - static_cast<int>(timer.elapsed()), size, error)) return false;
    }

    *output = QByteArray(bufferedData(), size);
    m_readOffset += size;
    compactBuffer();
    return true;
}

bool SerialTransport::peekExactly(qsizetype size, QByteArray* output, int timeoutMs, QString* error)
{
    if (!output || size < 0) {
        if (error) *error = QStringLiteral("预读参数错误");
        return false;
    }
    if (size == 0) {
        output->clear();
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    while (buffered() < size) {
        if (timer.elapsed() >= timeoutMs) {
            if (error) *error = QStringLiteral("串口预读超时（%1/%2 字节）")
                                     .arg(buffered()).arg(size);
            return false;
        }
        if (!fillBuffer(timeoutMs - static_cast<int>(timer.elapsed()), size, error)) return false;
    }

    *output = QByteArray(bufferedData(), size);
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
    // 增量扫描：每轮从上次确认「不含终结符」的位置继续找，而不是把整个缓冲
    // 重扫一遍。Firehose 一条命令可能分好几帧到达（现代 OPPO programmer 会先
    // 吐几十条 <log>），全量重扫会把这些帧的解析成本放大成平方级。
    qsizetype scanned = 0;
    for (;;) {
        const qsizetype position =
            QByteArray::fromRawData(bufferedData(), buffered()).indexOf(marker, scanned);
        if (position < 0) {
            // 终结符可能被这次读取截断在边界上，回退 marker.size()-1 字节重扫，
            // 否则跨帧边界的 "</data>" 会永远拼不出来。
            // 压缩只在交付成功后发生，而交付后立即 return，所以 scanned 这个
            // 相对偏移在循环内始终有效，不需要额外处理缓冲起点前移。
            scanned = qMax<qsizetype>(0, buffered() - (marker.size() - 1));
        } else {
            const qsizetype frameSize = position + marker.size();
            *output = QByteArray(bufferedData(), frameSize);
            m_readOffset += frameSize;
            compactBuffer();
            // Keep every byte after the XML terminator. Firehose may coalesce the
            // ACK and the beginning of a RAW read into one USB transfer. GPT data
            // commonly starts with hundreds of zero bytes; treating those bytes as
            // XML padding truncates the RAW payload and makes the host wait forever
            // for a tail that the target already sent.
            return true;
        }
        if (buffered() > maximumSize) {
            if (error) *error = QStringLiteral("接收帧超过限制：%1 字节").arg(maximumSize);
            return false;
        }
        if (timer.elapsed() >= timeoutMs) {
            if (error) *error = QStringLiteral("等待协议帧超时");
            return false;
        }
        if (!fillBuffer(timeoutMs - static_cast<int>(timer.elapsed()), kFillBudget, error)) {
            return false;
        }
    }
}

} // namespace swt::edl
