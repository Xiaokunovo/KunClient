#include "kc/mtk/MtkXflash.h"

#include "kc/mtk/MtkTransport.h"

#include <QBuffer>
#include <QThread>
#include <QtEndian>

#include <limits>

namespace kc::mtk {
namespace {

constexpr int kFrameTimeoutMs = 5000;
constexpr int kDataTimeoutMs = 30000;
constexpr int kParamChunk = kXflashParamChunk; // 0x200
constexpr int kMaxContinueRounds = 4096;       // 进度轮询硬上限，防设备侧死循环
constexpr uint32_t kMaxPacket = 64u * 1024u * 1024u;

bool validStorageRange(quint64 addr, quint64 size, QString* error)
{
    if (size == 0 || size > static_cast<quint64>(std::numeric_limits<qint64>::max())
        || addr % 512 != 0 || size % 512 != 0
        || size > std::numeric_limits<quint64>::max() - addr) {
        if (error) *error = QStringLiteral("存储范围无效：长度为零、未按 512 对齐或地址/长度溢出");
        return false;
    }
    return true;
}

// 命令已开始但未完整收尾时，禁止在不可信的帧边界上继续发命令。
struct StorageTransfer {
    bool& usable;
    bool complete = false;
    explicit StorageTransfer(bool& value) : usable(value) {}
    ~StorageTransfer() { if (!complete) usable = false; }
};

QByteArray leU32(uint32_t value)
{
    QByteArray out(4, '\0');
    qToLittleEndian<uint32_t>(value, out.data());
    return out;
}

uint16_t rdLe16(const char* p) { return qFromLittleEndian<uint16_t>(p); }
uint32_t rdLe32(const char* p) { return qFromLittleEndian<uint32_t>(p); }

/// XFlash 的 storage 参数是 pack("<IIQQ", storage, parttype, addr, size) + NandExtension(8×u32=0)
/// 共 24 + 32 = 56 字节。NandExtension 全零 = cellusage uni / addr_type logical / raw。
QByteArray storageParam(const DaStorage& storage, uint32_t partType, quint64 addr, quint64 size)
{
    QByteArray param(56, '\0');
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(storage), param.data() + 0);
    qToLittleEndian<uint32_t>(partType, param.data() + 4);
    qToLittleEndian<quint64>(addr, param.data() + 8);
    qToLittleEndian<quint64>(size, param.data() + 16);
    // 24..55 保持 0（NandExtension）
    return param;
}

} // namespace

XflashSession::XflashSession(ITransport* transport, QObject* parent)
    : QObject(parent)
    , m_tp(transport)
{
}

// ── 帧原语 ──────────────────────────────────────────────────────────────────

bool XflashSession::xsend(XCmd cmd, XDataType type, const QByteArray& payload)
{
    if (!m_usable || cancelled() || !m_tp || !m_tp->isOpen()) return false;

    // 🔴 参考实现的 xsend(data)：包头 length = len(data)，而**命令本身就是 data**
    //    （`xsend(int)` 会 pack("<I") ⇒ 命令帧的 payload 恒为 4 字节小端）。
    //    曾经写成"payload 为空就 length=0"，于是每一帧命令都成了空负载，
    //    设备端根本看不到命令 —— 整条 DA 链在第一条命令就断掉。
    const QByteArray body =
        payload.isEmpty() ? leU32(static_cast<uint32_t>(cmd)) : payload;
    if (static_cast<quint64>(body.size()) > kMaxPacket) return false;

    QByteArray header(12, '\0');
    qToLittleEndian<uint32_t>(kXflashMagic, header.data() + 0);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(type), header.data() + 4);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(body.size()), header.data() + 8);

    if (!m_tp->write(header)) return false;
    if (!body.isEmpty() && !m_tp->write(body)) return false;
    return true;
}

QByteArray XflashSession::xread(int timeoutMs, bool* ok)
{
    if (ok) *ok = false;
    if (!m_usable || cancelled() || !m_tp || !m_tp->isOpen()) return QByteArray();

    const QByteArray header = m_tp->readExact(12, timeoutMs);
    if (header.size() != 12) return QByteArray();
    if (rdLe32(header.constData()) != kXflashMagic) {
        emit logLine(QStringLiteral("error"),
                     QStringLiteral("xread：包头 magic 不对（0x%1）")
                         .arg(rdLe32(header.constData()), 8, 16, QLatin1Char('0')));
        return QByteArray();
    }
    const uint32_t length = rdLe32(header.constData() + 8);
    if (length > kMaxPacket) {
        emit logLine(QStringLiteral("error"), QStringLiteral("xread：包长异常（%1）").arg(length));
        return QByteArray();
    }
    QByteArray payload;
    if (length > 0) {
        payload = m_tp->readExact(static_cast<int>(length), timeoutMs);
        if (static_cast<uint32_t>(payload.size()) != length) {
            emit logLine(QStringLiteral("error"),
                         QStringLiteral("xread：数据不足（%1/%2）").arg(payload.size()).arg(length));
            return QByteArray();
        }
    }
    if (ok) *ok = true;
    return payload;
}

qint64 XflashSession::status(int timeoutMs)
{
    if (!m_usable || cancelled() || !m_tp || !m_tp->isOpen()) return kFrameError;

    const QByteArray header = m_tp->readExact(12, timeoutMs);
    if (header.size() != 12) return kFrameError;
    if (rdLe32(header.constData()) != kXflashMagic) {
        emit logLine(QStringLiteral("error"),
                     QStringLiteral("status：包头 magic 不对（0x%1）")
                         .arg(rdLe32(header.constData()), 8, 16, QLatin1Char('0')));
        return kFrameError;
    }
    const uint32_t length = rdLe32(header.constData() + 8);
    if (length != 2 && (length < 4 || length > 64u * 1024u || length % 4 != 0))
        return kFrameError;

    const QByteArray payload = m_tp->readExact(static_cast<int>(length), timeoutMs);
    if (static_cast<uint32_t>(payload.size()) != length) return kFrameError;

    if (length == 2) {
        const uint16_t value = rdLe16(payload.constData());
        return value == 0 ? qint64(0) : qint64(value);
    }
    if (length == 4) {
        const uint32_t value = rdLe32(payload.constData());
        if (value == 0 || value == kXflashMagic) return qint64(0);
        return qint64(value);
    }
    return qint64(rdLe32(payload.constData()));
}

QString XflashSession::statusText(qint64 status) const
{
    if (status == kFrameError) return QStringLiteral("帧错误（包头/长度不对）");
    const uint32_t value = static_cast<uint32_t>(status);
    const uint32_t severity = value >> 30;
    const uint32_t domain = (value >> 16) & 0xFF;
    const uint32_t code = value & 0xFFFF;
    const char* sevName = severity == 0 ? "Success" : severity == 1 ? "Info"
                        : severity == 2 ? "Warning" : "Error";
    return QStringLiteral("%1（0x%2：severity=%3 domain=%4 code=0x%5）")
        .arg(QLatin1String(sevName))
        .arg(value, 8, 16, QLatin1Char('0'))
        .arg(severity)
        .arg(domain)
        .arg(code, 4, 16, QLatin1Char('0'));
}

bool XflashSession::sendParam(const QVector<QByteArray>& params)
{
    if (!m_usable || cancelled() || !m_tp || !m_tp->isOpen()) return false;

    for (const QByteArray& param : params) {
        if (cancelled() || static_cast<quint64>(param.size()) > kMaxPacket) return false;
        QByteArray header(12, '\0');
        qToLittleEndian<uint32_t>(kXflashMagic, header.data() + 0);
        qToLittleEndian<uint32_t>(static_cast<uint32_t>(XDataType::ProtocolFlow), header.data() + 4);
        qToLittleEndian<uint32_t>(static_cast<uint32_t>(param.size()), header.data() + 8);
        if (!m_tp->write(header)) return false;

        int pos = 0;
        while (pos < param.size()) {
            if (cancelled()) return false;
            const int size = qMin(kParamChunk, param.size() - pos);
            if (!m_tp->write(param.mid(pos, size))) return false;
            pos += size;
        }
    }

    const qint64 st = status();
    if (st == 0) return true;
    if (static_cast<uint32_t>(st) == 0xc0040050u) {
        // 参考实现把这一条当"可接受"（少数 DA 用它表示参数已收下）。
        emit logLine(QStringLiteral("warning"),
                     QStringLiteral("send_param 返回 0xC0040050（按参考实现视为可接受）"));
        return true;
    }
    emit logLine(QStringLiteral("error"),
                 QStringLiteral("send_param 失败：%1").arg(statusText(st)));
    return false;
}

bool XflashSession::sendData(const QByteArray& data)
{
    if (!m_usable || cancelled() || !m_tp || !m_tp->isOpen()
        || static_cast<quint64>(data.size()) > kMaxPacket) return false;

    QByteArray header(12, '\0');
    qToLittleEndian<uint32_t>(kXflashMagic, header.data() + 0);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(XDataType::ProtocolFlow), header.data() + 4);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(data.size()), header.data() + 8);
    if (!m_tp->write(header)) return false;

    const int maxPacket = qMax(1, m_tp->maxWritePacket());
    int pos = 0;
    while (pos < data.size()) {
        if (cancelled()) return false;
        const int size = qMin(maxPacket, data.size() - pos);
        if (!m_tp->write(data.mid(pos, size))) return false;
        pos += size;
    }
    const qint64 st = status();
    if (st == 0) return true;
    emit logLine(QStringLiteral("error"), QStringLiteral("send_data 失败：%1").arg(statusText(st)));
    return false;
}

bool XflashSession::ack()
{
    // ack = 一个 length=4、内容为 0 的 PROTOCAL_FLOW 包；随后读一次状态。
    // ⚠️ 部分芯片（dacode 0x6781）用 <IIII 单包写法，本实现统一用 <III + <I 双写，
    //    真机若在此处卡住，按 chipconfig.dacode 分支改造。
    if (!sendFlowPacket(leU32(0), kFrameTimeoutMs)) return false;
    return status() == 0;
}

bool XflashSession::sendFlowPacket(const QByteArray& payload, int timeoutMs)
{
    if (!m_usable || cancelled() || !m_tp || !m_tp->isOpen()
        || static_cast<quint64>(payload.size()) > kMaxPacket) return false;
    QByteArray header(12, '\0');
    qToLittleEndian<uint32_t>(kXflashMagic, header.data() + 0);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(XDataType::ProtocolFlow), header.data() + 4);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(payload.size()), header.data() + 8);
    Q_UNUSED(timeoutMs)
    if (!m_tp->write(header)) return false;
    if (!payload.isEmpty() && !m_tp->write(payload)) return false;
    return true;
}

bool XflashSession::sendDevCtrl(XCmd cmd, const QByteArray& param, QByteArray* out)
{
    if (!xsend(XCmd::DeviceCtrl)) return false;
    const qint64 s1 = status();
    if (s1 != 0) {
        if (static_cast<uint32_t>(s1) != 0xC0010004u) {
            emit logLine(QStringLiteral("error"),
                         QStringLiteral("DEVICE_CTRL 失败：%1").arg(statusText(s1)));
        }
        return false;
    }
    if (!xsend(cmd)) return false;
    const qint64 s2 = status();
    if (s2 != 0) {
        if (static_cast<uint32_t>(s2) != 0xC0010004u) {
            emit logLine(QStringLiteral("error"),
                         QStringLiteral("设备控制 0x%1 失败：%2")
                             .arg(static_cast<uint32_t>(cmd), 6, 16, QLatin1Char('0'))
                             .arg(statusText(s2)));
        }
        return false;
    }
    if (param.isEmpty()) {
        bool ok = false;
        const QByteArray data = xread(kFrameTimeoutMs, &ok);
        if (!ok) return false;
        if (out) *out = data;
        return true;
    }
    return sendParam(param);
}

// ── DA2 初始化 ──────────────────────────────────────────────────────────────

bool XflashSession::waitDaReady(QString* error)
{
    if (!m_tp || !m_tp->isOpen()) {
        if (error) *error = QStringLiteral("DA 口未打开");
        return false;
    }
    // DA1 起来后会先发 1 字节 0xC0。设备可能顺带吐一些串口日志，跳过并记录。
    constexpr int kMaxReads = 40;
    for (int i = 0; i < kMaxReads; ++i) {
        if (cancelled()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        const QByteArray one = m_tp->readExact(1, 250);
        if (one.isEmpty()) continue;
        if (static_cast<uint8_t>(one.at(0)) == kDaReadyByte) {
            emit logLine(QStringLiteral("success"), QStringLiteral("DA 已就绪（收到 0xC0）"));
            return true;
        }
        emit logLine(QStringLiteral("info"),
                     QStringLiteral("DA 口杂散字节 0x%1（已跳过）")
                         .arg(static_cast<uint8_t>(one.at(0)), 2, 16, QLatin1Char('0')));
    }
    if (error) *error = QStringLiteral("等待 DA 就绪超时（未收到 0xC0）");
    return false;
}

bool XflashSession::syncDa(QString* error)
{
    // 参考实现 Sync：xsend(pack("<I", 0x434E5953))，即一个 payload 为 SYNC 值的流包。
    if (!sendFlowPacket(leU32(kXflashSyncSignal), kFrameTimeoutMs)) {
        if (error) *error = QStringLiteral("发送 SYNC 失败");
        return false;
    }
    return true;
}

bool XflashSession::setupEnv(QString* error)
{
    if (!xsend(XCmd::SetupEnvironment)) {
        if (error) *error = QStringLiteral("SETUP_ENVIRONMENT 发送失败");
        return false;
    }
    // pack("<IIIII", da_log_level, log_channel, system_os, ufs_provision, 0)
    //   log_level=2 / log_channel=2(USB) / system_os=1(LINUX) / ufs=0 / 0
    QByteArray param(20, '\0');
    qToLittleEndian<uint32_t>(2, param.data() + 0);
    qToLittleEndian<uint32_t>(2, param.data() + 4);
    qToLittleEndian<uint32_t>(1, param.data() + 8);
    if (!sendParam(param)) {
        if (error) *error = QStringLiteral("SETUP_ENVIRONMENT 参数被拒");
        return false;
    }
    return true;
}

bool XflashSession::setupHwInit(QString* error)
{
    if (!xsend(XCmd::SetupHwInitParams)) {
        if (error) *error = QStringLiteral("SETUP_HW_INIT_PARAMS 发送失败");
        return false;
    }
    if (!sendParam(leU32(0))) {
        if (error) *error = QStringLiteral("SETUP_HW_INIT_PARAMS 参数被拒");
        return false;
    }
    return true;
}

bool XflashSession::handshakeStage2(QString* error)
{
    if (!syncDa(error)) return false;
    if (!setupEnv(error)) return false;
    if (!setupHwInit(error)) return false;

    bool ok = false;
    const QByteArray res = xread(kFrameTimeoutMs, &ok);
    if (!ok || res.size() != 4) {
        if (error) *error = QStringLiteral("DA2 未回 SYNC 数据包");
        return false;
    }
    if (rdLe32(res.constData()) != kXflashSyncSignal) {
        if (error) {
            *error = QStringLiteral("DA2 SYNC 值不符（0x%1）")
                         .arg(rdLe32(res.constData()), 8, 16, QLatin1Char('0'));
        }
        return false;
    }
    emit logLine(QStringLiteral("success"), QStringLiteral("DA2 会话已建立（SYNC 0x434E5953）"));
    refreshPacketSize();
    return true;
}

// ── 信息查询 ────────────────────────────────────────────────────────────────

bool XflashSession::getConnectionAgent(QString* agent, QString* error)
{
    QByteArray data;
    if (!sendDevCtrl(XCmd::GetConnectionAgent, QByteArray(), &data)) {
        if (error) *error = QStringLiteral("GET_CONNECTION_AGENT 失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("GET_CONNECTION_AGENT 状态非 0");
        return false;
    }
    if (agent) *agent = QString::fromLatin1(data).trimmed();
    return true;
}

bool XflashSession::getDaVersion(QString* version, QString* error)
{
    QByteArray data;
    if (!sendDevCtrl(XCmd::GetDaVersion, QByteArray(), &data)) {
        if (error) *error = QStringLiteral("GET_DA_VERSION 失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("GET_DA_VERSION 状态非 0");
        return false;
    }
    if (version) *version = QString::fromLatin1(data).trimmed();
    return true;
}

bool XflashSession::getPacketLength(uint32_t* writeLen, uint32_t* readLen, QString* error)
{
    QByteArray data;
    if (!sendDevCtrl(XCmd::GetPacketLength, QByteArray(), &data)) {
        if (error) *error = QStringLiteral("GET_PACKET_LENGTH 失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("GET_PACKET_LENGTH 状态非 0");
        return false;
    }
    if (data.size() < 8) {
        if (error) *error = QStringLiteral("GET_PACKET_LENGTH 应答过短（%1）").arg(data.size());
        return false;
    }
    if (writeLen) *writeLen = rdLe32(data.constData());
    if (readLen) *readLen = rdLe32(data.constData() + 4);
    return true;
}

void XflashSession::refreshPacketSize()
{
    uint32_t w = 0;
    uint32_t r = 0;
    if (getPacketLength(&w, &r, nullptr)) {
        // 写包必须是 512 的整数倍（scatter 扇区对齐）
        if (w >= 512) m_writePacket = (qMin(w, kMaxPacket) / 512) * 512;
        if (r >= 512) m_readPacket = (qMin(r, kMaxPacket) / 512) * 512;
        emit logLine(QStringLiteral("info"),
                     QStringLiteral("DA 最优包长：写 %1 B / 读 %2 B").arg(m_writePacket).arg(m_readPacket));
    } else {
        emit logLine(QStringLiteral("warning"),
                     QStringLiteral("未能取得 DA 包长，回退写 %1 B / 读 %2 B")
                         .arg(m_writePacket)
                         .arg(m_readPacket));
    }
}

bool XflashSession::setChecksumLevel(uint32_t level, QString* error)
{
    if (!sendDevCtrl(XCmd::SetChecksumLevel, leU32(level))) {
        if (error) *error = QStringLiteral("SET_CHECKSUM_LEVEL 失败");
        return false;
    }
    return true;
}

bool XflashSession::setResetKey(uint32_t key, QString* error)
{
    if (!sendDevCtrl(XCmd::SetResetKey, leU32(key))) {
        if (error) *error = QStringLiteral("SET_RESET_KEY 失败");
        return false;
    }
    return true;
}

bool XflashSession::slaEnabledStatus(bool* enabled, QString* error)
{
    QByteArray data;
    if (!sendDevCtrl(XCmd::SlaEnabledStatus, QByteArray(), &data)) {
        if (error) *error = QStringLiteral("SLA_ENABLED_STATUS 失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("SLA_ENABLED_STATUS 状态非 0");
        return false;
    }
    uint32_t value = 0;
    for (int i = 0; i < data.size(); ++i) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(data.at(i))) << (8 * i);
    }
    if (enabled) *enabled = (value != 0);
    return true;
}

bool XflashSession::getChipId(uint16_t* hwCode, uint16_t* hwSubCode, uint16_t* hwVersion, QString* error)
{
    QByteArray data;
    if (!sendDevCtrl(XCmd::GetChipId, QByteArray(), &data)) {
        if (error) *error = QStringLiteral("GET_CHIP_ID 失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("GET_CHIP_ID 状态非 0");
        return false;
    }
    if (data.size() < 10) {
        if (error) *error = QStringLiteral("GET_CHIP_ID 应答过短（%1）").arg(data.size());
        return false;
    }
    if (hwCode) *hwCode = rdLe16(data.constData() + 0);
    if (hwSubCode) *hwSubCode = rdLe16(data.constData() + 2);
    if (hwVersion) *hwVersion = rdLe16(data.constData() + 4);
    return true;
}

// ── 存储操作 ────────────────────────────────────────────────────────────────

bool XflashSession::sendEmi(const QByteArray& emi, QString* error)
{
    if (!xsend(XCmd::InitExtRam)) {
        if (error) *error = QStringLiteral("INIT_EXT_RAM 发送失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("INIT_EXT_RAM 状态非 0");
        return false;
    }
    QThread::msleep(10);
    if (!sendFlowPacket(leU32(static_cast<uint32_t>(emi.size())), kFrameTimeoutMs)) {
        if (error) *error = QStringLiteral("EMI 长度发送失败");
        return false;
    }
    if (!sendParam(emi)) {
        if (error) *error = QStringLiteral("EMI 数据被拒（DRAM 初始化失败）");
        return false;
    }
    emit logLine(QStringLiteral("success"), QStringLiteral("DRAM 参数（EMI）已被设备接受"));
    return true;
}

bool XflashSession::bootTo(uint32_t addr, const QByteArray& data, int settleMs, QString* error)
{
    if (!xsend(XCmd::BootTo)) {
        if (error) *error = QStringLiteral("BOOT_TO 发送失败");
        return false;
    }
    const qint64 s1 = status();
    if (s1 != 0) {
        if (error) *error = QStringLiteral("BOOT_TO 被拒：%1").arg(statusText(s1));
        return false;
    }

    // param = <QQ addr, len(da)
    QByteArray param(16, '\0');
    qToLittleEndian<quint64>(static_cast<quint64>(addr), param.data());
    qToLittleEndian<quint64>(static_cast<quint64>(data.size()), param.data() + 8);

    QByteArray header(12, '\0');
    qToLittleEndian<uint32_t>(kXflashMagic, header.data() + 0);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(XDataType::ProtocolFlow), header.data() + 4);
    qToLittleEndian<uint32_t>(static_cast<uint32_t>(param.size()), header.data() + 8);
    if (!m_tp->write(header) || !m_tp->write(param)) {
        if (error) *error = QStringLiteral("BOOT_TO 参数发送失败");
        return false;
    }
    if (!sendData(data)) {
        if (error) *error = QStringLiteral("BOOT_TO 镜像发送失败");
        return false;
    }

    if (settleMs > 0) {
        // 参考实现：sleep(timeout) 后再读状态（设备在引导 DA2）
        const int step = 100;
        for (int waited = 0; waited < settleMs; waited += step) {
            if (cancelled()) {
                if (error) *error = QStringLiteral("已取消");
                return false;
            }
            QThread::msleep(step);
        }
    }

    const qint64 s2 = status();
    if (s2 == 0 || static_cast<uint32_t>(s2) == kXflashSyncSignal) {
        emit logLine(QStringLiteral("success"),
                     QStringLiteral("DA2 引导成功（0x%1）").arg(addr, 8, 16, QLatin1Char('0')));
        return true;
    }
    if (error) {
        *error = QStringLiteral("DA2 引导失败：%1（addr=0x%2）")
                     .arg(statusText(s2))
                     .arg(addr, 8, 16, QLatin1Char('0'));
    }
    return false;
}

bool XflashSession::writeData(quint64 addr, const DaStorage& storage, uint32_t partType,
                              const QByteArray& data, QString* error)
{
    QBuffer source;
    source.setData(data);
    source.open(QIODevice::ReadOnly);
    return writeFromDevice(addr, static_cast<quint64>(data.size()), storage, partType, &source, error);
}

bool XflashSession::writeFromDevice(quint64 addr, quint64 size, const DaStorage& storage,
                                    uint32_t partType, QIODevice* source, QString* error)
{
    if (error) error->clear();
    if (!m_usable || !m_tp || !m_tp->isOpen()) {
        if (error) *error = QStringLiteral("DA 会话未打开或已失效，请重新连接");
        return false;
    }
    if (cancelled()) {
        if (error) *error = QStringLiteral("已取消");
        return false;
    }
    if (!source || !source->isReadable()
        || size > static_cast<quint64>(std::numeric_limits<qint64>::max()) - 511) {
        if (error) *error = QStringLiteral("源设备不可读或镜像长度溢出");
        return false;
    }
    const quint64 total = (size + 511) & ~quint64(511);
    if (!validStorageRange(addr, total, error)) return false;
    StorageTransfer transfer(m_usable);

    // ① WRITE_DATA 声明目标与长度
    if (!xsend(XCmd::WriteData)) {
        if (error) *error = QStringLiteral("WRITE_DATA 发送失败");
        return false;
    }
    const qint64 s1 = status();
    if (s1 != 0) {
        if (error) *error = QStringLiteral("WRITE_DATA 被拒：%1").arg(statusText(s1));
        return false;
    }
    if (!sendParam(storageParam(storage, partType, addr, total))) {
        if (error) *error = QStringLiteral("WRITE_DATA 参数被拒");
        return false;
    }

    // ② 分块送数据：每块 3 个 param（0 / 块校验和 / 数据）
    const int packet = static_cast<int>(qBound<uint32_t>(512, m_writePacket, kMaxPacket) / 512 * 512);
    quint64 pos = 0;
    while (pos < total) {
        if (cancelled()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        const int chunkSize = static_cast<int>(qMin<quint64>(packet, total - pos));
        const int sourceSize = static_cast<int>(qMin<quint64>(chunkSize, size - qMin(pos, size)));
        QByteArray chunk(chunkSize, '\0');
        int received = 0;
        while (received < sourceSize) {
            if (cancelled()) {
                if (error) *error = QStringLiteral("已取消");
                return false;
            }
            const qint64 n = source->read(chunk.data() + received, sourceSize - received);
            if (n <= 0 || n > sourceSize - received) {
                if (error) *error = QStringLiteral("源镜像读取不足或失败：%1").arg(source->errorString());
                return false;
            }
            received += static_cast<int>(n);
        }

        uint16_t checksum = 0;
        for (int i = 0; i < chunk.size(); ++i) {
            checksum = static_cast<uint16_t>((checksum + static_cast<uint8_t>(chunk.at(i))) & 0xFFFF);
        }

        const QVector<QByteArray> params{ leU32(0), leU32(checksum), chunk };
        if (!sendParam(params)) {
            if (error) {
                *error = QStringLiteral("写入 0x%1 处失败")
                             .arg(addr + pos, 0, 16);
            }
            return false;
        }
        pos += static_cast<quint64>(chunkSize);
        report(static_cast<qint64>(pos), static_cast<qint64>(total));
    }

    const qint64 s2 = status();
    if (s2 != 0) {
        if (error) *error = QStringLiteral("写入收尾状态异常：%1").arg(statusText(s2));
        return false;
    }
    // 保留原有下载动作时序，但不能在协议失败后继续复用会话。
    if (!sendDevCtrl(XCmd::CcOptionalDownloadAct) || cancelled()) {
        if (error) *error = cancelled() ? QStringLiteral("已取消")
                                       : QStringLiteral("写入下载动作收尾失败");
        return false;
    }
    transfer.complete = true;
    return true;
}

bool XflashSession::readData(quint64 addr, quint64 size, const DaStorage& storage, uint32_t partType,
                             QByteArray* out, QString* error)
{
    if (out) out->clear();
    // 旧接口需要连续内存；大分区必须走 readToDevice。
    if (!out || size > static_cast<quint64>(std::numeric_limits<int>::max()) - 512) {
        if (error) *error = QStringLiteral("QByteArray 读回目标无效或过大，请使用流式接口");
        return false;
    }
    QByteArray buffer;
    QBuffer destination(&buffer);
    destination.open(QIODevice::WriteOnly);
    if (!readToDevice(addr, size, storage, partType, &destination, error)) return false;
    *out = std::move(buffer);
    return true;
}

bool XflashSession::readToDevice(quint64 addr, quint64 size, const DaStorage& storage,
                                 uint32_t partType, QIODevice* destination, QString* error)
{
    if (error) error->clear();
    if (!m_usable || !m_tp || !m_tp->isOpen()) {
        if (error) *error = QStringLiteral("DA 会话未打开或已失效，请重新连接");
        return false;
    }
    if (cancelled()) {
        if (error) *error = QStringLiteral("已取消");
        return false;
    }
    if (!destination || !destination->isWritable()) {
        if (error) *error = QStringLiteral("读回目标设备不可写");
        return false;
    }
    if (!validStorageRange(addr, size, error)) return false;
    StorageTransfer transfer(m_usable);

    if (!xsend(XCmd::ReadData)) {
        if (error) *error = QStringLiteral("READ_DATA 发送失败");
        return false;
    }
    const qint64 s1 = status();
    if (s1 != 0) {
        if (error) *error = QStringLiteral("READ_DATA 被拒：%1").arg(statusText(s1));
        return false;
    }
    if (!sendParam(storageParam(storage, partType, addr, size))) {
        if (error) *error = QStringLiteral("READ_DATA 参数被拒");
        return false;
    }
    const qint64 s2 = status();
    if (s2 != 0) {
        if (error) *error = QStringLiteral("READ_DATA 就绪状态异常：%1").arg(statusText(s2));
        return false;
    }

    const uint32_t packet = qBound<uint32_t>(512, m_readPacket, kMaxPacket);
    quint64 remaining = size;

    while (remaining > 0) {
        if (cancelled()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }

        const QByteArray header = m_tp->readExact(12, kFrameTimeoutMs);
        if (header.size() != 12) {
            if (error) *error = QStringLiteral("读回数据包头超时");
            return false;
        }
        if (rdLe32(header.constData()) != kXflashMagic) {
            if (error) {
                *error = QStringLiteral("读回数据包 magic 不对（0x%1）")
                             .arg(rdLe32(header.constData()), 8, 16, QLatin1Char('0'));
            }
            return false;
        }
        const uint32_t payloadLen = rdLe32(header.constData() + 8);

        // 长度 2/4 是状态而不是数据；提前状态（即使为 0）不能算成功。
        if (payloadLen <= 4 || payloadLen > kMaxPacket || payloadLen > remaining) {
            if (error) *error = QStringLiteral("读回包长度异常或提前结束（%1，尚需 %2）")
                                    .arg(payloadLen).arg(remaining);
            return false;
        }
        uint32_t received = 0;
        while (received < payloadLen) {
            if (cancelled()) {
                if (error) *error = QStringLiteral("已取消");
                return false;
            }
            const int want = static_cast<int>(qMin(packet, payloadLen - received));
            const QByteArray part = m_tp->readExact(want, kDataTimeoutMs);
            if (part.size() != want) {
                if (error) *error = QStringLiteral("读回数据不完整（%1/%2）").arg(part.size()).arg(want);
                return false;
            }
            if (cancelled()) {
                if (error) *error = QStringLiteral("已取消");
                return false;
            }
            if (destination->write(part) != part.size()) {
                if (error) *error = QStringLiteral("读回目标短写或写入失败：%1")
                                        .arg(destination->errorString());
                return false;
            }
            received += static_cast<uint32_t>(want);
            remaining -= static_cast<quint64>(want);
            report(static_cast<qint64>(size - remaining), static_cast<qint64>(size));
        }
        // 本块确认（不读状态，参考实现 rstatus=False）。
        if (!sendFlowPacket(leU32(0), kFrameTimeoutMs)) {
            if (error) *error = QStringLiteral("读回确认发送失败");
            return false;
        }
    }

    // 恰好收齐数据仍须收到完整的成功状态；额外数据不能冒充尾状态。
    if (cancelled()) {
        if (error) *error = QStringLiteral("已取消");
        return false;
    }
    const QByteArray tailHeader = m_tp->readExact(12, kFrameTimeoutMs);
    if (tailHeader.size() != 12 || rdLe32(tailHeader.constData()) != kXflashMagic) {
        if (error) *error = QStringLiteral("读取收尾状态包头丢失或无效");
        return false;
    }
    const uint32_t tailLen = rdLe32(tailHeader.constData() + 8);
    if (tailLen != 2 && tailLen != 4) {
        if (error) *error = QStringLiteral("读取收尾状态长度异常（%1）").arg(tailLen);
        return false;
    }
    const QByteArray tail = m_tp->readExact(static_cast<int>(tailLen), kFrameTimeoutMs);
    if (tail.size() != static_cast<int>(tailLen)
        || (tailLen == 2 ? rdLe16(tail.constData()) : rdLe32(tail.constData())) != 0
        || cancelled()) {
        if (error) *error = cancelled() ? QStringLiteral("已取消")
            : QStringLiteral("读取收尾状态不完整或非 0");
        return false;
    }
    transfer.complete = true;
    return true;
}

bool XflashSession::format(quint64 addr, quint64 size, const DaStorage& storage, uint32_t partType,
                           QString* error)
{
    if (error) error->clear();
    if (!m_usable || !m_tp || !m_tp->isOpen()) {
        if (error) *error = QStringLiteral("DA 会话未打开或已失效，请重新连接");
        return false;
    }
    if (cancelled()) {
        if (error) *error = QStringLiteral("已取消");
        return false;
    }
    if (!validStorageRange(addr, size, error)) return false;
    StorageTransfer transfer(m_usable);
    if (!xsend(XCmd::Format)) {
        if (error) *error = QStringLiteral("FORMAT 发送失败");
        return false;
    }
    qint64 st = status();
    if (st != 0) {
        if (error) *error = QStringLiteral("FORMAT 被拒：%1").arg(statusText(st));
        return false;
    }
    if (!sendParam(storageParam(storage, partType, addr, size))) {
        if (error) *error = QStringLiteral("FORMAT 参数被拒");
        return false;
    }

    st = status();
    int rounds = 0;
    while (static_cast<uint32_t>(st) == kXflashStatusContinue && rounds < kMaxContinueRounds) {
        ++rounds;
        if (cancelled()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        // 设备给的下一步等待毫秒数
        const qint64 delay = status();
        if (delay < 0 || delay >= 60000) {
            if (error) *error = QStringLiteral("FORMAT 等待时间状态异常");
            return false;
        }
        for (qint64 waited = 0; waited < delay; ) {
            if (cancelled()) {
                if (error) *error = QStringLiteral("已取消");
                return false;
            }
            const qint64 step = qMin<qint64>(100, delay - waited);
            QThread::msleep(static_cast<unsigned long>(step));
            waited += step;
        }
        if (!ack()) {
            if (error) *error = QStringLiteral("FORMAT 进度确认失败");
            return false;
        }
        st = status();
    }
    if (static_cast<uint32_t>(st) == kXflashStatusComplete && !cancelled()) {
        report(static_cast<qint64>(size), static_cast<qint64>(size));
        if (cancelled()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        transfer.complete = true;
        emit logLine(QStringLiteral("success"),
                     QStringLiteral("擦除完成（0x%1，%2 字节）").arg(addr, 8, 16, QLatin1Char('0')).arg(size));
        return true;
    }
    if (error) *error = QStringLiteral("FORMAT 未完成：%1").arg(statusText(st));
    return false;
}

bool XflashSession::shutdown(QString* error)
{
    if (!xsend(XCmd::Shutdown)) {
        if (error) *error = QStringLiteral("SHUTDOWN 发送失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("SHUTDOWN 状态非 0");
        return false;
    }
    // <IIIIIIII hasflags, enablewdt, async_mode, bootmode, dl_bit, dont_resetrtc, leaveusb, 0
    QByteArray param(32, '\0');
    qToLittleEndian<uint32_t>(0, param.data() + 0); // hasflags=0（全默认）
    qToLittleEndian<uint32_t>(0, param.data() + 4); // enablewdt=0
    if (!sendFlowPacket(param, kFrameTimeoutMs)) {
        if (error) *error = QStringLiteral("SHUTDOWN 参数发送失败");
        return false;
    }
    if (status() != 0) {
        if (error) *error = QStringLiteral("SHUTDOWN 参数被拒");
        return false;
    }
    return true;
}

void XflashSession::report(qint64 done, qint64 total)
{
    if (m_progress) m_progress(done, total);
}

} // namespace kc::mtk
