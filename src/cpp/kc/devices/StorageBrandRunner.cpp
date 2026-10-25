#include "kc/devices/StorageBrandRunner.h"

#include "protocol/Firehose.h"
#include "protocol/Sahara.h"
#include "transport/SerialTransport.h"

#include <QFileInfo>
#include <QThread>
#include <QtEndian>

#include <atomic>
#include <functional>

namespace kc::devices {
namespace {

using swt::edl::DeviceInfo;
using swt::edl::FirehoseClient;
using swt::edl::LogCallback;
using swt::edl::ProgressCallback;
using swt::edl::SaharaClient;
using swt::edl::SerialTransport;
using swt::edl::StorageType;

struct Outcome {
    bool ok = false;
    QString error;
    kc::devices::StorageBrandResult result;
};

/// 探测本体（后台线程；禁止碰任何控件）。
///
/// 步骤与 MiUfsProbe 对齐，只是把「睡 1s 再发 nop」换成「先判模式」：
///   1. peek 8 字节：0x01 = Sahara HELLO，0x04 = 引导已传完等 DONE；
///   2. 没传过引导 → Sahara 上传（prog_*firehose*.elf / xbl_*devprg*.melf）；
///   3. 发 `<nop>`，解析回包里的 `INFO: ufs|emmc: <vendor>` 与 `Chip serial num`；
///   4. 解析不到 → getstorageinfo / configure 兜底（部分 loader 只在配置后打印）。
Outcome runProbe(const QString& portName, const QString& programmerPath,
                      std::atomic_bool& cancelled,
                      const std::function<void(const QString&)>& log)
{
    using namespace swt::edl;

    Outcome outcome;
    if (portName.isEmpty()) {
        outcome.error = QStringLiteral("请选择 9008 串口");
        return outcome;
    }
    if (programmerPath.isEmpty() || !QFileInfo::exists(programmerPath)) {
        outcome.error = QStringLiteral("请选择有效的 Firehose 引导文件");
        return outcome;
    }

    SerialTransport transport(&cancelled);
    QString error;
    if (!transport.open(portName, &error)) {
        outcome.error = error;
        return outcome;
    }
    if (log) log(QStringLiteral("已打开 %1：%2").arg(portName, transport.openDiagnostics()));

    const LogCallback logLine = [&log](const QString& line) {
        if (log) log(line);
    };
    const ProgressCallback noProgress = [](quint64, quint64) {};

    // ── ① 设备当前在哪一步 ────────────────────────────────────────────────
    bool saharaHello = false;
    bool pendingTransfer = false;
    QByteArray header;
    QString probeError;
    if (transport.peekExactly(8, &header, 400, &probeError)) {
        const auto* bytes = reinterpret_cast<const uchar*>(header.constData());
        const quint32 command = qFromLittleEndian<quint32>(bytes);
        const quint32 length = qFromLittleEndian<quint32>(bytes + 4);
        saharaHello = command == 0x01 && length >= 48 && length <= 1024 * 1024;
        pendingTransfer = command == 0x04 && length >= 16 && length <= 1024 * 1024;
    }

    bool programmerLoaded = false;
    if (pendingTransfer) {
        logLine(QStringLiteral("设备处于 Sahara「已传完引导」状态，补发 DONE"));
        SaharaClient sahara(transport, cancelled, logLine, noProgress);
        programmerLoaded = sahara.finishPendingTransfer(&error);
        if (!programmerLoaded) {
            // 和 EdlWorker 同一条逃生口：DONE 失败会把设备卡在等主机确认的状态，
            // 只能重开端口，走完整 Sahara 引导。
            logLine(QStringLiteral("DONE 确认失败：%1 —— 重开端口走完整引导流程").arg(error));
            transport.close();
            QThread::msleep(200);
            if (!transport.open(portName, &error)) {
                outcome.error = error;
                return outcome;
            }
        }
    }
    if (saharaHello) logLine(QStringLiteral("检测到 Sahara HELLO，设备等待发送引导"));

    std::unique_ptr<FirehoseClient> firehose;
    const auto ensureFirehose = [&]() -> FirehoseClient& {
        if (!firehose) {
            firehose = std::make_unique<FirehoseClient>(transport, cancelled, logLine, noProgress);
        }
        return *firehose;
    };

    // 已经进 Firehose 的设备再传一遍引导会打断会话 —— 先 NOP 探一下。
    bool inFirehose = false;
    if (!programmerLoaded) {
        QString nopError;
        inFirehose = ensureFirehose().ping(&nopError, 1200);
        if (inFirehose) logLine(QStringLiteral("设备已处于 Firehose 模式，跳过引导传输"));
    }

    if (!programmerLoaded && !inFirehose) {
        if (cancelled.load()) {
            outcome.error = QStringLiteral("已停止");
            return outcome;
        }
        logLine(QStringLiteral("开始上传 Firehose 引导：%1")
                    .arg(QFileInfo(programmerPath).fileName()));
        SaharaClient sahara(transport, cancelled, logLine, noProgress);
        DeviceInfo identity;
        if (!sahara.uploadProgrammer(programmerPath, &identity, false, &error)) {
            outcome.error = QStringLiteral("引导上传失败：%1").arg(error);
            return outcome;
        }
        logLine(QStringLiteral("引导已传输，等待 loader 初始化…"));
        firehose.reset();   // 新会话：上一次 NOP 的解析状态作废
        QThread::msleep(900);   // MiUfsProbe 在这里睡 1s，loader 起来后才打存储行
    }

    FirehoseClient& client = ensureFirehose();

    // ── ② NOP 取字库行 ───────────────────────────────────────────────────
    QString pingError;
    const bool ack = client.ping(&pingError, 6000);
    const auto harvest = [&]() {
        outcome.result.storageType = client.detectedStorageType().toUpper();
        outcome.result.vendor = client.detectedStorageVendor();
        outcome.result.chipSerial = client.detectedChipSerial();
        if (!outcome.result.vendor.isEmpty()) {
            outcome.result.rawLine = QStringLiteral("INFO: %1: %2")
                                         .arg(outcome.result.storageType.toLower(),
                                              outcome.result.vendor);
        }
    };
    harvest();
    if (ack) logLine(QStringLiteral("NOP 已回 ACK"));

    // ── ③ 兜底：有些 loader 要 configure 之后才打厂商行 ───────────────────
    if (outcome.result.vendor.isEmpty() && !cancelled.load()) {
        logLine(QStringLiteral("NOP 回包未带厂商行，改发 getstorageinfo 再取一次…"));
        QString storageError;
        quint64 storageBytes = 0;
        if (!client.getStorageInfo(0, &storageBytes, &storageError)) {
            logLine(QStringLiteral("getstorageinfo 失败：%1").arg(storageError));
            QString configError;
            if (!client.configure(StorageType::Ufs, &configError)) {
                logLine(QStringLiteral("UFS configure 失败：%1").arg(configError));
                if (client.configure(StorageType::Emmc, &configError)) {
                    logLine(QStringLiteral("按 eMMC 重新 configure 成功"));
                }
            }
            client.getStorageInfo(0, &storageBytes, &storageError);
        }
        harvest();
    }

    if (cancelled.load()) {
        outcome.error = QStringLiteral("已停止");
        return outcome;
    }
    if (outcome.result.vendor.isEmpty()) {
        outcome.error = ack
            ? QStringLiteral("未解析到字库厂商：loader 没有打印 INFO: ufs/emmc，"
                             "多半是引导文件与机型不匹配")
            : QStringLiteral("Firehose 无响应：%1").arg(pingError);
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

} // namespace

StorageBrandProbeOutcome runStorageBrandProbe(const QString& portName,
                                              const QString& programmerPath,
                                              std::atomic_bool& cancelled,
                                              const std::function<void(const QString&)>& log)
{
    const Outcome r = runProbe(portName, programmerPath, cancelled, log);
    StorageBrandProbeOutcome out;
    out.ok = r.ok;
    out.error = r.error;
    out.result = r.result;
    return out;
}

} // namespace kc::devices
