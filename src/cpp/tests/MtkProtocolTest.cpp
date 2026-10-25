// ─────────────────────────────────────────────────────────────────────────────
//  MtkProtocolTest — MTK 协议层单测（BROM 回显 / DA 文件 / XFlash 包格式）
//
//  本机没有 MTK 真机，所以这里接一个**假设备**（MockMtkDevice）：它按真协议
//  逐字节应答（回显取反握手、大端字段、XFlash 12 字节包头、错误码…），
//  从而把「线格式实现」测穿 —— 这比"编译通过"强得多，也是能在这个环境下
//  拿到的最高强度验证。真机联调时若失败，先看这里哪条假设不成立。
//
//  覆盖：
//   ① 握手：正确回显 → 成功；错误回显 → 失败；
//   ② GET_HW_CODE（0xFD，大端两个 u16）；
//   ③ BROM READ32/WRITE32（0xD1/0xD4 回显式）；
//   ④ SEND_DA：长度宣告 + 分块上传 + 校验和回读；SLA(0x1D0D) 要报明确错误；
//   ⑤ JUMP_DA（0xD5）+ DA 就绪字节 0xC0；
//   ⑥ DA 文件解析（合成 MTK_AllInOne_DA.bin）+ 按 hw code 选条目 + 取 region；
//   ⑦ XFlash：SYNC / SETUP_ENV / SETUP_HW_INIT / GET_CONNECTION_AGENT /
//      GET_PACKET_LENGTH / WRITE_DATA / READ_DATA / FORMAT / BOOT_TO；
//   ⑧ scatter region → (DaStorage, parttype) 映射。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/mtk/MtkBrom.h"
#include "kc/mtk/MtkTransport.h"
#include "kc/mtk/MtkDaFile.h"
#include "kc/mtk/MtkTypes.h"
#include "kc/mtk/MtkXflash.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QtEndian>

#include <cstdio>
#include <functional>
#include <limits>

namespace {

int g_pass = 0;
int g_fail = 0;

/// 假设备（MockMtkDevice）是**栈对象** → 会话必须只"借用"它。
/// 用默认的 Ownership::Own 会让 ~BromSession() 去 delete 一个栈指针 → 直接崩。
constexpr auto kBorrow = kc::mtk::BromSession::Ownership::Borrow;

void check(bool ok, const QString& what)
{
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        // 注意：不能用 qPrintable()（= toLocal8Bit，中文 Windows 下转 GBK，
        // 而这里控制台是 UTF-8 → 全变乱码）。统一走 toUtf8()。
        std::printf("  [FAIL] %s\n", what.toUtf8().constData());
    }
}

void checkEq(qint64 got, qint64 want, const QString& what)
{
    check(got == want, QStringLiteral("%1（期望 %2 / 实得 %3）").arg(what).arg(want).arg(got));
}

/// 先执行、再用**它自己**写出的 error 组消息。
/// 🔴 不能写成 `check(fn(&error), QStringLiteral("…（%1）").arg(error))`：
///    实参在调用 check 之前就求值完了，读到的永远是上一次的 error（通常为空），
///    于是真正的失败原因被吃掉 —— 本文件就因此把 root cause 藏了两轮。
template <typename Fn>
void checkCall(Fn&& fn, const QString& what, const QString& error)
{
    if (fn()) {
        ++g_pass;
        return;
    }
    ++g_fail;
    std::printf("  [FAIL] %s%s%s\n", what.toUtf8().constData(),
                error.isEmpty() ? "" : "：", error.toUtf8().constData());
}

QByteArray be32(uint32_t v)
{
    QByteArray b(4, '\0');
    qToBigEndian<uint32_t>(v, b.data());
    return b;
}

QByteArray le32(uint32_t v)
{
    QByteArray b(4, '\0');
    qToLittleEndian<uint32_t>(v, b.data());
    return b;
}

uint32_t rdLe32(const char* p) { return qFromLittleEndian<uint32_t>(p); }

// ═════════════════════════════════════════════════════════════════════════════
//  假 MTK 设备
// ═════════════════════════════════════════════════════════════════════════════
class MockMtkDevice : public kc::mtk::ITransport {
public:
    // ── 可配置行为 ──────────────────────────────────────────────────────
    uint16_t hwCode = 0x6768;
    uint16_t hwVersion = 0x8A00;
    bool handshakeEchoBad = false;
    bool slaRequired = false;
    QString agent = QStringLiteral("brom");
    bool emiOk = true;
    bool da2BootOk = true;
    uint32_t writePacketLen = 0x10000;
    uint32_t readPacketLen = 0x10000;
    bool formatOk = true;

    // ── 观测 ────────────────────────────────────────────────────────────
    QHash<uint32_t, QByteArray> memory;     // BROM READ32/WRITE32 的内存模型
    QByteArray daReceived;                  // 收到的 DA1 数据
    QByteArray emiReceived;
    QByteArray da2Received;
    QHash<uint64_t, QByteArray> flash;      // DA 写入的"存储"，不截断高地址
    QHash<uint64_t, QString> flashTarget;   // addr → "storage/parttype"
    QVector<QByteArray> storageParams;     // 原始线格式，供独立的固定字节断言
    QVector<QByteArray> packets;
    int slaStatusReplies = 0;
    int formatCalls = 0;
    int m_badChecksums = 0;

    // ── ITransport ──────────────────────────────────────────────────────
    bool write(const QByteArray& data) override
    {
        if (data.isEmpty()) return true; // flush 空写
        m_in += data;
        pump();
        return true;
    }

    QByteArray readExact(int bytes, int timeoutMs) override
    {
        Q_UNUSED(timeoutMs)
        if (bytes <= 0) return QByteArray();
        if (m_out.size() < bytes) {
            // 设备侧没准备好这么多数据 → 交还已有的（协议层会据长度判失败，
            // 从而把"脚本与实现不匹配"暴露出来，而不是悄悄挂住）。
            const QByteArray partial = m_out;
            m_out.clear();
            return partial;
        }
        const QByteArray out = m_out.left(bytes);
        m_out.remove(0, bytes);
        return out;
    }

    bool isOpen() const override { return true; }
    QString lastError() const override { return QString(); }
    int maxWritePacket() const override { return 0x400; }

private:
    QByteArray m_in;
    QByteArray m_out;

    // ── BROM 状态机 ─────────────────────────────────────────────────────
    enum class St {
        Handshake,       // 逐字节回显取反
        Cmd,             // 等命令字节
        HwCodeData,      // 等 4 字节 GET_HW_CODE 应答
        SendDaAddr,
        SendDaLen,
        SendDaSig,
        SendDaData,
        JumpDaAddr,
        Read32Addr,
        Read32Count,
        Read32Data,
        Write32Addr,
        Write32Count,
        Write32Data,
        DaMode,          // 已跳转 DA，改走 12 字节包头
    };
    St m_st = St::Handshake;
    int m_hsIndex = 0;
    uint32_t m_len = 0;      // SEND_DA 宣告的长度
    uint32_t m_count = 0;    // READ32/WRITE32 字数
    uint32_t m_addr = 0;
    int m_dataSeen = 0;
    QVector<uint32_t> m_pendingValues;

    // ── DA(XFlash) 状态 ─────────────────────────────────────────────────
    //  参考实现的"期望脚本"：每个命令之后跟什么包、每个包要不要回 status，
    //  都是固定的。假设备照这张表走，才能与真机行为同构。
    enum class Pkt { None, ParamOfCmd, BootParam, BootData, AfterGetData };
    Pkt m_expect = Pkt::None;
    uint32_t m_curCmd = 0;
    int m_paramIndex = 0;
    uint64_t m_curDataAddr = 0;
    quint64 m_curDataSize = 0;
    QByteArray m_curData;
    uint16_t m_curChunkChecksum = 0;
    int m_expectedAcks = 0;
    // send_emi 的两段式状态
    uint32_t m_emiLen = 0;
    bool m_emiLenSeen = false;
    QByteArray m_emiBuf;

    void emitBytes(const QByteArray& data) { m_out += data; }

    void emitStatus(uint32_t value)
    {
        QByteArray h(12, '\0');
        qToLittleEndian<uint32_t>(kc::mtk::kXflashMagic, h.data() + 0);
        qToLittleEndian<uint32_t>(1, h.data() + 4);
        qToLittleEndian<uint32_t>(4, h.data() + 8);
        emitBytes(h);
        emitBytes(le32(value));
    }

    void emitDataPacket(const QByteArray& payload)
    {
        QByteArray h(12, '\0');
        qToLittleEndian<uint32_t>(kc::mtk::kXflashMagic, h.data() + 0);
        qToLittleEndian<uint32_t>(1, h.data() + 4);
        qToLittleEndian<uint32_t>(static_cast<uint32_t>(payload.size()), h.data() + 8);
        emitBytes(h);
        emitBytes(payload);
    }

    // ─────────────────────────────────────────────────────────────────────
    void pump()
    {
        bool progress = true;
        while (progress) {
            progress = false;
            progress = m_st == St::DaMode ? pumpDa() : pumpBrom();
        }
    }

    bool pumpBrom()
    {
        switch (m_st) {
        case St::Handshake: {
            if (m_in.isEmpty()) return false;
            const uint8_t byte = static_cast<uint8_t>(m_in.at(0));
            const uint8_t want = kc::mtk::kBromHandshakeBytes[m_hsIndex];
            if (byte == want) {
                // 🔴 正确回显 = ~want；"坏回显"必须真的不等于它。
                //    （曾经写成 want ^ 0xFF —— 那和 ~want & 0xFF 是同一个值，
                //      于是"坏设备"表现得和好设备一模一样，握手照样成功。）
                emitBytes(QByteArray(1, static_cast<char>(
                                        handshakeEchoBad ? want
                                                         : static_cast<uint8_t>(~want & 0xFF))));
                ++m_hsIndex;
                m_in.remove(0, 1);
                if (m_hsIndex >= kc::mtk::kBromHandshakeLen) m_st = St::Cmd;
                return true;
            }
            // 不对齐 → 从第一个字节重来（真机行为）
            m_hsIndex = 0;
            m_in.remove(0, 1);
            return true;
        }
        case St::Cmd: {
            if (m_in.isEmpty()) return false;
            const uint8_t cmd = static_cast<uint8_t>(m_in.at(0));
            m_in.remove(0, 1);
            emitBytes(QByteArray(1, static_cast<char>(cmd))); // 回显命令字节
            switch (cmd) {
            case kc::mtk::kCmdGetHwCode: m_st = St::HwCodeData; break;
            case kc::mtk::kCmdSendDa:    m_st = St::SendDaAddr; break;
            case kc::mtk::kCmdJumpDa:    m_st = St::JumpDaAddr; break;
            case kc::mtk::kCmdRead32:    m_st = St::Read32Addr; break;
            case kc::mtk::kCmdWrite32:   m_st = St::Write32Addr; break;
            default:                     m_st = St::Cmd; break;
            }
            return true;
        }
        case St::HwCodeData: {
            // 真机在回显 0xFD 之后**直接**给 4 字节（rdword = 4 字节大端，无长度前缀），
            // 所以这里不能等 host 再发东西，否则永远不推进。
            QByteArray payload(4, '\0');
            qToBigEndian<uint16_t>(hwCode, payload.data());
            qToBigEndian<uint16_t>(hwVersion, payload.data() + 2);
            emitBytes(payload);
            m_st = St::Cmd;
            return true;
        }
        case St::SendDaAddr: {
            if (m_in.size() < 4) return false;
            m_addr = qFromBigEndian<uint32_t>(m_in.constData());
            emitBytes(be32(m_addr)); // 回显
            m_in.remove(0, 4);
            m_st = St::SendDaLen;
            return true;
        }
        case St::SendDaLen: {
            if (m_in.size() < 4) return false;
            m_len = qFromBigEndian<uint32_t>(m_in.constData());
            emitBytes(be32(m_len));
            m_in.remove(0, 4);
            m_st = St::SendDaSig;
            return true;
        }
        case St::SendDaSig: {
            if (m_in.size() < 4) return false;
            const uint32_t sigLen = qFromBigEndian<uint32_t>(m_in.constData());
            emitBytes(be32(sigLen));
            m_in.remove(0, 4);
            if (slaRequired) {
                QByteArray s(2, '\0');
                qToBigEndian<uint16_t>(kc::mtk::kStatusSlaRequired, s.data());
                emitBytes(s);
                m_st = St::Cmd;
            } else {
                QByteArray s(2, '\0'); // status = 0
                emitBytes(s);
                m_dataSeen = 0;
                daReceived.clear();
                m_st = St::SendDaData;
            }
            return true;
        }
        case St::SendDaData: {
            const int want = static_cast<int>(m_len) - m_dataSeen;
            if (want <= 0) return false;
            const int take = qMin(want, m_in.size());
            if (take <= 0) return false;
            daReceived.append(m_in.constData(), take);
            m_dataSeen += take;
            m_in.remove(0, take);
            if (m_dataSeen >= static_cast<int>(m_len)) {
                const uint16_t chk = kc::mtk::BromSession::daChecksum(daReceived);
                QByteArray tail(4, '\0');
                qToBigEndian<uint16_t>(chk, tail.data());
                qToBigEndian<uint16_t>(0, tail.data() + 2); // status
                emitBytes(tail);
                m_st = St::Cmd;
            }
            return true;
        }
        case St::JumpDaAddr: {
            if (m_in.size() < 4) return false;
            const uint32_t addr = qFromBigEndian<uint32_t>(m_in.constData());
            m_in.remove(0, 4);
            emitBytes(be32(addr));                      // rdword 必须回显 addr
            QByteArray s(2, '\0');                 // rword status = 0
            emitBytes(s);
            emitBytes(QByteArray(1, static_cast<char>(kc::mtk::kDaReadyByte))); // DA 起来后的 0xC0
            m_st = St::DaMode;
            return true;
        }
        case St::Read32Addr: {
            if (m_in.size() < 4) return false;
            m_addr = qFromBigEndian<uint32_t>(m_in.constData());
            emitBytes(be32(m_addr));
            m_in.remove(0, 4);
            m_st = St::Read32Count;
            return true;
        }
        case St::Read32Count: {
            if (m_in.size() < 4) return false;
            m_count = qFromBigEndian<uint32_t>(m_in.constData());
            emitBytes(be32(m_count));
            m_in.remove(0, 4);
            QByteArray s(2, '\0');
            emitBytes(s); // status <= 0xFF
            const QByteArray mem = memory.value(m_addr);
            for (uint32_t i = 0; i < m_count; ++i) {
                const QByteArray word = mem.mid(static_cast<int>(i) * 4, 4);
                emitBytes(word.size() == 4 ? word : QByteArray(4, '\0'));
            }
            emitBytes(QByteArray(2, '\0')); // tail status
            m_st = St::Cmd;
            return true;
        }
        case St::Write32Addr: {
            if (m_in.size() < 4) return false;
            m_addr = qFromBigEndian<uint32_t>(m_in.constData());
            emitBytes(be32(m_addr));
            m_in.remove(0, 4);
            m_st = St::Write32Count;
            return true;
        }
        case St::Write32Count: {
            if (m_in.size() < 4) return false;
            m_count = qFromBigEndian<uint32_t>(m_in.constData());
            emitBytes(be32(m_count));
            m_in.remove(0, 4);
            QByteArray s(2, '\0');
            emitBytes(s); // status <= 3 → host 继续发数据
            m_pendingValues.clear();
            m_st = St::Write32Data;
            return true;
        }
        case St::Write32Data: {
            if (static_cast<uint32_t>(m_pendingValues.size()) < m_count) {
                if (m_in.size() < 4) return false;
                const uint32_t value = qFromBigEndian<uint32_t>(m_in.constData());
                emitBytes(be32(value)); // 逐字回显
                m_in.remove(0, 4);
                m_pendingValues.append(value);
                if (static_cast<uint32_t>(m_pendingValues.size()) < m_count) return true;
            }
            // 🔴 线上是**大端**：参考实现 write 走 echo(pack(">I", val))，
            //    read 走 rdword()（unpack(">I"…)）。内存模型必须存线上字节，
            //    曾经存成小端 → 读回来全字节反序 → "读写往返一致" 失败。
            QByteArray mem;
            for (uint32_t v : m_pendingValues) {
                mem += be32(v);
            }
            memory.insert(m_addr, mem);
            emitBytes(QByteArray(2, '\0')); // tail status
            m_st = St::Cmd;
            return true;
        }
        case St::DaMode:
            return false;
        }
        return false;
    }

    // ── DA（XFlash）包处理 ──────────────────────────────────────────────
    static bool isKnownCmd(uint32_t v)
    {
        using X = kc::mtk::XCmd;
        switch (v) {
        case static_cast<uint32_t>(X::DeviceCtrl):
        case static_cast<uint32_t>(X::BootTo):
        case static_cast<uint32_t>(X::WriteData):
        case static_cast<uint32_t>(X::ReadData):
        case static_cast<uint32_t>(X::Format):
        case static_cast<uint32_t>(X::SetupEnvironment):
        case static_cast<uint32_t>(X::SetupHwInitParams):
        case static_cast<uint32_t>(X::InitExtRam):
        case static_cast<uint32_t>(X::Shutdown):
            return true;
        default:
            break;
        }
        // 0x02xxxx / 0x04xxxx / 0x08xxxx 段 = DEVICE_CTRL 的子命令
        return (v & 0xFF0000u) == 0x020000u || (v & 0xFF0000u) == 0x040000u
               || (v & 0xFF0000u) == 0x080000u;
    }

    bool pumpDa()
    {
        if (m_in.size() < 12) return false;
        if (rdLe32(m_in.constData()) != kc::mtk::kXflashMagic) return false;
        const uint32_t length = rdLe32(m_in.constData() + 8);
        if (static_cast<uint32_t>(m_in.size()) < 12 + length) return false;

        const QByteArray payload = m_in.mid(12, static_cast<int>(length));
        packets.append(m_in.left(12 + static_cast<int>(length)));
        m_in.remove(0, 12 + static_cast<int>(length));

        // ① 读回阶段的 ack（4 字节全 0 流包）——只吃指定个数，免得把后续命令也吞了
        if (m_expectedAcks > 0 && payload.size() == 4 && rdLe32(payload.constData()) == 0) {
            --m_expectedAcks;
            return true;
        }
        // ② SYNC 流包：无应答
        if (payload.size() == 4 && rdLe32(payload.constData()) == kc::mtk::kXflashSyncSignal) {
            return true;
        }
        // ③ 命令包
        if (payload.size() == 4) {
            const uint32_t cmd = rdLe32(payload.constData());
            if (isKnownCmd(cmd)) {
                dispatch(cmd);
                return true;
            }
        }
        // ④ 其余按"期望脚本"
        switch (m_expect) {
        case Pkt::ParamOfCmd:
            handleParam(payload);
            return true;
        case Pkt::BootParam:
            m_expect = Pkt::BootData; // boot_to 的参数包不带 status
            return true;
        case Pkt::BootData:
            da2Received = payload;
            emitStatus(da2BootOk ? 0 : 0xC0070004u); // send_data 的状态
            // boot_to 在 sleep(timeout) 之后**还会再读一次** status，正常时为 SYNC_SIGNAL
            emitStatus(da2BootOk ? kc::mtk::kXflashSyncSignal : 0xC0070004u);
            m_expect = Pkt::None;
            return true;
        default:
            return true;
        }
    }

    void dispatch(uint32_t cmd)
    {
        using X = kc::mtk::XCmd;

        if (cmd == static_cast<uint32_t>(X::BootTo)) {
            m_curCmd = cmd;
            emitStatus(0);             // boot_to：xsend 之后立刻读一次 status
            m_expect = Pkt::BootParam; // 参数包本身不回 status（参考实现裸 usbwrite）
            return;
        }
        if (cmd == static_cast<uint32_t>(X::DeviceCtrl)) {
            emitStatus(0);
            m_expect = Pkt::None; // 下一个包是子命令，走 isKnownCmd 分支
            return;
        }
        // 🔴 setup_env / setup_hw_init **没有**命令级 status：
        //    参考实现是 xsend(...) 之后直接 send_param(param)，而 send_param 只在
        //    末尾读一次 status。这里多发一个 → 之后整条链全部错位一格。
        if (cmd == static_cast<uint32_t>(X::SetupEnvironment)
            || cmd == static_cast<uint32_t>(X::SetupHwInitParams)) {
            m_curCmd = cmd;
            m_paramIndex = 0;
            m_curData.clear();
            m_expect = Pkt::ParamOfCmd;
            return;
        }
        // 这几个在 xsend 之后**会**先读一次 status
        // （cmd_write_data / cmd_read_data / formatflash / send_emi / shutdown）
        if (cmd == static_cast<uint32_t>(X::InitExtRam)
            || cmd == static_cast<uint32_t>(X::WriteData)
            || cmd == static_cast<uint32_t>(X::ReadData)
            || cmd == static_cast<uint32_t>(X::Format)
            || cmd == static_cast<uint32_t>(X::Shutdown)) {
            m_curCmd = cmd;
            m_paramIndex = 0;
            m_curData.clear();
            m_emiLenSeen = false;
            m_emiBuf.clear();
            emitStatus(0); // 命令本身的 status（参考实现紧跟着就读一次）
            m_expect = Pkt::ParamOfCmd;
            return;
        }

        // 子命令（0x02xxxx / 0x04xxxx / 0x08xxxx）——都是 send_devctrl 的第二段：
        // 先一次子命令 status，再按参数分岔。
        const uint32_t group = cmd & 0xFF0000u;
        emitStatus(0);
        m_expect = Pkt::None;
        if (group == 0x040000u) {
            // GET_* → 回一个数据包，**调用方读完数据包后还会再读一次 status**
            // （get_packet_length / get_connection_agent / get_chip_id / get_da_version 都是这个写法）
            if (cmd == static_cast<uint32_t>(X::GetConnectionAgent)) {
                emitDataPacket(agent.toLatin1());
            } else if (cmd == static_cast<uint32_t>(X::GetPacketLength)) {
                QByteArray pl(8, '\0');
                qToLittleEndian<uint32_t>(writePacketLen, pl.data());
                qToLittleEndian<uint32_t>(readPacketLen, pl.data() + 4);
                emitDataPacket(pl);
            } else if (cmd == static_cast<uint32_t>(X::GetDaVersion)) {
                emitDataPacket(QByteArray("mock-da-1.0"));
            } else {
                emitDataPacket(le32(0));
            }
            emitStatus(0);
        } else if (group == 0x020000u) {
            // SET_* → 后面跟一个 param（send_param 末尾一次 status）
            m_curCmd = cmd;
            m_paramIndex = 0;
            m_expect = Pkt::ParamOfCmd;
        } else {
            // 0x08xxxx 动作 → send_devctrl(param=None) 返回 xread()，不再读 status
            emitDataPacket(le32(0));
        }
    }

    void handleParam(const QByteArray& payload)
    {
        using X = kc::mtk::XCmd;
        const uint32_t cmd = m_curCmd;
        if (payload.size() == 56 && (cmd == static_cast<uint32_t>(X::ReadData)
            || cmd == static_cast<uint32_t>(X::Format)
            || (cmd == static_cast<uint32_t>(X::WriteData) && m_paramIndex == 0))) {
            storageParams.append(payload);
            const uint64_t addr = qFromLittleEndian<uint64_t>(payload.constData() + 8);
            flashTarget.insert(addr, QStringLiteral("%1/%2")
                .arg(rdLe32(payload.constData())).arg(rdLe32(payload.constData() + 4)));
        }

        if (cmd == static_cast<uint32_t>(X::SetupEnvironment)) {
            emitStatus(0);
            m_expect = Pkt::None;
            return;
        }
        if (cmd == static_cast<uint32_t>(X::SetupHwInitParams)) {
            emitStatus(0);
            // 参考实现：setup_hw_init 之后 DA 回 SYNC 数据包（upload_da1 里的 xread）
            emitDataPacket(le32(kc::mtk::kXflashSyncSignal));
            m_expect = Pkt::None;
            return;
        }
        if (cmd == static_cast<uint32_t>(X::InitExtRam)) {
            // send_emi 是两段式：
            //   ① 裸 xsend(pack("<I", len(emi)))  —— 不回 status
            //   ② send_param([emi])               —— 末尾回一次 status
            if (!m_emiLenSeen) {
                m_emiLen = payload.size() >= 4 ? rdLe32(payload.constData()) : 0;
                m_emiLenSeen = true;
                m_emiBuf.clear();
                emiReceived.clear();
                return;
            }
            m_emiBuf += payload;
            emiReceived = m_emiBuf;
            if (m_emiBuf.size() >= static_cast<int>(m_emiLen)) {
                emitStatus(emiOk ? 0 : 0xC0070004u);
                m_expect = Pkt::None;
            }
            return;
        }
        if (cmd == static_cast<uint32_t>(X::WriteData)) {
            if (m_paramIndex == 0) {
                // 头参数：storage(4) parttype(4) addr(8) size(8) + NandExtension(32)
                if (payload.size() >= 24) {
                    m_curDataAddr = qFromLittleEndian<uint64_t>(payload.constData() + 8);
                    m_curDataSize = qFromLittleEndian<quint64>(payload.constData() + 16);
                    m_curData.clear();
                }
                ++m_paramIndex;
                emitStatus(0); // send_param 末尾的 status
                return;
            }
            // 之后每块 3 个 param：<I 0 / <I 块校验和 / data
            // 🔴 send_param 只在**三个都发完**之后读一次 status，不能每个 param 都回。
            const int phase = (m_paramIndex - 1) % 3;
            if (phase == 1) {
                m_curChunkChecksum = static_cast<uint16_t>(rdLe32(payload.constData()));
            } else if (phase == 2) {
                uint16_t sum = 0;
                for (int i = 0; i < payload.size(); ++i) {
                    sum = static_cast<uint16_t>((sum + static_cast<uint8_t>(payload.at(i))) & 0xFFFF);
                }
                if (sum != m_curChunkChecksum) ++m_badChecksums;
                m_curData += payload;
            }
            ++m_paramIndex;
            if (phase == 2) {
                emitStatus(0); // 这一组 param 的 send_param 末尾 status
                if (static_cast<quint64>(m_curData.size()) >= m_curDataSize) {
                    emitStatus(0); // writeflash 循环结束后的收尾 status
                    flash.insert(m_curDataAddr, m_curData);
                    m_expect = Pkt::None;
                }
            }
            return;
        }
        if (cmd == static_cast<uint32_t>(X::ReadData)) {
            emitStatus(0); // send_param 的 status
            emitStatus(0); // 显式再读一次的 status
            const uint64_t addr = payload.size() >= 24
                                      ? qFromLittleEndian<uint64_t>(payload.constData() + 8)
                                      : 0;
            const quint64 size = payload.size() >= 24
                                     ? qFromLittleEndian<quint64>(payload.constData() + 16)
                                     : 0;
            const QByteArray src = flash.value(addr);
            int pos = 0;
            int blocks = 0;
            while (pos < static_cast<int>(size)) {
                const int block = qMin<int>(static_cast<int>(readPacketLen), static_cast<int>(size) - pos);
                QByteArray chunk = src.mid(pos, block);
                if (chunk.size() < block) chunk.append(block - chunk.size(), '\0');
                emitDataPacket(chunk);
                pos += block;
                ++blocks;
            }
            emitDataPacket(le32(0)); // 收尾
            m_expectedAcks = blocks; // 之后会收到 blocks 个 ack
            m_expect = Pkt::None;
            return;
        }
        if (cmd == static_cast<uint32_t>(X::Format)) {
            ++formatCalls;
            if (!formatOk) {
                emitStatus(0xC0040002u);
                m_expect = Pkt::None;
                return;
            }
            emitStatus(0); // send_param 的 status
            emitStatus(kc::mtk::kXflashStatusComplete); // 显式 status
            m_expect = Pkt::None;
            return;
        }
        emitStatus(0);
        m_expect = Pkt::None;
    }
};
// ═════════════════════════════════════════════════════════════════════════════
//  合成一个最小 DA 文件
// ═════════════════════════════════════════════════════════════════════════════
QByteArray buildDaFile(uint16_t hwCode, const QByteArray& da1, const QByteArray& da2)
{
    // 新版条目长度 0xDC：magic/dword 字段按小端 u16/u32
    constexpr int kEntrySize = 0xDC;
    constexpr int kEntryCount = 1;
    // 🔴 布局必须是 0x00–0x67 头 / 0x68 条目数 / 0x6C 起条目表。
    //    曾经先垫到 0x6C 再 append 条目数 → 计数落在 0x6C、条目表落在 0x70，
    //    整体偏 4 字节 → 解析器在 0x68 读到 0 → "条目数异常 0"。
    QByteArray file(0x68, '\0');
    file.replace(0, 18, QByteArray("MTK_DOWNLOAD_AGENT", 18));

    // count_da（0x68，u32 LE）
    QByteArray count(4, '\0');
    qToLittleEndian<uint32_t>(kEntryCount, count.data());
    file.append(count);

    QByteArray entry(kEntrySize, '\0');
    auto putU16 = [&entry](int off, uint16_t v) { qToLittleEndian<uint16_t>(v, entry.data() + off); };
    auto putU32 = [&entry](int off, uint32_t v) { qToLittleEndian<uint32_t>(v, entry.data() + off); };
    putU16(0x00, kc::mtk::kDaEntryMagic); // 0xDADA
    putU16(0x02, hwCode);
    putU16(0x04, 0x8A00);
    putU16(0x06, 0x0001);
    putU16(0x08, 0x0002);  // sw_version
    putU16(0x0C, 0x0200);  // pagesize
    putU16(0x10, 1);       // entry_region_index
    putU16(0x12, 3);       // entry_region_count = 3

    // region 表从 0x14 起，每项 20 字节
    const int da1Offset = 0x6C + kEntryCount * kEntrySize + 0x40;
    const int da2Offset = da1Offset + da1.size();
    auto putRegion = [&](int index, uint32_t buf, uint32_t len, uint32_t addr, uint32_t sig) {
        const int base = 0x14 + index * 20;
        putU32(base + 0x00, buf);
        putU32(base + 0x04, len);
        putU32(base + 0x08, addr);
        putU32(base + 0x0C, 0);
        putU32(base + 0x10, sig);
    };
    putRegion(0, 0x1000, 0x100, 0x00100000, 0);                     // boot/占位区
    putRegion(1, static_cast<uint32_t>(da1Offset), static_cast<uint32_t>(da1.size()),
              0x4FFF0000, 0x100);                                   // DA1
    putRegion(2, static_cast<uint32_t>(da2Offset), static_cast<uint32_t>(da2.size()),
              0x68000000, 0x100);                                   // DA2

    file.append(entry);
    file.append(QByteArray(0x40, '\0'));  // 填充到 da1Offset
    file.append(da1);
    file.append(da2);
    return file;
}

QString writeTempFile(const QString& dir, const QString& name, const QByteArray& data)
{
    const QString path = QDir(dir).absoluteFilePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    file.write(data);
    file.close();
    return path;
}

// ═════════════════════════════════════════════════════════════════════════════
//  用例
// ═════════════════════════════════════════════════════════════════════════════

void testHandshake()
{
    std::printf("[1] BROM 握手（回显取反）\n");
    {
        MockMtkDevice device;
        kc::mtk::BromSession session(&device, kBorrow);
        QString error;
        check(session.handshake(&error), QStringLiteral("正确回显应握手成功（%1）").arg(error));
    }
    {
        MockMtkDevice device;
        device.handshakeEchoBad = true;
        kc::mtk::BromSession session(&device, kBorrow);
        QString error;
        check(!session.handshake(&error), QStringLiteral("错误回显必须握手失败"));
        check(!error.isEmpty(), QStringLiteral("握手失败要有可读原因"));
    }
}

void testHwCode()
{
    std::printf("[2] GET_HW_CODE（0xFD，大端两个 u16）\n");
    MockMtkDevice device;
    device.hwCode = 0x6768;
    kc::mtk::BromSession session(&device, kBorrow);
    QString error;
    // 不握手直接发 GET_HW_CODE 也应被接受（假设备在 Handshake 状态会吃掉命令字节，
    // 所以这里先握手对齐状态机）。
    check(session.handshake(&error), QStringLiteral("预置：握手成功"));
    uint16_t hw = 0;
    check(session.getHwCode(&hw, &error), QStringLiteral("读 HW code 应成功（%1）").arg(error));
    checkEq(hw, 0x6768, QStringLiteral("HW code"));
}

void testReadWrite32()
{
    std::printf("[3] BROM READ32 / WRITE32（0xD1 / 0xD4）\n");
    MockMtkDevice device;
    kc::mtk::BromSession session(&device, kBorrow);
    QString error;
    check(session.handshake(&error), QStringLiteral("预置：握手成功"));

    const QVector<uint32_t> values{ 0xDEADBEEF, 0x12345678, 0xA5A5A5A5 };
    check(session.write32(0x40000000, values, &error), QStringLiteral("WRITE32 应成功（%1）").arg(error));

    QVector<uint32_t> readback;
    check(session.read32(0x40000000, 3, &readback, &error), QStringLiteral("READ32 应成功（%1）").arg(error));
    checkEq(readback.size(), 3, QStringLiteral("读回字数"));
    check(readback == values, QStringLiteral("读写往返一致"));
}

void testSendDa()
{
    std::printf("[4] SEND_DA（长度宣告 / 分块 / 校验和回读 / SLA）\n");
    {
        MockMtkDevice device;
        kc::mtk::BromSession session(&device, kBorrow);
        QString error;
        check(session.handshake(&error), QStringLiteral("预置：握手成功"));

        QByteArray da(200000, 'D');
        for (int i = 0; i < da.size(); ++i) da[i] = static_cast<char>(i * 7 & 0xFF);
        const uint32_t sigLen = 0x100;
        check(session.sendDa(0x4FFF0000, da, sigLen, &error),
              QStringLiteral("SEND_DA 应成功（%1）").arg(error));
        checkEq(device.daReceived.size(), da.size(), QStringLiteral("设备收到的 DA 字节数"));
        check(device.daReceived == da.left(da.size()), QStringLiteral("DA 内容逐字节一致"));
    }
    {
        MockMtkDevice device;
        device.slaRequired = true;
        kc::mtk::BromSession session(&device, kBorrow);
        QString error;
        check(session.handshake(&error), QStringLiteral("预置：握手成功"));
        QByteArray da(1024, 'S');
        check(!session.sendDa(0x4FFF0000, da, 0, &error), QStringLiteral("SLA 场景必须失败"));
        check(error.contains(QStringLiteral("SLA")), QStringLiteral("错误信息要点明 SLA（实得：%1）").arg(error));
    }
}

void testDaFile()
{
    std::printf("[5] DA 文件解析 / 选条目 / 取 region\n");

    const QByteArray da1(0x400, 'A');
    const QByteArray da2(0x600, 'B');
    const QByteArray content = buildDaFile(0x6768, da1, da2);

    const QString dir = QDir::tempPath() + QStringLiteral("/kc-mtk-test");
    QDir().mkpath(dir);
    const QString path = writeTempFile(dir, QStringLiteral("MTK_AllInOne_DA.bin"), content);
    check(!path.isEmpty(), QStringLiteral("写入合成 DA 文件"));

    check(kc::mtk::DaFile::looksLikeDaFile(path, nullptr), QStringLiteral("头部识别为 DA 文件"));

    QVector<kc::mtk::DaEntry> entries;
    QString error;
    check(kc::mtk::DaFile::parse(path, &entries, &error),
          QStringLiteral("解析 DA 文件（%1）").arg(error));
    checkEq(entries.size(), 1, QStringLiteral("条目数"));
    if (entries.isEmpty()) return;

    checkEq(entries.at(0).hwCode, 0x6768, QStringLiteral("hw_code"));
    checkEq(entries.at(0).entryRegionCount, 3, QStringLiteral("region 数"));
    check(entries.at(0).da1() != nullptr && entries.at(0).da2() != nullptr,
          QStringLiteral("region[1]/region[2] 就位"));

    QString why;
    const kc::mtk::DaEntry* picked = kc::mtk::DaFile::pickForHwCode(entries, 0x6768, &why);
    check(picked != nullptr, QStringLiteral("按 hw code 选中条目（%1）").arg(why));
    check(kc::mtk::DaFile::pickForHwCode(entries, 0x9999, &why) == nullptr,
          QStringLiteral("不存在的 hw code 必须选不中"));
    check(why.contains(QStringLiteral("0x9999")), QStringLiteral("选不中时给出可读原因"));

    QByteArray got;
    check(kc::mtk::DaFile::readRegion(path, *entries.at(0).da1(), &got, &error),
          QStringLiteral("读 DA1 region（%1）").arg(error));
    check(got == da1, QStringLiteral("DA1 内容一致"));
    check(kc::mtk::DaFile::readRegion(path, *entries.at(0).da2(), &got, &error),
          QStringLiteral("读 DA2 region（%1）").arg(error));
    check(got == da2, QStringLiteral("DA2 内容一致"));
}

void testXflash()
{
    std::printf("[6] XFlash（DA2）会话：握手 / 包长 / 写入 / 读回 / 擦除\n");

    MockMtkDevice device;
    device.writePacketLen = 0x1000;  // 4KB 写块
    device.readPacketLen = 0x8000;   // 32KB 读块

    kc::mtk::BromSession brom(&device, kBorrow);
    QString error;
    check(brom.handshake(&error), QStringLiteral("预置：BROM 握手"));
    check(brom.sendDa(0x4FFF0000, QByteArray(4096, 'D'), 0x100, &error),
          QStringLiteral("预置：SEND_DA"));
    check(brom.jumpDa(0x4FFF0000, &error), QStringLiteral("预置：JUMP_DA"));

    kc::mtk::XflashSession xflash(brom.transport());
    checkCall([&] { return xflash.waitDaReady(&error); },
              QStringLiteral("等 DA 就绪 0xC0"), error);
    checkCall([&] { return xflash.handshakeStage2(&error); },
              QStringLiteral("DA2 握手"), error);

    uint32_t w = 0;
    uint32_t r = 0;
    checkCall([&] { return xflash.getPacketLength(&w, &r, &error); },
              QStringLiteral("GET_PACKET_LENGTH"), error);
    checkEq(w, 0x1000, QStringLiteral("写包长"));
    checkEq(r, 0x8000, QStringLiteral("读包长"));

    QString agent;
    checkCall([&] { return xflash.getConnectionAgent(&agent, &error); },
              QStringLiteral("GET_CONNECTION_AGENT"), error);
    checkEq(agent == QStringLiteral("brom") ? 1 : 0, 1, QStringLiteral("agent 内容"));

    checkCall([&] { return xflash.bootTo(0x68000000, QByteArray(2048, 'E'), 0, &error); },
              QStringLiteral("BOOT_TO DA2"), error);
    checkEq(device.da2Received.size(), 2048, QStringLiteral("设备收到的 DA2 字节数"));

    // ── 写入（0x2500 字节 > 一个写包，验证分块 + 逐块校验和） ──
    const QByteArray image(0x2500, 'W');
    checkCall([&] {
        return xflash.writeData(0x1000, kc::mtk::DaStorage::Emmc,
                                static_cast<uint32_t>(kc::mtk::EmmcPartitionType::User), image, &error);
    }, QStringLiteral("WRITE_DATA"), error);
    checkEq(device.m_badChecksums, 0, QStringLiteral("分块校验和错误数"));

    {
        // 长度按 512 扇区对齐（0x2500 = 9472，9472 % 512 = 256 → 补 256 个 0）
        QByteArray expected = image;
        expected.append(512 - (image.size() % 512), '\0');
        const QByteArray got = device.flash.value(0x1000);
        checkEq(got.size(), expected.size(), QStringLiteral("设备侧收到的写入字节数"));
        check(got == expected, QStringLiteral("设备侧写入内容与镜像一致（含扇区补齐）"));
    }

    // ── 读回 ──
    QByteArray readback;
    checkCall([&] {
        return xflash.readData(0x1000, 0x19000, kc::mtk::DaStorage::Emmc,
                               static_cast<uint32_t>(kc::mtk::EmmcPartitionType::User), &readback, &error);
    }, QStringLiteral("READ_DATA"), error);
    checkEq(readback.size(), 0x19000, QStringLiteral("读回字节数"));

    // ── 擦除 ──
    checkCall([&] {
        return xflash.format(0x2000, 0x1000, kc::mtk::DaStorage::Emmc,
                             static_cast<uint32_t>(kc::mtk::EmmcPartitionType::User), &error);
    }, QStringLiteral("FORMAT"), error);
    checkEq(device.formatCalls, 1, QStringLiteral("FORMAT 调用次数"));
}

// 独立应答脚本：测试畸形输入不依赖 Mock 的正常状态机，也不调用被测编码函数。
QByteArray wireHeader(uint32_t length)
{
    return QByteArray::fromHex("efeeeefe01000000") + le32(length);
}

QByteArray wirePacket(const QByteArray& payload)
{
    return wireHeader(static_cast<uint32_t>(payload.size())) + payload;
}

QByteArray readReadyScript()
{
    const QByteArray ok = wirePacket(QByteArray(4, '\0'));
    return ok + ok + ok; // 命令确认、参数确认、读取就绪
}

class ScriptTransport : public kc::mtk::ITransport {
public:
    QByteArray incoming;
    QByteArray written;
    int maxRead = 0;
    int reads = 0;
    std::function<void(int)> onRead;

    bool write(const QByteArray& data) override { written += data; return true; }
    QByteArray readExact(int bytes, int timeoutMs) override
    {
        Q_UNUSED(timeoutMs)
        maxRead = qMax(maxRead, bytes);
        const QByteArray result = incoming.left(bytes);
        incoming.remove(0, result.size());
        ++reads;
        if (onRead) onRead(reads);
        return result;
    }
    bool isOpen() const override { return true; }
    QString lastError() const override { return QString(); }
};

class ShortWriteDevice : public QIODevice {
public:
    explicit ShortWriteDevice(qint64 limit) : m_limit(limit) { open(WriteOnly | Unbuffered); }
    QByteArray accepted;
protected:
    qint64 readData(char*, qint64) override { return -1; }
    qint64 writeData(const char* data, qint64 size) override
    {
        if (m_limit < 0) return -1;
        const qint64 n = qMin(size, m_limit);
        accepted.append(data, static_cast<int>(n));
        return n;
    }
private:
    qint64 m_limit;
};

class FragmentedSource : public QIODevice {
public:
    explicit FragmentedSource(const QByteArray& data) : m_data(data) { open(ReadOnly | Unbuffered); }
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        const int n = static_cast<int>(qMin<qint64>(qMin<qint64>(size, 7), m_data.size() - m_pos));
        for (int i = 0; i < n; ++i) data[i] = m_data.at(m_pos + i);
        m_pos += n;
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    QByteArray m_data;
    int m_pos = 0;
};

class GeneratedSource : public QIODevice {
public:
    explicit GeneratedSource(quint64 size) : m_size(size) { open(ReadOnly | Unbuffered); }
    quint64 consumed = 0;
    bool isSequential() const override { return true; }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        const qint64 n = static_cast<qint64>(qMin<quint64>(static_cast<quint64>(size), m_size - consumed));
        for (qint64 i = 0; i < n; ++i) data[i] = 'S';
        consumed += static_cast<quint64>(n);
        return n;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
private:
    quint64 m_size;
};

void testStorage64AndStreaming()
{
    std::printf("[9] 64 位存储线格式 / 流式补齐 / 包长上限\n");
    MockMtkDevice device;
    device.writePacketLen = 0xFFFFFFFFu;
    device.readPacketLen = 0xFFFFFFFFu;
    kc::mtk::BromSession brom(&device, kBorrow);
    kc::mtk::XflashSession xf(&device);
    QString error;
    check(brom.handshake(&error), QStringLiteral("64 位测试预置：BROM 握手"));
    check(brom.jumpDa(0x4FFF0000, &error), QStringLiteral("64 位测试预置：JUMP_DA"));
    check(xf.waitDaReady(&error), QStringLiteral("64 位测试预置：DA 就绪"));
    check(xf.handshakeStage2(&error), QStringLiteral("64 位测试预置：DA 握手"));
    checkEq(xf.writePacketSize(), 67108864, QStringLiteral("异常协商写包长限制在 64MiB"));
    checkEq(xf.readPacketSize(), 67108864, QStringLiteral("异常协商读包长限制在 64MiB"));
    device.readPacketLen = 512; // Mock 只模拟小数据，高地址不需要大内存。

    constexpr quint64 addr = 0x1234567800ull;
    constexpr uint32_t part = 8; // eMMC USER 的线值
    FragmentedSource source(QByteArray(513, 'W'));
    checkCall([&] { return xf.writeFromDevice(addr, 513, kc::mtk::DaStorage::Emmc, part, &source, &error); },
              QStringLiteral("高地址流式写入（源设备每次最多 7 字节）"), error);
    const QByteArray expectedData = QByteArray(513, 'W') + QByteArray(511, '\0');
    check(device.flash.value(addr) == expectedData, QStringLiteral("高地址内容含正确末扇区补零"));
    check(!device.flash.contains(0x34567800u), QStringLiteral("高地址写入未落到截断后的低地址"));
    checkEq(device.m_badChecksums, 0, QStringLiteral("流式分片的设备端校验和"));

    QByteArray readback;
    QBuffer destination(&readback);
    destination.open(QIODevice::WriteOnly);
    checkCall([&] { return xf.readToDevice(addr, 1024, kc::mtk::DaStorage::Emmc, part, &destination, &error); },
              QStringLiteral("高地址流式读回"), error);
    check(readback == expectedData, QStringLiteral("高地址读回为固定预期字节而非低地址内容"));
    checkCall([&] { return xf.format(addr, 1024, kc::mtk::DaStorage::Emmc, part, &error); },
              QStringLiteral("高地址擦除"), error);

    // 来自 IIQQ 规范的固定字节向量：storage=1、part=8、addr=0x1234567800、size=1024。
    const QByteArray expectedParam = QByteArray::fromHex(
        "010000000800000000785634120000000004000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000");
    checkEq(expectedParam.size(), 56, QStringLiteral("固定存储参数向量长度"));
    checkEq(device.storageParams.size(), 3, QStringLiteral("写/读/擦除各发送一个存储参数"));
    for (const QByteArray& param : device.storageParams)
        check(param == expectedParam, QStringLiteral("写/读/擦除线参数保留完整 64 位地址"));
    check(device.packets.contains(QByteArray::fromHex("efeeeefe0100000038000000") + expectedParam),
          QStringLiteral("参数包头声明 56 字节，正文为固定线向量"));
}

void testLargeStorageStreams()
{
    std::printf("[14] 超过 4GiB 的流长度，不分配整分区内存\n");
    constexpr quint64 addr = 0x1234567800ull;
    constexpr quint64 size = 0x100000200ull;
    const QByteArray expectedParam = QByteArray::fromHex(
        "010000000800000000785634120000000002000001000000"
        "0000000000000000000000000000000000000000000000000000000000000000");
    const QByteArray ok = wirePacket(QByteArray(4, '\0'));
    {
        ScriptTransport tp;
        tp.incoming = ok + ok + ok;
        kc::mtk::XflashSession xf(&tp);
        bool cancel = false;
        xf.setCancelFn([&] { return cancel; });
        xf.setProgressFn([&](qint64, qint64 total) {
            checkEq(total, 4294967808ll, QStringLiteral("大镜像进度总长度不截断"));
            cancel = true;
        });
        GeneratedSource source(size);
        QString error;
        check(!xf.writeFromDevice(addr, size, kc::mtk::DaStorage::Emmc, 8, &source, &error),
              QStringLiteral("大镜像流式写首块后按请求取消"));
        checkEq(static_cast<qint64>(source.consumed), 1024, QStringLiteral("取消前只读一个有界源块"));
        check(tp.written.mid(16, 68) == QByteArray::fromHex("efeeeefe0100000038000000") + expectedParam,
              QStringLiteral("WRITE_DATA 参数的地址及长度均保留高 32 位"));
        check(!xf.isUsable(), QStringLiteral("大镜像中断后废弃 RAW 会话"));
    }
    {
        ScriptTransport tp;
        tp.incoming = readReadyScript() + wirePacket(QByteArray(512, 'R'));
        kc::mtk::XflashSession xf(&tp);
        bool cancel = false;
        xf.setCancelFn([&] { return cancel; });
        xf.setProgressFn([&](qint64, qint64) { cancel = true; });
        QByteArray data;
        QBuffer out(&data);
        out.open(QIODevice::WriteOnly);
        QString error;
        check(!xf.readToDevice(addr, size, kc::mtk::DaStorage::Emmc, 8, &out, &error),
              QStringLiteral("大分区流式读首块后按请求取消"));
        check(data == QByteArray(512, 'R'), QStringLiteral("大分区只输出已收到的一个数据块"));
        check(tp.written.mid(16, 68) == QByteArray::fromHex("efeeeefe0100000038000000") + expectedParam,
              QStringLiteral("READ_DATA 长度不截断到 512"));
    }
    {
        ScriptTransport tp;
        tp.incoming = ok + ok + wirePacket(QByteArray::fromHex("05000440"));
        kc::mtk::XflashSession xf(&tp);
        QString error;
        check(xf.format(addr, size, kc::mtk::DaStorage::Emmc, 8, &error),
              QStringLiteral("大分区擦除接受完整的完成状态"));
        check(tp.written.mid(16, 68) == QByteArray::fromHex("efeeeefe0100000038000000") + expectedParam,
              QStringLiteral("FORMAT 长度保留高 32 位"));
    }
}

void testStatusLengths()
{
    std::printf("[10] 状态包长度边界\n");
    for (uint32_t length : {0u, 1u, 3u, 5u, 65540u, 0xFFFFFFFFu}) {
        ScriptTransport tp;
        tp.incoming = wireHeader(length);
        if (length <= 5) tp.incoming += QByteArray(static_cast<int>(length), '\0');
        kc::mtk::XflashSession xf(&tp);
        checkEq(xf.status(), kc::mtk::XflashSession::kFrameError,
                QStringLiteral("坏状态长度 %1 必须返回帧错误").arg(length));
        check(tp.maxRead <= 12, QStringLiteral("坏状态长度不触发正文分配或读取"));
    }
    for (const QByteArray& payload : {QByteArray::fromHex("3412"), QByteArray::fromHex("34120000"),
                                    QByteArray::fromHex("3412000000000000")}) {
        ScriptTransport tp;
        tp.incoming = wirePacket(payload);
        kc::mtk::XflashSession xf(&tp);
        checkEq(xf.status(), 0x1234, QStringLiteral("合法状态字保持小端解析"));
    }
}

void testReadFailures()
{
    std::printf("[11] 读回严格长度 / 畸形帧 / 丢失收尾\n");
    const QByteArray data = wirePacket(QByteArray(512, 'R'));
    const QByteArray tail = wirePacket(QByteArray(4, '\0'));
    struct Case { const char* name; quint64 size; QByteArray response; };
    const Case cases[] = {
        {"空数据包", 512, wirePacket(QByteArray())},
        {"提前零状态", 512, tail + tail},
        {"提前错误状态", 512, wirePacket(QByteArray::fromHex("01000000"))},
        {"短数据后提前零状态", 1024, data + tail + tail},
        {"超量数据", 512, wirePacket(QByteArray(513, 'R')) + tail},
        {"UINT32_MAX 包长", 512, wireHeader(0xFFFFFFFFu)},
        {"超过 64MiB 包长", 512, wireHeader(67108865u)},
        {"正文短读", 512, wireHeader(512) + QByteArray(31, 'R')},
        {"包头短读", 512, wireHeader(512).left(5)},
        {"丢失收尾", 512, data},
        {"收尾包头短读", 512, data + tail.left(7)},
        {"收尾正文短读", 512, data + tail.left(14)},
        {"收尾长度 1", 512, data + wirePacket(QByteArray(1, '\0'))},
        {"收尾长度 3", 512, data + wirePacket(QByteArray(3, '\0'))},
        {"收尾 magic 错误", 512, data + QByteArray(12, '\0')},
        {"收尾非零", 512, data + wirePacket(QByteArray::fromHex("02000000"))},
        {"额外全零数据不能当尾状态", 512, data + wirePacket(QByteArray(512, '\0')) + tail},
    };
    for (const Case& c : cases) {
        ScriptTransport tp;
        tp.incoming = readReadyScript() + c.response;
        kc::mtk::XflashSession xf(&tp);
        QByteArray out("旧数据");
        QString error;
        const bool ok = xf.readData(0, c.size, kc::mtk::DaStorage::Emmc, 8, &out, &error);
        check(!ok, QStringLiteral("%1 必须失败").arg(QString::fromUtf8(c.name)));
        check(out.isEmpty(), QStringLiteral("失败的 QByteArray wrapper 不暴露部分结果"));
        check(!error.isEmpty(), QStringLiteral("读回失败提供原因"));
        const int written = tp.written.size();
        check(!xf.xsend(kc::mtk::XCmd::ReadData), QStringLiteral("读回失败后禁止继续发命令"));
        checkEq(tp.written.size(), written, QStringLiteral("失效会话不再写 transport"));
        check(tp.maxRead <= 67108864, QStringLiteral("所有读取请求均有界"));
    }
    for (int tailSize : {2, 4}) {
        ScriptTransport tp;
        tp.incoming = readReadyScript() + data + wirePacket(QByteArray(tailSize, '\0'));
        kc::mtk::XflashSession xf(&tp);
        QByteArray out;
        QString error;
        check(xf.readData(0, 512, kc::mtk::DaStorage::Emmc, 8, &out, &error),
              QStringLiteral("恰好 512 字节加完整零尾状态应成功"));
        check(out == QByteArray(512, 'R'), QStringLiteral("成功读回精确固定字节内容"));
        check(tp.incoming.isEmpty(), QStringLiteral("成功消费完整收尾且不留下残帧"));
        const QByteArray expectedWire = QByteArray::fromHex(
            "efeeeefe010000000400000005000100"
            "efeeeefe0100000038000000010000000800000000000000000000000002000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000"
            "efeeeefe010000000400000000000000");
        check(tp.written == expectedWire, QStringLiteral("合法读取线时序固定为命令、参数、单次块 ACK"));
    }
}

void testStreamShortWritesAndCancellation()
{
    std::printf("[12] 流式目标短写 / 取消 / 失效会话\n");
    for (qint64 limit : {qint64(-1), qint64(0), qint64(511)}) {
        ScriptTransport tp;
        tp.incoming = readReadyScript() + wirePacket(QByteArray(512, 'R'))
                      + wirePacket(QByteArray(4, '\0'));
        kc::mtk::XflashSession xf(&tp);
        ShortWriteDevice out(limit);
        QString error;
        check(!xf.readToDevice(0, 512, kc::mtk::DaStorage::Emmc, 8, &out, &error),
              QStringLiteral("目标 write 返回 %1 应失败").arg(limit));
        check(!xf.isUsable(), QStringLiteral("短写发生在 RAW 中，必须废弃会话"));
        checkEq(tp.written.size(), 84, QStringLiteral("短写后不能向设备确认数据块"));
    }
    {
        ScriptTransport tp;
        kc::mtk::XflashSession xf(&tp);
        xf.setCancelFn([] { return true; });
        QByteArray out;
        QString error;
        check(!xf.readData(0, 512, kc::mtk::DaStorage::Emmc, 8, &out, &error),
              QStringLiteral("传输前取消应失败"));
        check(tp.written.isEmpty(), QStringLiteral("传输前取消不发送命令"));
    }
    {
        ScriptTransport tp;
        bool cancel = false;
        tp.incoming = readReadyScript() + wirePacket(QByteArray(512, 'R'))
                      + wirePacket(QByteArray(4, '\0'));
        tp.onRead = [&](int count) { if (count == 8) cancel = true; }; // 数据正文返回时取消
        kc::mtk::XflashSession xf(&tp);
        xf.setCancelFn([&] { return cancel; });
        QByteArray bytes;
        QBuffer out(&bytes);
        out.open(QIODevice::WriteOnly);
        QString error;
        check(!xf.readToDevice(0, 512, kc::mtk::DaStorage::Emmc, 8, &out, &error),
              QStringLiteral("RAW 正文读取中取消应失败"));
        check(bytes.isEmpty(), QStringLiteral("取消后不再将正文写入目标"));
        cancel = false;
        check(!xf.xsend(kc::mtk::XCmd::ReadData), QStringLiteral("清取消标志也不能复用中断的会话"));
    }
    {
        ScriptTransport tp;
        bool cancel = false;
        const QByteArray ok = wirePacket(QByteArray(4, '\0'));
        tp.incoming = ok + ok + ok + ok;
        kc::mtk::XflashSession xf(&tp);
        xf.setCancelFn([&] { return cancel; });
        xf.setProgressFn([&](qint64, qint64) { cancel = true; });
        FragmentedSource source(QByteArray(2048, 'W'));
        QString error;
        check(!xf.writeFromDevice(0, 2048, kc::mtk::DaStorage::Emmc, 8, &source, &error),
              QStringLiteral("写入首块后取消应失败"));
        cancel = false;
        check(!xf.isUsable() && !xf.xsend(kc::mtk::XCmd::WriteData),
              QStringLiteral("取消的写入会话不能继续复用"));
    }
    {
        ScriptTransport tp;
        const QByteArray ok = wirePacket(QByteArray(4, '\0'));
        tp.incoming = ok + ok;
        kc::mtk::XflashSession xf(&tp);
        FragmentedSource source(QByteArray(31, 'W'));
        QString error;
        check(!xf.writeFromDevice(0, 512, kc::mtk::DaStorage::Emmc, 8, &source, &error),
              QStringLiteral("源设备未提供声明的字节数应失败"));
        check(!xf.isUsable(), QStringLiteral("源短读中断 RAW 会话"));
    }
    {
        ScriptTransport tp;
        bool cancel = false;
        const QByteArray ok = wirePacket(QByteArray(4, '\0'));
        tp.incoming = ok + ok + wirePacket(QByteArray::fromHex("04000440"));
        tp.onRead = [&](int count) { if (count == 6) cancel = true; };
        kc::mtk::XflashSession xf(&tp);
        xf.setCancelFn([&] { return cancel; });
        QString error;
        check(!xf.format(0, 512, kc::mtk::DaStorage::Emmc, 8, &error),
              QStringLiteral("FORMAT 进度轮询中取消应失败"));
        cancel = false;
        check(!xf.isUsable(), QStringLiteral("取消的 FORMAT 会话不能继续复用"));
    }
    {
        MockMtkDevice device;
        kc::mtk::BromSession brom(&device, kBorrow);
        brom.setCancelFn([] { return true; });
        QString error;
        check(!brom.handshake(&error), QStringLiteral("BROM 握手取消回调生效"));
    }
}

void testWriteWireAndTail()
{
    std::printf("[15] 合法写入线时序 / 写入丢尾\n");
    const QByteArray ok = wirePacket(QByteArray(4, '\0'));
    {
        ScriptTransport tp;
        for (int i = 0; i < 7; ++i) tp.incoming += ok;
        kc::mtk::XflashSession xf(&tp);
        QString error;
        check(xf.writeData(0, kc::mtk::DaStorage::Emmc, 8, QByteArray(512, '\0'), &error),
              QStringLiteral("旧写入 wrapper 仍支持合法扇区镜像"));
        const QByteArray expectedWire = QByteArray::fromHex(
            "efeeeefe010000000400000004000100"
            "efeeeefe0100000038000000010000000800000000000000000000000002000000000000"
            "0000000000000000000000000000000000000000000000000000000000000000"
            "efeeeefe010000000400000000000000"
            "efeeeefe010000000400000000000000"
            "efeeeefe0100000000020000") + QByteArray(512, '\0') + QByteArray::fromHex(
            "efeeeefe010000000400000009000100"
            "efeeeefe010000000400000005000800");
        check(tp.written == expectedWire,
              QStringLiteral("写入仍为命令、存储参数、0/校验和/数据、原有下载动作"));
        check(tp.incoming.isEmpty(), QStringLiteral("合法写入状态消费数量保持不变"));
    }
    for (int replies : {3, 4, 5, 6}) {
        ScriptTransport tp;
        for (int i = 0; i < replies; ++i) tp.incoming += ok;
        kc::mtk::XflashSession xf(&tp);
        QString error;
        check(!xf.writeData(0, kc::mtk::DaStorage::Emmc, 8, QByteArray(512, '\0'), &error),
              QStringLiteral("写入收尾/下载动作应答丢失（仅 %1 包）必须失败").arg(replies));
        check(!xf.isUsable(), QStringLiteral("写入收尾异常后禁止复用会话"));
    }
}

void testStorageValidation()
{
    std::printf("[13] 存储长度 / 对齐 / 64 位溢出预检\n");
    struct Case { quint64 addr; quint64 size; };
    const Case cases[] = {
        {0, 0}, {1, 512}, {0, 513},
        {std::numeric_limits<quint64>::max() - 511, 512},
        {0, quint64(1) << 63}
    };
    for (const Case& c : cases) {
        ScriptTransport tp;
        kc::mtk::XflashSession xf(&tp);
        QString error;
        QByteArray bytes;
        QBuffer out(&bytes);
        out.open(QIODevice::WriteOnly);
        check(!xf.readToDevice(c.addr, c.size, kc::mtk::DaStorage::Emmc, 8, &out, &error),
              QStringLiteral("非法读回范围应在命令前拒绝"));
        check(!xf.format(c.addr, c.size, kc::mtk::DaStorage::Emmc, 8, &error),
              QStringLiteral("非法擦除范围应在命令前拒绝"));
        check(tp.written.isEmpty(), QStringLiteral("范围校验失败不接触设备"));
    }
    for (const Case& c : {Case{0, 0}, Case{1, 512},
                          Case{std::numeric_limits<quint64>::max() - 511, 1},
                          Case{0, std::numeric_limits<quint64>::max()}}) {
        ScriptTransport tp;
        kc::mtk::XflashSession xf(&tp);
        FragmentedSource source(QByteArray(512, 'W'));
        QString error;
        check(!xf.writeFromDevice(c.addr, c.size, kc::mtk::DaStorage::Emmc, 8, &source, &error),
              QStringLiteral("写入含补齐的非法范围应在命令前拒绝"));
        check(tp.written.isEmpty(), QStringLiteral("写入范围失败没有发 WRITE_DATA"));
    }
}

void testRegionMapping()
{
    std::printf("[7] scatter region → (DaStorage, parttype) 映射\n");
    struct Case {
        const char* region;
        kc::mtk::StorageType hint;
        kc::mtk::DaStorage storage;
        uint32_t partType;
    };
    const Case cases[] = {
        { "EMMC_USER", kc::mtk::StorageType::Emmc, kc::mtk::DaStorage::Emmc,
          static_cast<uint32_t>(kc::mtk::EmmcPartitionType::User) },
        { "EMMC_BOOT_1", kc::mtk::StorageType::Emmc, kc::mtk::DaStorage::Emmc,
          static_cast<uint32_t>(kc::mtk::EmmcPartitionType::Boot1) },
        { "EMMC_BOOT_2", kc::mtk::StorageType::Emmc, kc::mtk::DaStorage::Emmc,
          static_cast<uint32_t>(kc::mtk::EmmcPartitionType::Boot2) },
        { "EMMC_RPMB", kc::mtk::StorageType::Emmc, kc::mtk::DaStorage::Emmc,
          static_cast<uint32_t>(kc::mtk::EmmcPartitionType::Rpmb) },
        { "UFS_LU0", kc::mtk::StorageType::Ufs, kc::mtk::DaStorage::Ufs,
          static_cast<uint32_t>(kc::mtk::UfsPartitionType::User) },
        { "UFS_LU1", kc::mtk::StorageType::Ufs, kc::mtk::DaStorage::Ufs,
          static_cast<uint32_t>(kc::mtk::UfsPartitionType::Boot1) },
        { "NAND", kc::mtk::StorageType::Nand, kc::mtk::DaStorage::Nand, 0 },
        { "HW_STORAGE_NOR", kc::mtk::StorageType::Nor, kc::mtk::DaStorage::Nor, 0 },
    };

    for (const Case& c : cases) {
        kc::mtk::DaTarget target;
        const bool ok = kc::mtk::regionToTarget(QString::fromLatin1(c.region), c.hint, &target);
        check(ok, QStringLiteral("%1 应可映射").arg(QLatin1String(c.region)));
        if (!ok) continue;
        check(target.storage == c.storage,
              QStringLiteral("%1 的 DaStorage（实得 0x%2）")
                  .arg(QLatin1String(c.region))
                  .arg(static_cast<uint32_t>(target.storage), 0, 16));
        checkEq(target.partType, c.partType, QStringLiteral("%1 的 parttype").arg(QLatin1String(c.region)));
    }

    // region 认不出来时回退到 scatter 顶层 storage
    kc::mtk::DaTarget target;
    check(kc::mtk::regionToTarget(QStringLiteral("user"), kc::mtk::StorageType::Ufs, &target),
          QStringLiteral("裸 \"user\" + UFS 提示应可映射"));
    check(target.storage == kc::mtk::DaStorage::Ufs, QStringLiteral("回退到 UFS"));

    check(!kc::mtk::regionToTarget(QString(), kc::mtk::StorageType::None, &target),
          QStringLiteral("空 region + 无提示 → 必须映射失败"));
}

void testExtractEmi()
{
    std::printf("[8] preloader → EMI（DRAM 参数）抽取\n");

    // A：bldr 串在开头（xflash 体系）→ 整段 data 就是 EMI
    {
        QByteArray pl(0x40, static_cast<char>(0xAB));
        pl.replace(0, 21, QByteArray("MTK_BLOADER_INFO_v10", 21));
        const QByteArray emi = kc::mtk::extractEmiFromPreloader(pl);
        check(emi == pl, QStringLiteral("bldr 串在开头时应原样返回（实得 %1 字节）").arg(emi.size()));
    }

    // B：bldr 串在中间 + 有 MTK_BIN → 取 MTK_BIN + 0xC 之后
    {
        QByteArray pl(0x100, static_cast<char>(0x11));
        pl.replace(0x20, 21, QByteArray("MTK_BLOADER_INFO_v10", 21));
        pl.replace(0x60, 7, QByteArray("MTK_BIN", 7));
        const QByteArray emi = kc::mtk::extractEmiFromPreloader(pl);
        check(emi == pl.mid(0x60 + 0xC), QStringLiteral("老式路径应取 MTK_BIN + 0xC 之后"));
    }

    // C：两样都没有 → 空（调用方只警告，不判死）
    {
        const QByteArray pl(0x100, static_cast<char>(0x22));
        check(kc::mtk::extractEmiFromPreloader(pl).isEmpty(),
              QStringLiteral("无 EMI 特征时应返回空而不是瞎猜"));
    }

    // D：EMI magic 路径（4D 4D 4D 01 38 00 00 00）后的切片不能越界崩
    {
        QByteArray pl(0x200, static_cast<char>(0x33));
        pl.replace(0x10, 8, QByteArray("\x4D\x4D\x4D\x01\x38\x00\x00\x00", 8));
        // mlen @0x20 / siglen @0x2C（相对切片后），故意给个越界的 mlen
        qToLittleEndian<uint32_t>(0xFFFFF, pl.data() + 0x10 + 0x20);
        qToLittleEndian<uint32_t>(0x10, pl.data() + 0x10 + 0x2C);
        const QByteArray emi = kc::mtk::extractEmiFromPreloader(pl);
        check(emi.isEmpty(), QStringLiteral("越界的 mlen 必须被边界检查挡下（不崩、不瞎猜）"));
    }
}

} // namespace

int main(int argc, char** argv)
{
    // 关掉 stdout 缓冲：一旦中途崩了，至少能看到"崩在哪一条用例"。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    QCoreApplication app(argc, argv);

    std::printf("=== MTK 协议层测试（假设备）===\n");
    testHandshake();
    testHwCode();
    testReadWrite32();
    testSendDa();
    testDaFile();
    testXflash();
    testRegionMapping();
    testExtractEmi();
    testStorage64AndStreaming();
    testStatusLengths();
    testReadFailures();
    testStreamShortWritesAndCancellation();
    testStorageValidation();
    testLargeStorageStreams();
    testWriteWireAndTail();

    std::printf("\n结果：%d 通过 / %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) std::printf("ALL MTK PROTOCOL TESTS PASSED\n");
    return g_fail == 0 ? 0 : 1;
}
