#include "kc/mtk/MtkBrom.h"

#include "kc/mtk/MtkTransport.h"

#include <QThread>
#include <QtEndian>

namespace kc::mtk {
namespace {

/// 协议里所有多字节字段都是**大端**。
QByteArray be32(uint32_t value)
{
    QByteArray out(4, '\0');
    qToBigEndian<uint32_t>(value, out.data());
    return out;
}

uint16_t readBe16(const char* p) { return qFromBigEndian<uint16_t>(p); }
uint32_t readBe32(const char* p) { return qFromBigEndian<uint32_t>(p); }

// 超时预算：BROM 口是 CDC 链路，字节级等待都在毫秒量级。
constexpr int kEchoTimeoutMs = 250;    // 单个回显字节
constexpr int kCmdTimeoutMs = 800;     // 命令应答（参考实现里 sleep(0.05) 后即读）
constexpr int kStatusTimeoutMs = 1500; // 命令状态字
constexpr int kUploadTailTimeoutMs = 20000; // 上传大镜像后的 (checksum,status)

} // namespace

BromSession::BromSession(ITransport* transport, QObject* parent)
    : QObject(parent)
    , m_tp(transport)
    , m_owns(true)
{
}

BromSession::BromSession(ITransport* transport, Ownership ownership, QObject* parent)
    : QObject(parent)
    , m_tp(transport)
    , m_owns(ownership == Ownership::Own)
{
}

BromSession::~BromSession()
{
    if (m_owns) delete m_tp;
    m_tp = nullptr;
}

BromSession* BromSession::openSerial(const QString& comPort, QString* error)
{
    auto* tp = new SerialTransport(comPort);
    if (!tp->open()) {
        if (error) *error = QStringLiteral("打开 %1 失败：%2").arg(comPort, tp->lastError());
        delete tp;
        return nullptr;
    }
    auto* session = new BromSession(tp);
    emit session->logLine(QStringLiteral("info"),
                          QStringLiteral("BROM 口 %1 已打开（%2 8N1）")
                              .arg(comPort)
                              .arg(tp->baudRate()));
    return session;
}

void BromSession::replaceTransport(ITransport* transport)
{
    if (m_tp == transport) return;
    if (m_owns) delete m_tp;   // 借来的旧 transport 不归我们管
    m_tp = transport;
    m_owns = true;             // 新 transport 一律归本会话所有
}

bool BromSession::isOpen() const
{
    return m_tp && m_tp->isOpen();
}

// ── 线原语 ──────────────────────────────────────────────────────────────────

bool BromSession::echo(const QByteArray& data)
{
    if (!m_tp) return false;
    if (!m_tp->write(data)) return false;
    const QByteArray back = m_tp->readExact(data.size(), kEchoTimeoutMs);
    return back == data;
}

bool BromSession::echoU32(uint32_t value)
{
    return echo(be32(value));
}

bool BromSession::readWord(uint16_t* out, int timeoutMs)
{
    const QByteArray raw = m_tp->readExact(2, timeoutMs);
    if (raw.size() != 2) return false;
    if (out) *out = readBe16(raw.constData());
    return true;
}

bool BromSession::readDwords(int count, QVector<uint32_t>* out, int timeoutMs)
{
    if (count <= 0) return true;
    const QByteArray raw = m_tp->readExact(4 * count, timeoutMs);
    if (raw.size() != 4 * count) return false;
    if (out) {
        out->clear();
        out->reserve(count);
        for (int i = 0; i < count; ++i) {
            out->append(readBe32(raw.constData() + i * 4));
        }
    }
    return true;
}

// ── 握手 ────────────────────────────────────────────────────────────────────

bool BromSession::handshake(QString* error)
{
    if (!isOpen()) {
        if (error) *error = QStringLiteral("会话未打开");
        return false;
    }

    // 设备上电异步：口已存在但 BROM 还没准备好应答，所以整轮重试。
    constexpr int kAttempts = 20;
    for (int attempt = 1; attempt <= kAttempts; ++attempt) {
        if (cancelled()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }

        m_tp->clear(); // 丢掉上一轮的残字节，否则会被当成新一轮的回显

        int matched = 0;
        bool transportDead = false;
        while (matched < kBromHandshakeLen) {
            const char byte = static_cast<char>(kBromHandshakeBytes[matched]);
            if (!m_tp->write(QByteArray(1, byte))) {
                transportDead = true;
                break;
            }
            const QByteArray back = m_tp->readExact(1, kEchoTimeoutMs);
            if (back.size() == 1
                && static_cast<uint8_t>(back.at(0))
                       == static_cast<uint8_t>(~static_cast<uint8_t>(byte) & 0xFF)) {
                ++matched;
            } else if (back.isEmpty()) {
                // 一个字节都没回来 → 本轮到此为止（参考实现在这里是无限自旋）
                break;
            } else {
                // 回来但不是取反值 → 对齐错了，从第一个字节重来
                matched = 0;
            }
        }

        if (matched == kBromHandshakeLen) {
            emit logLine(QStringLiteral("success"),
                         QStringLiteral("BROM 握手成功（回显 %1，第 %2 次尝试）")
                             .arg(QStringLiteral("A0 0A 50 05"))
                             .arg(attempt));
            return true;
        }
        if (transportDead) {
            if (error) *error = QStringLiteral("写入失败：%1").arg(m_tp->lastError());
            return false;
        }
        QThread::msleep(50);
    }

    emit logLine(QStringLiteral("error"), QStringLiteral("BROM 握手超时（%1 轮无有效回显）").arg(kAttempts));
    if (error) {
        *error = QStringLiteral(
            "BROM 握手超时。请确认：① 设备已关机后按住音量键插入 USB 进入 BROM/9008 模式；"
            "② 已安装 MTK USB 驱动（设备管理器无感叹号）；③ 选择的端口号正确、且未被其他工具占用。");
    }
    return false;
}

// ── 芯片识别 ────────────────────────────────────────────────────────────────

bool BromSession::getHwCode(uint16_t* hwCode, QString* error)
{
    if (!isOpen()) {
        if (error) *error = QStringLiteral("会话未打开");
        return false;
    }

    // UsbCmd(0xFD, 4)：写命令 → 读回同字节 → 读 4 字节应答。
    if (!m_tp->write(QByteArray(1, static_cast<char>(kCmdGetHwCode)))) {
        if (error) *error = QStringLiteral("发送 GET_HW_CODE 失败：%1").arg(m_tp->lastError());
        return false;
    }
    const QByteArray ack = m_tp->readExact(1, kCmdTimeoutMs);
    if (ack.size() != 1 || static_cast<uint8_t>(ack.at(0)) != kCmdGetHwCode) {
        if (error) {
            *error = QStringLiteral("GET_HW_CODE 未被确认（回显 %1）")
                         .arg(ack.isEmpty() ? QStringLiteral("空") : QString::number(int(ack.at(0)), 16));
        }
        return false;
    }
    const QByteArray payload = m_tp->readExact(4, kCmdTimeoutMs);
    if (payload.size() != 4) {
        if (error) *error = QStringLiteral("GET_HW_CODE 应答长度不足（%1/4）").arg(payload.size());
        return false;
    }
    // 前 2 字节 = hw code，后 2 字节 = hw version（都是大端）
    const uint16_t code = readBe16(payload.constData());
    if (hwCode) *hwCode = code;
    emit logLine(QStringLiteral("info"),
                 QStringLiteral("HW code = 0x%1（hwver = 0x%2）")
                     .arg(code, 4, 16, QLatin1Char('0'))
                     .arg(readBe16(payload.constData() + 2), 4, 16, QLatin1Char('0')));
    return true;
}

// ── 内存读写（BROM 的 0xD1/0xD4 只作用于 RAM，不是存储分区）────────────────

bool BromSession::read32(uint32_t addr, int dwords, QVector<uint32_t>* out, QString* error)
{
    if (!isOpen()) {
        if (error) *error = QStringLiteral("会话未打开");
        return false;
    }
    if (dwords <= 0) {
        if (out) out->clear();
        return true;
    }

    if (!echo(QByteArray(1, static_cast<char>(kCmdRead32)))) {
        if (error) *error = QStringLiteral("READ32 命令未被回显");
        return false;
    }
    if (!echoU32(addr)) {
        if (error) *error = QStringLiteral("READ32 地址 0x%1 未被回显").arg(addr, 8, 16, QLatin1Char('0'));
        return false;
    }
    if (!echoU32(static_cast<uint32_t>(dwords))) {
        if (error) *error = QStringLiteral("READ32 长度 %1 未被回显").arg(dwords);
        return false;
    }

    uint16_t status = 0;
    if (!readWord(&status, kStatusTimeoutMs)) {
        if (error) *error = QStringLiteral("READ32 未收到状态字");
        return false;
    }
    if (status > 0xFF) {
        if (error) {
            *error = QStringLiteral("READ32 被拒（status=0x%1 @ 0x%2）")
                         .arg(status, 4, 16, QLatin1Char('0'))
                         .arg(addr, 8, 16, QLatin1Char('0'));
        }
        return false;
    }

    QVector<uint32_t> data;
    if (!readDwords(dwords, &data, kStatusTimeoutMs)) {
        if (error) *error = QStringLiteral("READ32 数据不完整");
        return false;
    }
    uint16_t tail = 0;
    if (!readWord(&tail, kStatusTimeoutMs) || tail > 0xFF) {
        if (error) *error = QStringLiteral("READ32 尾状态异常");
        return false;
    }
    if (out) *out = data;
    return true;
}

bool BromSession::write32(uint32_t addr, const QVector<uint32_t>& values, QString* error)
{
    if (!isOpen()) {
        if (error) *error = QStringLiteral("会话未打开");
        return false;
    }
    if (values.isEmpty()) return true;

    if (!echo(QByteArray(1, static_cast<char>(kCmdWrite32)))) {
        if (error) *error = QStringLiteral("WRITE32 命令未被回显");
        return false;
    }
    if (!echoU32(addr)) {
        if (error) *error = QStringLiteral("WRITE32 地址 0x%1 未被回显").arg(addr, 8, 16, QLatin1Char('0'));
        return false;
    }
    if (!echoU32(static_cast<uint32_t>(values.size()))) {
        if (error) *error = QStringLiteral("WRITE32 长度 %1 未被回显").arg(values.size());
        return false;
    }

    uint16_t status = 0;
    if (!readWord(&status, kStatusTimeoutMs)) {
        if (error) *error = QStringLiteral("WRITE32 未收到状态字");
        return false;
    }
    if (status > 0xFF) {
        if (error) {
            *error = QStringLiteral("WRITE32 被拒（status=0x%1 @ 0x%2）")
                         .arg(status, 4, 16, QLatin1Char('0'))
                         .arg(addr, 8, 16, QLatin1Char('0'));
        }
        return false;
    }
    // 参考实现：只有 status <= 3 才继续发数据（>3 视为设备拒绝但不报错）
    if (status <= 3) {
        for (const uint32_t value : values) {
            if (!echoU32(value)) {
                if (error) *error = QStringLiteral("WRITE32 数据字未被回显");
                return false;
            }
        }
        uint16_t tail = 0;
        if (!readWord(&tail, kStatusTimeoutMs)) {
            if (error) *error = QStringLiteral("WRITE32 未收到尾状态");
            return false;
        }
        if (tail > 0xFF) {
            if (error) {
                *error = QStringLiteral("WRITE32 尾状态异常（0x%1）")
                             .arg(tail, 4, 16, QLatin1Char('0'));
            }
            return false;
        }
    }
    return true;
}

// ── DA 上传 ────────────────────────────────────────────────────────────────

uint16_t BromSession::daChecksum(const QByteArray& data)
{
    uint16_t checksum = 0;
    int pos = 0;
    while (pos + 2 <= data.size()) {
        checksum ^= qFromLittleEndian<uint16_t>(data.constData() + pos);
        pos += 2;
    }
    if (pos < data.size()) checksum ^= static_cast<uint8_t>(data.at(pos));
    return checksum;
}

bool BromSession::uploadData(const QByteArray& data)
{
    const int maxPacket = qMax(1, m_tp->maxWritePacket());
    int pos = 0;
    while (pos < data.size()) {
        if (cancelled()) return false;
        const int size = qMin(maxPacket, data.size() - pos);
        if (!m_tp->write(data.mid(pos, size))) return false;
        pos += size;
        if (pos % 0x2000 == 0) {
            // 每 32KB 插一个 0 长度写，给设备侧的接收窗口一个边界（参考实现同）。
            if (!m_tp->write(QByteArray())) return false;
        }
    }
    if (!m_tp->write(QByteArray())) return false;
    QThread::msleep(120);
    return true;
}

bool BromSession::sendDa(uint32_t addr, const QByteArray& data, uint32_t sigLen, QString* error)
{
    if (!isOpen()) {
        if (error) *error = QStringLiteral("会话未打开");
        return false;
    }

    QByteArray payload = data;
    if (payload.size() % 2 != 0) payload.append('\0');
    const uint16_t expected = daChecksum(payload);

    if (!echo(QByteArray(1, static_cast<char>(kCmdSendDa)))) {
        if (error) *error = QStringLiteral("SEND_DA 命令未被回显");
        return false;
    }
    if (!echoU32(addr)) {
        if (error) *error = QStringLiteral("SEND_DA 地址 0x%1 未被回显").arg(addr, 8, 16, QLatin1Char('0'));
        return false;
    }
    if (!echoU32(static_cast<uint32_t>(payload.size()))) {
        if (error) *error = QStringLiteral("SEND_DA 长度 %1 未被回显").arg(payload.size());
        return false;
    }
    if (!echoU32(sigLen)) {
        if (error) *error = QStringLiteral("SEND_DA 签名长度 %1 未被回显").arg(sigLen);
        return false;
    }

    uint16_t status = 0;
    if (!readWord(&status, kStatusTimeoutMs)) {
        if (error) *error = QStringLiteral("SEND_DA 未收到状态字");
        return false;
    }
    if (status == kStatusSlaRequired) {
        if (error) {
            *error = QStringLiteral(
                "设备要求 SLA（0x1D0D）：该 DA 需要 Secure Legacy Auth 签名校验，"
                "当前版本不实现 SLA，无法继续。请使用与设备匹配且已解锁的固件包，或换用官方工具。");
        }
        emit logLine(QStringLiteral("error"), QStringLiteral("SEND_DA 返回 0x1D0D —— 设备要求 SLA"));
        return false;
    }
    if (status > 0xFF) {
        if (error) {
            *error = QStringLiteral("SEND_DA 被拒（status=0x%1）")
                         .arg(status, 4, 16, QLatin1Char('0'));
        }
        return false;
    }

    emit logLine(QStringLiteral("info"),
                 QStringLiteral("正在上传 DA1：%1 字节 @ 0x%2（块 %3B）")
                     .arg(payload.size())
                     .arg(addr, 8, 16, QLatin1Char('0'))
                     .arg(m_tp->maxWritePacket()));
    if (!uploadData(payload)) {
        if (error) *error = QStringLiteral("DA 数据上传失败（%1）").arg(m_tp->lastError());
        return false;
    }

    const QByteArray tail = m_tp->readExact(4, kUploadTailTimeoutMs);
    if (tail.size() != 4) {
        if (error) *error = QStringLiteral("DA 上传后未收到 (checksum,status)（收到 %1 字节）").arg(tail.size());
        return false;
    }
    const uint16_t gotChecksum = readBe16(tail.constData());
    const uint16_t gotStatus = readBe16(tail.constData() + 2);

    if (gotChecksum != expected && gotChecksum != 0) {
        // 参考实现把不匹配当警告（有些 DA 回 0 表示"不校验"）
        emit logLine(QStringLiteral("warning"),
                     QStringLiteral("DA 上传校验和不匹配（本地 0x%1 / 设备 0x%2），继续尝试跳转")
                         .arg(expected, 4, 16, QLatin1Char('0'))
                         .arg(gotChecksum, 4, 16, QLatin1Char('0')));
    }
    if (gotStatus > 0xFF) {
        if (error) {
            *error = QStringLiteral("DA 上传被设备拒绝（status=0x%1）")
                         .arg(gotStatus, 4, 16, QLatin1Char('0'));
        }
        return false;
    }
    emit logLine(QStringLiteral("success"),
                 QStringLiteral("DA1 上传完成（校验和 0x%1）")
                     .arg(gotChecksum, 4, 16, QLatin1Char('0')));
    return true;
}

/// JUMP_DA 的公共流程（32/64 位只差一个 echo(b"\x01")）：
///   echo(cmd) → 写 addr（注意：**不是 echo**）→ rdword 必须回显 addr
///   → [64 位：echo(b"\x01")] → rword 状态必须为 0 → sleep(100ms)
bool BromSession::jumpDa(uint32_t addr, QString* error)
{
    return jumpDaInternal(addr, error, /*is64=*/false);
}

bool BromSession::jumpDa64(uint32_t addr, QString* error)
{
    return jumpDaInternal(addr, error, /*is64=*/true);
}

bool BromSession::jumpDaInternal(uint32_t addr, QString* error, bool is64)
{
    if (!isOpen()) {
        if (error) *error = QStringLiteral("会话未打开");
        return false;
    }
    const uint8_t cmd = is64 ? kCmdJumpDa64 : kCmdJumpDa;
    const QString name = is64 ? QStringLiteral("JUMP_DA64") : QStringLiteral("JUMP_DA");

    if (!echo(QByteArray(1, static_cast<char>(cmd)))) {
        if (error) *error = QStringLiteral("%1 命令未被回显").arg(name);
        return false;
    }
    if (!m_tp->write(be32(addr))) {
        if (error) *error = QStringLiteral("%1 地址写入失败").arg(name);
        return false;
    }

    QVector<uint32_t> resAddr;
    if (!readDwords(1, &resAddr, kStatusTimeoutMs)) {
        if (error) *error = QStringLiteral("%1 未收到地址回显").arg(name);
        return false;
    }
    if (resAddr.value(0) != addr) {
        if (error) {
            *error = QStringLiteral("%1 地址回显不符（发 0x%2 / 回 0x%3）")
                         .arg(name)
                         .arg(addr, 8, 16, QLatin1Char('0'))
                         .arg(resAddr.value(0), 8, 16, QLatin1Char('0'));
        }
        return false;
    }

    if (is64 && !echo(QByteArray(1, '\x01'))) {
        if (error) *error = QStringLiteral("JUMP_DA64 的 64 位标志未被回显");
        return false;
    }

    uint16_t status = 0;
    if (!readWord(&status, kStatusTimeoutMs)) {
        if (error) *error = QStringLiteral("%1 未收到状态字").arg(name);
        return false;
    }
    // 参考实现：读状态之后才 sleep（不是 sleep 之后再读）
    QThread::msleep(100);
    if (status != 0) {
        if (error) {
            *error = QStringLiteral("%1 返回失败状态 0x%2").arg(name).arg(status, 4, 16, QLatin1Char('0'));
        }
        return false;
    }
    emit logLine(QStringLiteral("success"),
                 QStringLiteral("已跳转到 DA @ 0x%1，等待 DA 就绪")
                     .arg(addr, 8, 16, QLatin1Char('0')));
    return true;
}

} // namespace kc::mtk
