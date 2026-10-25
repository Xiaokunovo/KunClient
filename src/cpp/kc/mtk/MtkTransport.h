#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkTransport — MTK 协议层的字节通道抽象
//
//  为什么抽这一层：BROM 的回显协议与 XFlash 的包头协议都是**纯字节流**语义，
//  把它们绑死在 QSerialPort 上就没法在没有真机的情况下验证 ——
//  有了这个接口，单测可以塞一个「假设备」（MtkMockTransport）逐字节应答，
//  从而把协议实现本身测穿（见 tests/MtkProtocolTest.cpp）。
//
//  真实实现 SerialTransport：Windows 下 MTK 口（BROM 与 DA）都经 usbser 呈现为
//  COM 口，所以两者都是「打开 COMx → 读写字节」。
//
//  🔴 QSerialPort 有线程亲和性：谁 open 谁用谁 close，不能跨 QThreadPool 线程复用。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QString>

namespace kc::mtk {

/// 字节通道。实现方负责 open/close 的可见状态；协议层只管读写。
class ITransport {
public:
    virtual ~ITransport() = default;

    /// 全量写出（内部自旋直到写完或出错）。失败返回 false。
    virtual bool write(const QByteArray& data) = 0;

    /// 尽力读满 bytes 字节；超时/设备消失时返回**已读到的部分**（可能为空）。
    /// 不抛异常、不在此层判错 —— 由协议层根据「读回长度是否够」决定重试还是失败。
    virtual QByteArray readExact(int bytes, int timeoutMs) = 0;

    virtual bool isOpen() const = 0;
    virtual QString lastError() const = 0;

    /// 单次写块上限（XFlash send_param 分块 / DA 上传分块用）。
    virtual int maxWritePacket() const { return 0x400; }

    /// 丢弃残留输入（握手重试前清缓冲，避免上一轮的垃圾字节被当成本轮回显）。
    virtual void clear() {}
};

/// QSerialPort 实现。波特率默认取 kBromBaud（BROM/DA 在 usbser 下都是这个口径）。
class SerialTransport : public ITransport {
public:
    explicit SerialTransport(const QString& portName, int baudRate = 0);
    ~SerialTransport() override;

    bool open();
    void close();

    bool write(const QByteArray& data) override;
    QByteArray readExact(int bytes, int timeoutMs) override;
    bool isOpen() const override;
    QString lastError() const override;
    int maxWritePacket() const override;
    void clear() override;

    QString portName() const { return m_portName; }
    int baudRate() const { return m_baudRate; }

private:
    struct Private; // 隐藏 QSerialPort，头文件不引 QtSerialPort
    Private* d = nullptr;

    QString m_portName;
    int m_baudRate = 0;
};

} // namespace kc::mtk
