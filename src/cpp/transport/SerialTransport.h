#pragma once

#include <QByteArray>
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

private:
    bool fillBuffer(int timeoutMs, QString* error);
    bool wasCancelled(QString* error) const;

    QSerialPort m_port;
    QByteArray m_buffer;
    QString m_openDiagnostics;
    std::atomic_bool* m_cancelled = nullptr;
};

} // namespace swt::edl
