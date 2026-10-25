#include "kc/mtk/MtkDeviceSession.h"

#include "kc/mtk/MtkBrom.h"
#include "kc/mtk/MtkDaFile.h"
#include "kc/mtk/MtkDevice.h"
#include "kc/mtk/MtkTransport.h"
#include "kc/mtk/MtkXflash.h"

#include <QFile>
#include <QThread>
#include <QtEndian>

namespace kc::mtk {
namespace {

// EMI 抽取实现已移到 MtkDaFile.cpp（extractEmiFromPreloader）——
// 那里不依赖 win32 设备层，协议单测可以直接覆盖它。

struct BootAttempt {
    XflashSession*& xflash;
    BromSession*& brom;
    bool complete = false;
    BootAttempt(XflashSession*& xf, BromSession*& br) : xflash(xf), brom(br) {}
    ~BootAttempt()
    {
        if (complete) return;
        delete xflash;
        xflash = nullptr;
        delete brom;
        brom = nullptr;
    }
};

} // namespace

MtkDeviceSession::MtkDeviceSession(QObject* parent)
    : QObject(parent)
{
}

MtkDeviceSession::~MtkDeviceSession()
{
    // 先销毁借用 transport 的 XFlash，再释放拥有 transport 的 BROM。
    delete m_xflash;
    m_xflash = nullptr;
    delete m_brom;
    m_brom = nullptr;
}

bool MtkDeviceSession::daReady() const
{
    return m_daReady && m_xflash && m_xflash->isUsable();
}

bool MtkDeviceSession::cancelled() const
{
    return m_cancel && m_cancel();
}

void MtkDeviceSession::log(const QString& level, const QString& message)
{
    emit logLine(level, message);
}

void MtkDeviceSession::setCancelFn(CancelFn fn)
{
    m_cancel = std::move(fn);
    if (m_brom) m_brom->setCancelFn(m_cancel);
    if (m_xflash) m_xflash->setCancelFn(m_cancel);
}

void MtkDeviceSession::setProgressFn(ProgressFn fn)
{
    m_progress = std::move(fn);
    // XFlash 的回调始终同时发送 progressChanged 和调用当前 m_progress。
}

MtkDeviceSession* MtkDeviceSession::openBrom(const QString& comPort, QString* error, CancelFn cancel)
{
    if (cancel && cancel()) {
        if (error) *error = QStringLiteral("已取消");
        return nullptr;
    }
    auto* tp = new SerialTransport(comPort);
    if (!tp->open()) {
        if (error) *error = QStringLiteral("打开 %1 失败：%2").arg(comPort, tp->lastError());
        delete tp;
        return nullptr;
    }

    auto* session = new MtkDeviceSession();
    session->m_comPort = comPort;
    session->m_brom = new BromSession(tp, session);
    session->setCancelFn(std::move(cancel));
    QObject::connect(session->m_brom, &BromSession::logLine, session, &MtkDeviceSession::logLine);

    if (!session->m_brom->handshake(error) || session->cancelled()) {
        if (session->cancelled() && error) *error = QStringLiteral("已取消");
        delete session;
        return nullptr;
    }

    uint16_t hwCode = 0;
    if (!session->m_brom->getHwCode(&hwCode, error) || session->cancelled()) {
        if (session->cancelled() && error) *error = QStringLiteral("已取消");
        delete session;
        return nullptr;
    }
    session->m_hwCode = hwCode;
    return session;
}

QByteArray MtkDeviceSession::extractEmi(const QByteArray& preloader)
{
    return extractEmiFromPreloader(preloader);
}

bool MtkDeviceSession::reopenDaTransport(QString* error)
{
    // JUMP_DA 之后设备通常以 PID 0x2001 重新枚举，COM 口号会变。
    // 轮询至多约 3 秒；没等到就沿用原端口（部分机型不换口，实测只能靠真机确认）。
    const QString oldPort = m_comPort;
    MtkUsbDevice daDevice;
    bool found = false;
    for (int i = 0; i < 20; ++i) {
        if (cancelled()) {
            if (error) *error = QStringLiteral("已取消");
            return false;
        }
        if (findMtkDevice(DeviceStage::Da, &daDevice) && !daDevice.comPort.isEmpty()
            && daDevice.comPort.compare(oldPort, Qt::CaseInsensitive) != 0) {
            found = true;
            break;
        }
        QThread::msleep(150);
    }

    if (!found) {
        log(QStringLiteral("info"),
            QStringLiteral("未检测到新的 DA 端口，沿用 %1（部分机型跳转后不换口）").arg(oldPort));
        return true;
    }

    auto* tp = new SerialTransport(daDevice.comPort);
    if (!tp->open()) {
        if (error) {
            *error = QStringLiteral("打开 DA 口 %1 失败：%2").arg(daDevice.comPort, tp->lastError());
        }
        delete tp;
        return false;
    }
    m_brom->replaceTransport(tp);
    m_comPort = daDevice.comPort;
    log(QStringLiteral("success"),
        QStringLiteral("设备已切到 DA 口 %1（PID 0x%2）")
            .arg(daDevice.comPort)
            .arg(daDevice.pid, 4, 16, QLatin1Char('0')));
    return true;
}

bool MtkDeviceSession::bootDa(const QString& daPath, const QString& preloaderPath, QString* error)
{
    if (!m_brom) {
        if (error) *error = QStringLiteral("BROM 会话不存在");
        return false;
    }
    if (cancelled()) {
        if (error) *error = QStringLiteral("已取消");
        return false;
    }
    if (daReady()) return true;
    if (m_xflash) {
        if (error) *error = QStringLiteral("DA 会话已失效，请重新连接");
        return false;
    }

    // ① 解析 DA 文件
    QVector<DaEntry> entries;
    QString parseError;
    if (!DaFile::parse(daPath, &entries, &parseError)) {
        if (error) *error = QStringLiteral("解析 DA 文件失败：%1").arg(parseError);
        return false;
    }
    QString why;
    const DaEntry* entry = DaFile::pickForHwCode(entries, m_hwCode, &why);
    if (!entry) {
        if (error) *error = QStringLiteral("找不到匹配的 DA 条目：%1").arg(why);
        return false;
    }
    const DaRegion* da1 = entry->da1();
    const DaRegion* da2 = entry->da2();
    if (!da1 || !da2) {
        if (error) *error = QStringLiteral("DA 条目缺少 region[1]/region[2]（DA1/DA2）");
        return false;
    }
    log(QStringLiteral("info"),
        QStringLiteral("选用 DA 条目 hw=0x%1 hwver=0x%2 swver=0x%3（DA1 @0x%4 / DA2 @0x%5）%6")
            .arg(entry->hwCode, 4, 16, QLatin1Char('0'))
            .arg(entry->hwVersion, 4, 16, QLatin1Char('0'))
            .arg(entry->swVersion, 4, 16, QLatin1Char('0'))
            .arg(da1->startAddr, 8, 16, QLatin1Char('0'))
            .arg(da2->startAddr, 8, 16, QLatin1Char('0'))
            .arg(entry->v6 ? QStringLiteral(" [v6]") : QString()));

    // ② 读 DA1 并上传 + 跳转
    QByteArray da1Data;
    QString regionError;
    if (!DaFile::readRegion(daPath, *da1, &da1Data, &regionError)) {
        if (error) *error = QStringLiteral("读取 DA1 失败：%1").arg(regionError);
        return false;
    }
    BootAttempt attempt(m_xflash, m_brom);
    if (!m_brom->sendDa(da1->startAddr, da1Data, da1->sigLen, error)) return false;
    if (!m_brom->jumpDa(da1->startAddr, error)) return false;

    // ③ 换到 DA 口
    if (!reopenDaTransport(error)) return false;

    // ④ DA2 握手
    m_xflash = new XflashSession(m_brom->transport(), this);
    m_xflash->setCancelFn(m_cancel);
    m_xflash->setProgressFn([this](qint64 done, qint64 total) {
        emit progressChanged(done, total);
        if (m_progress) m_progress(done, total);
    });
    QObject::connect(m_xflash, &XflashSession::logLine, this, &MtkDeviceSession::logLine);

    if (!m_xflash->waitDaReady(error)) return false;
    if (!m_xflash->handshakeStage2(error)) return false;

    // ⑤ connection agent：BROM 起 DA 时需要 DRAM 参数
    QString agent;
    if (m_xflash->getConnectionAgent(&agent, error)) {
        log(QStringLiteral("info"), QStringLiteral("DA 连接代理：%1").arg(agent.isEmpty() ? QStringLiteral("(空)") : agent));
        if (agent == QStringLiteral("brom")) {
            QByteArray emi;
            if (!preloaderPath.isEmpty()) {
                QFile file(preloaderPath);
                if (file.open(QIODevice::ReadOnly)) {
                    emi = extractEmiFromPreloader(file.readAll());
                    file.close();
                }
            }
            if (!emi.isEmpty()) {
                if (!m_xflash->sendEmi(emi, error)) return false;
            } else {
                log(QStringLiteral("warning"),
                    QStringLiteral("该平台需要 EMI（DRAM 参数），但没从 preloader 里抠出来 —— "
                                   "请在 scatter 里为 PRELOADER 分区指上正确的 preloader 镜像后重试"));
            }
        }
    } else {
        return false;
    }

    // ⑥ 读 DA2 并 BOOT_TO（尾部署名要去掉，参考实现 daconfig.da2 同口径）
    QByteArray da2Data;
    if (!DaFile::readRegion(daPath, *da2, &da2Data, &regionError)) {
        if (error) *error = QStringLiteral("读取 DA2 失败：%1").arg(regionError);
        return false;
    }
    if (da2->sigLen > 0 && static_cast<quint64>(da2->sigLen) < static_cast<quint64>(da2Data.size())) {
        da2Data.chop(static_cast<int>(da2->sigLen));
    }
    log(QStringLiteral("info"),
        QStringLiteral("正在引导 DA2（%1 字节 → 0x%2）").arg(da2Data.size()).arg(da2->startAddr, 8, 16, QLatin1Char('0')));
    if (!m_xflash->bootTo(da2->startAddr, da2Data, 500, error)) return false;

    // ⑦ SLA 状态
    bool sla = false;
    if (m_xflash->slaEnabledStatus(&sla, nullptr) && sla) {
        log(QStringLiteral("warning"),
            QStringLiteral("设备报告 DA SLA 已启用 —— 本实现不执行 SLA 签名握手，"
                           "若后续读写返回 Security 域错误，即为此因，需换用已解锁/免 SLA 的 DA"));
    }

    if (!m_xflash->setResetKey(0x68, error)) return false;     // 默认复位键
    if (!m_xflash->setChecksumLevel(0x0, error)) return false; // 与原实现相同的校验级别

    QString daVersion;
    if (!m_xflash->getDaVersion(&daVersion, error)) return false;
    if (!daVersion.isEmpty()) {
        log(QStringLiteral("info"), QStringLiteral("DA 版本：%1").arg(daVersion));
    }
    if (cancelled()) {
        if (error) *error = QStringLiteral("已取消");
        return false;
    }
    m_daReady = true;
    attempt.complete = true;
    log(QStringLiteral("success"), QStringLiteral("DA 已就绪，可以开始刷写 / 读回"));
    return true;
}

} // namespace kc::mtk
