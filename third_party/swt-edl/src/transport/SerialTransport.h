#pragma once

#include <QByteArray>
#include <QSerialPort>
#include <QString>

#include <atomic>
#include <chrono>

namespace swt::edl {

class SerialTransport final {
public:
    explicit SerialTransport(std::atomic_bool* cancelled = nullptr);
    ~SerialTransport();

    bool open(const QString& portName, QString* error);
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString portName() const;
    [[nodiscard]] QString openDiagnostics() const;

    void clear();
    bool writeAll(QByteArrayView data, int timeoutMs, QString* error);
    bool peekExactly(qsizetype size, QByteArray* output, int timeoutMs, QString* error);
    bool readExactly(qsizetype size, QByteArray* output, int timeoutMs, QString* error);
    bool readUntil(QByteArrayView marker, QByteArray* output, qsizetype maximumSize,
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
