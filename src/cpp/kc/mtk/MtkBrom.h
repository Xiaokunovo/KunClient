#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkBrom — BROM（Boot ROM）会话
//
//  线格式（**回显式**，来源：mtkclient Port.run_handshake / mtk_preloader.py）：
//    · 握手：host 逐字节发 A0 0A 50 05 → device 逐字节回 ~b & 0xFF；
//    · 每条命令：发命令字节 → 读回**同一字节**做确认（echo）；
//    · 多字节字段一律**大端**（">I" 地址/长度，">H" 状态）；
//    · 命令完成后再读 u16 大端状态：> 0xFF 表示错误码（0x1D0D = 需要 SLA）。
//
//  🔴 上一版（v1）把这里写成「魔数 0xE0/0x5A + [cmd][len:4 LE][payload] 帧」，
//     与真机协议完全不符 —— 那段代码一行都跑不通。v2 起以本文件为唯一口径。
//
//  用途：识别芯片（GET_HW_CODE）→ 上传并跳转 DA（SEND_DA/JUMP_DA）→
//        此后一切存储读写都交给 DA（见 MtkXflash）。
//        BROM 自带的 READ32/WRITE32 是**内存**读写，不作用于 eMMC/UFS 分区。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkTypes.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>

namespace kc::mtk {

class ITransport;

class BromSession : public QObject {
    Q_OBJECT

public:
    /// 取消回调：返回 true 表示上层要求中止（长循环里轮询）。
    using CancelFn = std::function<bool()>;

    /// transport 所有权语义。
    ///   · Own    —— 接管所有权，析构时 delete（生产路径一律用这个）；
    ///   · Borrow —— 只借用，析构时**不** delete（测试里的假设备是栈对象；
    ///               也适用于上层想跨会话复用同一传输层）。
    enum class Ownership { Own, Borrow };

    /// 接管 transport 所有权（析构时 delete）。
    explicit BromSession(ITransport* transport, QObject* parent = nullptr);

    /// 显式指定所有权（见 Ownership 说明）。
    BromSession(ITransport* transport, Ownership ownership, QObject* parent = nullptr);

    ~BromSession() override;

    /// 便捷构造：SerialTransport(comPort) + open()。失败返回 nullptr 并写 *error。
    static BromSession* openSerial(const QString& comPort, QString* error);

    ITransport* transport() const { return m_tp; }
    bool isOpen() const;

    void setCancelFn(CancelFn fn) { m_cancel = std::move(fn); }
    bool cancelled() const { return m_cancel && m_cancel(); }

    /// 握手（回显式）。设备上电异步，内部多轮重试，总时长约 5s。
    bool handshake(QString* error);

    /// GET_HW_CODE：UsbCmd(0xFD, 4) → 前 2 字节大端 = hw code。
    bool getHwCode(uint16_t* hwCode, QString* error);

    /// READ32：读 dwords 个 32 位字（**内存**地址，不是存储偏移）。
    bool read32(uint32_t addr, int dwords, QVector<uint32_t>* out, QString* error);

    /// WRITE32：写 32 位字序列到**内存**地址。
    bool write32(uint32_t addr, const QVector<uint32_t>& values, QString* error);

    /// SEND_DA：把 DA1 镜像上传到 addr（含签名，共 data.size() 字节）。
    ///   sigLen = 镜像尾部签名字节数（0 表示无签名）。
    bool sendDa(uint32_t addr, const QByteArray& data, uint32_t sigLen, QString* error);

    /// JUMP_DA（0xD5，32 位 DA）。
    bool jumpDa(uint32_t addr, QString* error);

    /// JUMP_DA64（0xDE，64 位 DA）。
    bool jumpDa64(uint32_t addr, QString* error);

    /// upper layer 用：SEND_DA 之后设备可能已经切到 DA 口，需要重开传输。
    /// （Windows/usbser 下 DA 会以 PID 0x2001 重新枚举 → COM 口号变化。）
    /// 🔴 传入的 transport **一律转为 Own**（生产路径全是 new 出来的）；
    ///    借用来的旧 transport 不会被 delete，避免误删外部对象。
    void replaceTransport(ITransport* transport);

    /// prepare_data 的校验和：每 2 字节按**小端** u16 异或（mtkclient prepare_data）。
    static uint16_t daChecksum(const QByteArray& data);

signals:
    /// 协议过程透传（level: info/success/warning/error）。跨线程自动排队。
    void logLine(const QString& level, const QString& message);

private:
    // ── 线原语 ──────────────────────────────────────────────────────────
    bool echo(const QByteArray& data);
    bool echoU32(uint32_t value);
    bool readWord(uint16_t* out, int timeoutMs);
    bool readDwords(int count, QVector<uint32_t>* out, int timeoutMs);
    bool uploadData(const QByteArray& data);

    /// JUMP_DA / JUMP_DA64 的公共流程（两者只差一个 64 位标志字节）。
    bool jumpDaInternal(uint32_t addr, QString* error, bool is64);

    ITransport* m_tp = nullptr;
    bool m_owns = true;
    CancelFn m_cancel;
};

} // namespace kc::mtk
