#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MtkXflash — DA2 的 XFlash 协议会话（DA 起来之后的全部存储操作）
//
//  包格式（来源：mtkclient DA/xflash/xflash_lib.py）：
//    每个包 = 12 字节包头 <III[magic=0xFEEEEEEF][datatype][length]> + payload；
//    命令字 u32 **小端**；状态用 status() 读回来（0 = OK，其余见错误码编码）。
//
//  典型时序：
//    jump_da → 读 1 字节 0xC0 → syncDa() → setupEnv() → setupHwInit()
//    → xread() 应等于 <I SYNC_SIGNAL> → getConnectionAgent()
//    →（若 agent == "brom"）sendEmi() → bootTo(DA2) → 之后即可读写。
//
//  🔴 本会话**不持有** transport 的所有权（归 BromSession），因为 JUMP_DA 之后
//     设备会以 PID 0x2001 重新枚举、需要换一个 COM 口 —— 换口由上层
//     （MtkDeviceSession）调 BromSession::replaceTransport 完成。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkTypes.h"

#include <QByteArray>
#include <QObject>
#include <QIODevice>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>

namespace kc::mtk {

class ITransport;

class XflashSession : public QObject {
    Q_OBJECT

public:
    using CancelFn = std::function<bool()>;
    using ProgressFn = std::function<void(qint64 done, qint64 total)>;

    /// 帧级错误的哨兵值（status() 返回它表示包头不对/读不满）。
    static constexpr qint64 kFrameError = -1;

    explicit XflashSession(ITransport* transport, QObject* parent = nullptr);

    void setCancelFn(CancelFn fn) { m_cancel = std::move(fn); }
    void setProgressFn(ProgressFn fn) { m_progress = std::move(fn); }
    ITransport* transport() const { return m_tp; }

    // ── 帧原语 ──────────────────────────────────────────────────────────
    /// 参考实现 xsend(data)：正文就是 data（命令即 payload，4 字节小端）。
    /// 不额外给 payload 时，正文 = leU32(cmd) —— **不是空负载**。
    bool xsend(XCmd cmd, XDataType type, const QByteArray& payload);
    bool xsend(XCmd cmd) { return xsend(cmd, XDataType::ProtocolFlow, QByteArray()); }

    /// 读一个数据包（自动吃掉 12 字节包头）。失败返回空 QByteArray。
    QByteArray xread(int timeoutMs = 5000, bool* ok = nullptr);

    /// 读一个状态包。返回 kFrameError 表示包头/长度不对；否则返回状态值（0 = OK）。
    qint64 status(int timeoutMs = 5000);

    /// send_param：每个 param 单独一个包头，数据按 0x200 分块写；末尾读一次状态。
    bool sendParam(const QVector<QByteArray>& params);
    bool sendParam(const QByteArray& param) { return sendParam(QVector<QByteArray>{ param }); }

    bool sendData(const QByteArray& data);
    bool ack();

    /// 直接发一个 PROTOCAL_FLOW 包（payload 任意长度；ack / SYNC / EMI 长度都用它）。
    bool sendFlowPacket(const QByteArray& payload, int timeoutMs);

    bool sendDevCtrl(XCmd cmd, const QByteArray& param = QByteArray(), QByteArray* out = nullptr);

    // ── DA2 初始化 ──────────────────────────────────────────────────────
    bool waitDaReady(QString* error);                 // 等设备发 0xC0
    bool syncDa(QString* error);                      // 发 SYNC_SIGNAL
    bool setupEnv(QString* error);
    bool setupHwInit(QString* error);
    bool handshakeStage2(QString* error);             // sync + setupEnv + setupHwInit + 读 SYNC

    // ── 信息查询 ────────────────────────────────────────────────────────
    bool getDaVersion(QString* version, QString* error);
    bool getConnectionAgent(QString* agent, QString* error);
    bool getPacketLength(uint32_t* writeLen, uint32_t* readLen, QString* error);
    bool setChecksumLevel(uint32_t level, QString* error);
    bool setResetKey(uint32_t key, QString* error);
    bool slaEnabledStatus(bool* enabled, QString* error);
    bool getChipId(uint16_t* hwCode, uint16_t* hwSubCode, uint16_t* hwVersion, QString* error);

    // ── 存储操作 ────────────────────────────────────────────────────────
    bool sendEmi(const QByteArray& emi, QString* error);
    bool bootTo(uint32_t addr, const QByteArray& data, int settleMs, QString* error);

    /// size 是源数据长度；末块流式补零到 512 字节，地址必须按扇区对齐。
    bool writeFromDevice(quint64 addr, quint64 size, const DaStorage& storage, uint32_t partType,
                         QIODevice* source, QString* error);
    /// 地址与长度均须按 512 字节对齐；目标短写视为失败。
    bool readToDevice(quint64 addr, quint64 size, const DaStorage& storage, uint32_t partType,
                      QIODevice* destination, QString* error);
    bool writeData(quint64 addr, const DaStorage& storage, uint32_t partType,
                   const QByteArray& data, QString* error);
    bool readData(quint64 addr, quint64 size, const DaStorage& storage, uint32_t partType,
                  QByteArray* out, QString* error);
    bool format(quint64 addr, quint64 size, const DaStorage& storage, uint32_t partType,
                QString* error);
    /// 存储传输失败后帧边界不再可信；必须销毁会话再连接。
    bool isUsable() const { return m_usable; }
    bool shutdown(QString* error);

    /// 供上层复用：写入前先问一次最优包长（失败时回退 1MB / 0x400）。
    uint32_t writePacketSize() const { return m_writePacket; }
    uint32_t readPacketSize() const { return m_readPacket; }
    void refreshPacketSize();

signals:
    void logLine(const QString& level, const QString& message);

private:
    bool cancelled() const { return m_cancel && m_cancel(); }
    void report(qint64 done, qint64 total);
    QString statusText(qint64 status) const;

    bool m_usable = true;
    ITransport* m_tp = nullptr;
    CancelFn m_cancel;
    ProgressFn m_progress;
    uint32_t m_writePacket = 0x400;
    uint32_t m_readPacket = kXflashMaxPacketFallback;
};

} // namespace kc::mtk
