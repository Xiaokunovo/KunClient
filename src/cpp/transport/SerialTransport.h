#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QSerialPort>
#include <QString>

#include <atomic>
#include <chrono>

namespace swt::edl {

// 虚拟 I/O 边界允许协议回归测试接假设备，不需要打开真实串口。
class SerialTransport {
public:
    explicit SerialTransport(std::atomic_bool* cancelled = nullptr);
    virtual ~SerialTransport();

    bool open(const QString& portName, QString* error);
    void close();
    [[nodiscard]] virtual bool isOpen() const;
    [[nodiscard]] QString portName() const;
    [[nodiscard]] QString openDiagnostics() const;

    void clear();
    virtual bool writeAll(QByteArrayView data, int timeoutMs, QString* error);
    bool peekExactly(qsizetype size, QByteArray* output, int timeoutMs, QString* error);
    virtual bool readExactly(qsizetype size, QByteArray* output, int timeoutMs, QString* error);
    virtual bool readUntil(QByteArrayView marker, QByteArray* output, qsizetype maximumSize,
                   int timeoutMs, QString* error);

protected:
    /// 把端口里已到的数据搬进 m_buffer，最多攒到 budget 字节就停手。
    /// 不做「一直读到断流」的贪心循环：设备持续高速输出时那样攒的缓冲没有
    /// 上界，进度回调也永远轮不上。
    /// 假设备测试覆盖这一个函数，就能让 readExactly/readUntil 的真实缓冲
    /// 逻辑（读游标、压缩、帧切分）在内存里被跑到，无需打开串口。
    virtual bool fillBuffer(int timeoutMs, qsizetype budget, QString* error);

    /// 已收下但还没交付给调用方的字节数。
    [[nodiscard]] qsizetype buffered() const { return m_buffer.size() - m_readOffset; }
    /// 待交付区间的起始指针。
    [[nodiscard]] const char* bufferedData() const { return m_buffer.constData() + m_readOffset; }

    QByteArray m_buffer;
    qsizetype m_readOffset = 0;

private:
    /// 丢弃已交付的前缀。用读游标而不是每次 remove(0,n)：RAW 传输每交付一片
    /// 都搬一次剩余缓冲是 O(n²)，4 GB 镜像下内存带宽会直接吃掉吞吐。
    void compactBuffer();
    bool wasCancelled(QString* error) const;

    QSerialPort m_port;
    QString m_openDiagnostics;
    std::atomic_bool* m_cancelled = nullptr;
};

} // namespace swt::edl
