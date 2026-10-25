#include "app/EdlWorker.h"

#include "core/DeviceIdentity.h"
#include "core/EdlMaintenance.h"
#include "core/PartitionProtection.h"
#include "kc/config/AppSettings.h"
#include "protocol/Firehose.h"
#include "protocol/Sahara.h"
#include "storage/GptParser.h"
#include "storage/DynamicSuper.h"
#include "storage/LunCapacity.h"
#include "storage/RawProgramParser.h"
#include "storage/RawProgramWriter.h"
#include "storage/SparseImage.h"
#include "storage/SystemVersionReader.h"
#include "transport/SerialTransport.h"

#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QBuffer>
#include <QDir>
#include <QSaveFile>
#include <QThread>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

namespace swt::edl {
namespace {

/// 每次 program 命令的分片大小：正常 64 MB，兼容模式 8 MB（见 AppSettings 注释）。
/// 每次调用都读一次配置，用户改完设置不必重启就能生效。
quint64 sliceBytes()
{
    return kc::AppSettings::current().edlSliceBytes();
}

/// 擦除路径的字节数展示（EdlWorker 没有 MainWindow::formatBytes 可用）。
QString describeBytes(quint64 bytes)
{
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 2).arg(units[unit]);
}

/// 「开始 → 收尾」守卫（TMF 风格的 Start xxx.... → OK/Error）。
///
/// 为什么用 RAII 而不是在每个 return 前手写一次收尾：XML 刷写那条循环里有十几处
/// 失败 return（META 计划错误 / 容量越界 / extent 未对齐 / 写入失败…），漏一处就会
/// 留下一条永远停在「....」的日志行。守卫析构时统一补 Error，成功路径显式 ok()。
class OperationScope
{
public:
    /// body 是「开始行」的正文（带进度序号），endBody 是「收尾行」的正文（简短，
    /// 默认与 body 相同）。两行分开是为了不让收尾行重复一遍长串进度信息 ——
    /// 收尾行真正要读的是末尾那个 OK / Error 和耗时。
    OperationScope(std::function<void(const QString&)> begin,
                   std::function<void(const QString&, bool)> end, QString body,
                   QString endBody = QString())
        : m_end(std::move(end))
        , m_body(std::move(body))
        , m_endBody(endBody.isEmpty() ? m_body : std::move(endBody))
    {
        if (begin) begin(m_body);
    }
    ~OperationScope()
    {
        if (m_end) m_end(m_endBody, m_ok);
    }
    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;

    /// 标记成功（必须在作用域结束前调用，否则析构时补的是 Error）。
    void ok() { m_ok = true; }

private:
    std::function<void(const QString&, bool)> m_end;
    QString m_body;
    QString m_endBody;
    bool m_ok = false;
};

/// 按 32 位重复模式填充 [data, data+count)，字节相位从 phase（0..3）起。
/// 原实现逐字节移位拼图案：4 GB 的 FILL extent 就是 40 亿次循环。
/// 现在先把 4 字节图案摊成 4 KB 瓦片，再整块 memcpy —— 语义逐字节等价
/// （位置 p 的字节恒为 (value >> ((p & 3) * 8)) & 0xFF）。
void fillPattern(char* data, quint64 count, quint32 value, unsigned phase)
{
    if (count == 0) return;
    char tile[4096];
    for (qsizetype index = 0; index < static_cast<qsizetype>(sizeof(tile)); index += 4) {
        tile[index] = static_cast<char>(value & 0xFFU);
        tile[index + 1] = static_cast<char>((value >> 8) & 0xFFU);
        tile[index + 2] = static_cast<char>((value >> 16) & 0xFFU);
        tile[index + 3] = static_cast<char>((value >> 24) & 0xFFU);
    }
    // 头部：补到 4 字节对齐（最多 3 字节，逐字节无损失）
    const quint64 head = qMin<quint64>(count, (4U - phase) & 3U);
    for (quint64 index = 0; index < head; ++index) {
        data[index] = tile[(phase + index) & 3U];
    }
    // 主体：相位已归零，瓦片按 4 字节周期重复；末段不足整瓦时取瓦片前缀即可
    quint64 written = head;
    while (written < count) {
        const quint64 run = qMin<quint64>(static_cast<quint64>(sizeof(tile)),
                                          count - written);
        std::memcpy(data + written, tile, static_cast<size_t>(run));
        written += run;
    }
}

/// 擦除结果里「校验状态」的统一后缀 —— 报「完成」时必须让用户看见结果可信度。
QString eraseVerificationSuffix(FirehoseClient::EraseOutcome outcome)
{
    return outcome == FirehoseClient::EraseOutcome::Verified
        ? QStringLiteral("（首/尾哨兵已变化或为零，不代表整段逐字节校验）")
        : QStringLiteral("（结果未校验：哨兵扇区回读失败）");
}

class FillDevice final : public QIODevice {
public:
    FillDevice(quint32 value, quint64 length)
        : m_value(value), m_remaining(length)
    {
        open(QIODevice::ReadOnly);
    }

    bool isSequential() const override { return true; }
    qint64 size() const override { return static_cast<qint64>(m_remaining + m_position); }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if (maxSize <= 0 || m_remaining == 0) return 0;
        const quint64 count = qMin<quint64>(static_cast<quint64>(maxSize), m_remaining);
        fillPattern(data, count, m_value, static_cast<unsigned>(m_position & 3ULL));
        m_position += count;
        m_remaining -= count;
        return static_cast<qint64>(count);
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    quint32 m_value = 0;
    quint64 m_remaining = 0;
    quint64 m_position = 0;
};

class DynamicImageDevice final : public QIODevice {
public:
    explicit DynamicImageDevice(const DynamicSuperImage& image)
        : m_file(image.sourcePath), m_totalLength(image.allocatedSize),
          m_sourceLength(image.expandedSize), m_sparse(image.sparse)
    {
    }

    bool initialize(QString* error)
    {
        if (!m_file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("打开动态分区镜像失败：%1")
                                    .arg(m_file.errorString());
            return false;
        }
        if (m_sparse && !SparseImage::inspect(m_file, &m_sparseInfo, error)) return false;
        open(QIODevice::ReadOnly);
        return true;
    }

    bool isSequential() const override { return true; }
    qint64 size() const override { return static_cast<qint64>(m_totalLength); }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if (maxSize <= 0 || m_position >= m_totalLength) return 0;
        quint64 requested = qMin<quint64>(static_cast<quint64>(maxSize),
                                          m_totalLength - m_position);
        quint64 produced = 0;
        while (produced < requested) {
            if (m_position >= m_sourceLength) {
                const quint64 zeros = requested - produced;
                std::memset(data + produced, 0, static_cast<size_t>(zeros));
                produced += zeros;
                m_position += zeros;
                continue;
            }
            if (!m_sparse) {
                const quint64 count = qMin(requested - produced,
                                           m_sourceLength - m_position);
                if (!m_file.seek(static_cast<qint64>(m_position))) return -1;
                const qint64 read = m_file.read(data + produced, static_cast<qint64>(count));
                if (read != static_cast<qint64>(count)) return -1;
                produced += count;
                m_position += count;
                continue;
            }

            while (m_extentIndex < m_sparseInfo.extents.size()) {
                const SparseExtent& extent = m_sparseInfo.extents.at(m_extentIndex);
                const quint64 start = extent.outputBlock * m_sparseInfo.blockSize;
                const quint64 end = start + extent.blockCount * m_sparseInfo.blockSize;
                if (m_position < end) break;
                ++m_extentIndex;
            }
            if (m_extentIndex >= m_sparseInfo.extents.size()) return -1;
            const SparseExtent& extent = m_sparseInfo.extents.at(m_extentIndex);
            const quint64 extentStart = extent.outputBlock * m_sparseInfo.blockSize;
            const quint64 extentBytes = extent.blockCount * m_sparseInfo.blockSize;
            if (m_position < extentStart) return -1;
            const quint64 within = m_position - extentStart;
            const quint64 count = qMin(requested - produced, extentBytes - within);
            if (extent.type == SparseExtentType::Raw) {
                if (!m_file.seek(static_cast<qint64>(extent.fileOffset + within))) return -1;
                const qint64 read = m_file.read(data + produced, static_cast<qint64>(count));
                if (read != static_cast<qint64>(count)) return -1;
            } else if (extent.type == SparseExtentType::Fill) {
                fillPattern(data + produced, count, extent.fillValue,
                            static_cast<unsigned>(within & 3ULL));
            } else {
                std::memset(data + produced, 0, static_cast<size_t>(count));
            }
            produced += count;
            m_position += count;
        }
        return static_cast<qint64>(produced);
    }

    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QFile m_file;
    quint64 m_totalLength = 0;
    quint64 m_sourceLength = 0;
    quint64 m_position = 0;
    bool m_sparse = false;
    SparseImageInfo m_sparseInfo;
    qsizetype m_extentIndex = 0;
};

quint64 saturatingAdd(quint64 left, quint64 right)
{
    return right > std::numeric_limits<quint64>::max() - left
        ? std::numeric_limits<quint64>::max() : left + right;
}

bool resolvePatchExpression(QString expression, quint64 diskSectors, QString* resolved)
{
    static const QRegularExpression token(QStringLiteral(
        "NUM_DISK_SECTORS(?:\\s*([+-])\\s*([0-9]+)\\.?)?"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = token.match(expression);
    while (match.hasMatch()) {
        quint64 value = diskSectors;
        if (value == 0) return false;
        if (!match.captured(1).isEmpty()) {
            bool ok = false;
            const quint64 operand = match.captured(2).toULongLong(&ok);
            if (!ok) return false;
            if (match.captured(1) == QStringLiteral("-")) {
                if (operand > value) return false;
                value -= operand;
            } else {
                if (operand > std::numeric_limits<quint64>::max() - value) return false;
                value += operand;
            }
        }
        expression.replace(match.capturedStart(), match.capturedLength(), QString::number(value));
        match = token.match(expression);
    }
    *resolved = expression.trimmed();
    return !resolved->contains(QStringLiteral("NUM_DISK_SECTORS"), Qt::CaseInsensitive);
}

bool parseUnsignedText(QString text, quint64* value)
{
    text = text.trimmed();
    while (text.endsWith(QLatin1Char('.'))) text.chop(1);
    bool ok = false;
    const quint64 parsed = text.toULongLong(
        &ok, text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive) ? 16 : 10);
    if (ok) *value = parsed;
    return ok;
}

} // namespace

EdlWorker::EdlWorker(QObject* parent)
    : QObject(parent)
{
}

EdlWorker::~EdlWorker() = default;

void EdlWorker::setState(SessionState state, const QString& detail)
{
    emit stateChanged(state, detail.isEmpty() ? stateText(state) : detail);
}

quint32 EdlWorker::sectorSize() const
{
    return m_storage == StorageType::Ufs ? 4096U : 512U;
}

bool EdlWorker::resolveLunCapacity(quint32 lun, quint64* sectors)
{
    const quint64 cached = m_lunSectorCounts.value(lun, 0);
    if (cached > 0) {
        if (sectors) *sectors = cached;
        return true;
    }
    // GPT 没给出容量（该 LUN 为空 / 分区表损坏 / 还没读过分区表）时，向 loader
    // 逐 LUN 问一次 getstorageinfo —— 空设备刷机包时 NUM_DISK_SECTORS 的唯一
    // 合法来源。问不到就返回 false，让调用方 fail-closed 中止。
    if (!m_firehose) return false;
    quint64 bytes = 0;
    QString error;
    if (!m_firehose->getStorageInfo(lun, &bytes, &error) || bytes == 0) return false;
    const quint64 size = sectorSize();
    if (bytes < size) return false;
    const quint64 resolved = bytes / size;
    m_lunSectorCounts.insert(lun, resolved);
    if (sectors) *sectors = resolved;
    return true;
}

void EdlWorker::harvestLunCapacity(quint32 lun)
{
    if (m_lunSectorCounts.value(lun, 0) > 0) return; // GPT 已给出更权威的容量
    quint64 sectors = 0;
    if (!resolveLunCapacity(lun, &sectors)) return; // loader 没返回：静默，刷写时会报错
    emit logMessage(QStringLiteral("LUN %1 容量已由 getstorageinfo 解析：%2 扇区")
                        .arg(lun).arg(sectors));
}

void EdlWorker::fail(const QString& message, bool closeTransport)
{
    restoreProgressCallback();
    emit logMessage(QStringLiteral("错误：%1").arg(message));
    if (closeTransport) {
        m_firehose.reset();
        if (m_transport) m_transport->close();
        m_deviceInfo = {};
        m_lunSectorCounts.clear();
        m_currentPartitions.clear();
        emit deviceInfoChanged(m_deviceInfo);
        emit partitionsReady(QVector<PartitionInfo>{});
        setState(SessionState::Failed, message);
    } else {
        // Parameter, source-file and local-save failures leave the established
        // Firehose channel usable, so keep the UI in the connected Ready state.
        setState(SessionState::Ready, message);
    }
    m_busy = false;
    emit operationFinished(false, message);
}

void EdlWorker::finish(const QString& message)
{
    if (m_cancelled.load(std::memory_order_relaxed)) {
        fail(QStringLiteral("操作已停止，任务未报告成功"));
        return;
    }
    restoreProgressCallback();
    emit logMessage(message);
    setState(SessionState::Ready);
    m_busy = false;
    emit operationFinished(true, message);
}

void EdlWorker::restoreProgressCallback()
{
    if (!m_firehose) return;
    m_firehose->setProgressCallback([this](quint64 current, quint64 total) {
        emit progressChanged(current, total);
    });
}

bool EdlWorker::ensureReady(const QString& operation)
{
    if (m_busy) {
        emit operationFinished(false, QStringLiteral("当前已有任务正在执行"));
        return false;
    }
    if (!m_transport || !m_transport->isOpen() || !m_firehose) {
        emit operationFinished(false, QStringLiteral("请先连接 EDL 设备再执行%1").arg(operation));
        return false;
    }
    m_busy = true;
    m_cancelled.store(false, std::memory_order_relaxed);
    emit progressChanged(0, 0);
    emit overallProgressChanged(0, 0);
    return true;
}

void EdlWorker::connectDevice(const QString& portName, const QString& programmerPath,
                              StorageType storage, const OplusVipPackage& vipPackage)
{
    if (m_busy) return;
    m_busy = true;
    m_cancelled.store(false, std::memory_order_relaxed);
    emit progressChanged(0, 0);
    emit overallProgressChanged(0, 3);
    m_storage = storage;
    // LUN 容量是**按设备**的：换设备 / 重连必须清空，否则上一台的容量会被
    // 当成这一台的 NUM_DISK_SECTORS 分母用（读分区表也会清，这里补上
    // 「没读分区表就直接刷写」的路径）。
    m_lunSectorCounts.clear();
    m_deviceInfo = {};
    m_deviceInfo.portName = portName;
    m_deviceInfo.storage = storageName(storage).toUpper();
    m_deviceInfo.programmer = QFileInfo(programmerPath).fileName();
    m_deviceInfo.vipMode = vipPackage.enabled ? QStringLiteral("OPPO VIP")
                                              : QStringLiteral("标准 EDL");
    if (vipPackage.enabled) m_deviceInfo.brand = QStringLiteral("OPLUS");
    const QString initialChip = detectProgrammerChip(
        programmerPath, QStringList{vipPackage.packageDirectory});
    if (!initialChip.isEmpty()) {
        m_deviceInfo.model = qualcommChipDisplayName(initialChip);
    }

    if (portName.isEmpty()) {
        fail(QStringLiteral("请选择 Qualcomm 9008 串口"));
        return;
    }

    m_firehose.reset();
    m_transport = std::make_unique<SerialTransport>(&m_cancelled);
    QString error;

    setState(SessionState::OpeningPort);
    emit logMessage(QStringLiteral("打开端口 %1").arg(portName));
    if (!m_transport->open(portName, &error)) {
        fail(error);
        return;
    }
    emit logMessage(QStringLiteral("串口配置：%1").arg(m_transport->openDiagnostics()));
    if (!initialChip.isEmpty()) {
        emit logMessage(QStringLiteral("Programmer 芯片标识：%1（本地元数据）")
                            .arg(initialChip));
    }

    const auto log = [this](const QString& line) { emit logMessage(line); };
    const auto progress = [this](quint64 current, quint64 total) {
        emit progressChanged(current, total);
    };

    emit logMessage(QStringLiteral("检测设备当前通信状态…"));
    QByteArray probeHeader;
    QString probeError;
    bool saharaDetected = false;
    bool pendingSaharaTransfer = false;
    // Most BootROMs either expose HELLO immediately or wait for a host kick.
    // Keep this passive window short; the active Sahara recovery below handles
    // delayed qcusbser delivery without adding several seconds to every connect.
    if (m_transport->peekExactly(8, &probeHeader, 400, &probeError)) {
        const auto* bytes = reinterpret_cast<const uchar*>(probeHeader.constData());
        const quint32 command = qFromLittleEndian<quint32>(bytes);
        const quint32 length = qFromLittleEndian<quint32>(bytes + 4);
        saharaDetected = command == 0x01 && length >= 48 && length <= 1024 * 1024;
        pendingSaharaTransfer = command == 0x04 && length >= 16 && length <= 1024 * 1024;
    }

    if (!saharaDetected && !pendingSaharaTransfer) {
        setState(SessionState::WaitingForFirehose, QStringLiteral("检测 Firehose 模式"));
        auto firehose = std::make_unique<FirehoseClient>(
            *m_transport, m_cancelled, log, progress);
        QString firehoseError;
        if (firehose->ping(&firehoseError, 1200)) {
            emit logMessage(QStringLiteral("状态检测：设备已处于 Firehose 模式，跳过引导传输"));
            m_firehose = std::move(firehose);
            if (vipPackage.enabled) m_firehose->resumeOplusVip(vipPackage.digestPath);

            QString storageError;
            quint64 storageBytes = 0;
            if (!m_firehose->getStorageInfo(0, &storageBytes, &storageError)) {
                emit logMessage(QStringLiteral("Firehose 存储尚未就绪，自动执行 Configure"));
                setState(SessionState::Configuring);
                if (!m_firehose->configure(storage, &error)) {
                    fail(error);
                    return;
                }
                storageError.clear();
                storageBytes = 0;
                if (!m_firehose->getStorageInfo(0, &storageBytes, &storageError)) {
                    emit logMessage(QStringLiteral("存储信息查询跳过：%1").arg(storageError));
                }
            } else {
                emit logMessage(QStringLiteral("Firehose 存储已配置，可直接读取分区表"));
            }
            // 同主链路：连接期收下 LUN0 容量，供「不读分区表直接刷 XML」用。
            const quint64 detectedSize = sectorSize();
            if (storageBytes >= detectedSize) {
                m_lunSectorCounts.insert(0, storageBytes / detectedSize);
                emit logMessage(QStringLiteral("LUN0 容量已由 getstorageinfo 解析：%1 扇区")
                                    .arg(storageBytes / detectedSize));
            }

            if (m_deviceInfo.programmer.isEmpty()) {
                m_deviceInfo.programmer = QStringLiteral("已加载 Firehose");
            }
            m_firehose->enrichDeviceInfo(&m_deviceInfo);
            emit deviceInfoChanged(m_deviceInfo);
            emit overallProgressChanged(3, 3);
            m_busy = false;
            setState(SessionState::Ready);
            emit logMessage(QStringLiteral("EDL 通道接管完成"));
            emit operationFinished(true, QStringLiteral("设备连接成功"));
            return;
        }

        emit logMessage(QStringLiteral("Firehose 探测未响应，切换为 Sahara 引导流程"));
        m_transport->close();
        QThread::msleep(220);
        if (!m_transport->open(portName, &error)) {
            fail(error);
            return;
        }
        emit logMessage(QStringLiteral("串口已按 Sahara 参数重新打开：%1")
                            .arg(m_transport->openDiagnostics()));
    } else if (saharaDetected) {
        emit logMessage(QStringLiteral("状态检测：设备处于 Sahara 模式，需要发送引导"));
    }

    bool programmerUploaded = false;
    if (pendingSaharaTransfer) {
        emit logMessage(QStringLiteral("状态检测：Programmer 已传输，正在完成 Sahara 交接"));
        setState(SessionState::SaharaHandshake, QStringLiteral("确认 Sahara DONE"));
        SaharaClient sahara(*m_transport, m_cancelled, log, progress);
        programmerUploaded = sahara.finishPendingTransfer(&error);
        if (!programmerUploaded) {
            // 🔴 旧行为：这一支失败就直接 fail()。设备会一直停在「已发 END_TRANSFER
            // 等主机回 DONE」的状态，下次连接 probe 又读到 0x04、又进同一分支、
            // 又同样失败 —— 形成「一次失败后再也发不了引导」的死路（而 else 分支
            // 有「候选轮询 + 重试 2 次 + 重开端口」三层恢复，结构本身不对称）。
            // 逃生口：重开端口后交给下面的完整 uploadProgrammer()，它自带
            // HELLO_RESPONSE + SWITCH_MODE + RESET_STATE_MACHINE 三轮唤醒。
            emit logMessage(QStringLiteral("Sahara 交接失败：%1").arg(error));
            emit logMessage(QStringLiteral(
                "重开端口并把状态机推回 HELLO，回退到完整引导流程"));
            m_transport->close();
            QThread::msleep(220);
            error.clear();
            if (!m_transport->open(portName, &error)) {
                fail(error);
                return;
            }
            emit logMessage(QStringLiteral("串口已重开：%1")
                                .arg(m_transport->openDiagnostics()));
        }
    }
    if (!programmerUploaded) {
        if (programmerPath.isEmpty() || !QFileInfo(programmerPath).isFile()) {
            fail(QStringLiteral("设备处于 Sahara 模式，请选择有效的 Firehose Programmer"));
            return;
        }

        QStringList programmerCandidates{programmerPath};
        if (vipPackage.enabled && !vipPackage.programmerCandidates.isEmpty()) {
            programmerCandidates = vipPackage.programmerCandidates;
            if (!programmerCandidates.contains(programmerPath, Qt::CaseInsensitive)) {
                programmerCandidates.prepend(programmerPath);
            }
        }

        bool terminalSaharaFailure = false;
        for (int candidateIndex = 0;
             candidateIndex < programmerCandidates.size() && !programmerUploaded &&
             !terminalSaharaFailure;
             ++candidateIndex) {
            const QString candidate = programmerCandidates.at(candidateIndex);
            m_deviceInfo.programmer = QFileInfo(candidate).fileName();
            const QString candidateChip = detectProgrammerChip(
                candidate, QStringList{vipPackage.packageDirectory});
            if (!candidateChip.isEmpty()) {
                m_deviceInfo.model = qualcommChipDisplayName(candidateChip);
            }
            if (candidateIndex > 0) {
                emit logMessage(QStringLiteral("Sahara 自动切换候选 Programmer：%1")
                                    .arg(m_deviceInfo.programmer));
            }

            for (int attempt = 0; attempt < 2; ++attempt) {
                setState(SessionState::SaharaHandshake);
                SaharaClient sahara(*m_transport, m_cancelled, log, progress);
                setState(SessionState::UploadingProgrammer);
                error.clear();
                quint32 saharaStatus = 0;
                if (sahara.uploadProgrammer(candidate, &m_deviceInfo,
                                            !vipPackage.enabled, &error,
                                            &saharaStatus)) {
                    programmerUploaded = true;
                    emit deviceInfoChanged(m_deviceInfo);
                    break;
                }

                const bool authenticationNak = saharaStatus == 0x21 ||
                                               saharaStatus == 0x22;
                const bool hasNextCandidate = candidateIndex + 1 < programmerCandidates.size();
                const bool retrySameCandidate = attempt == 0 && !authenticationNak;
                if ((retrySameCandidate || (authenticationNak && hasNextCandidate)) &&
                    !m_cancelled.load(std::memory_order_relaxed)) {
                    emit logMessage(authenticationNak
                        ? QStringLiteral("当前 Programmer 被 BootROM 校验拒绝，重新打开端口后尝试下一签名版本")
                        : QStringLiteral("Sahara 首次握手未完成，重新打开端口再试一次"));
                    m_transport->close();
                    QThread::msleep(800);
                    if (!m_transport->open(portName, &error)) break;
                    emit logMessage(QStringLiteral("串口重连参数：%1")
                                        .arg(m_transport->openDiagnostics()));
                }
                if (authenticationNak) break;
                if (attempt == 1) terminalSaharaFailure = true;
            }
        }
    }
    if (!programmerUploaded) {
        fail(error);
        return;
    }
    emit overallProgressChanged(1, 3);

    setState(SessionState::WaitingForFirehose);
    // VIP programmers start a short watchdog as soon as Sahara DONE completes.
    // Enter the Firehose receive/send path immediately instead of spending the
    // first watchdog window in a fixed hand-off delay.
    QThread::msleep(vipPackage.enabled ? 20 : 700);
    m_firehose = std::make_unique<FirehoseClient>(*m_transport, m_cancelled, log, progress);

    if (vipPackage.enabled) {
        setState(SessionState::OplusVipVerification);
        if (!m_firehose->initializeOplusVip(vipPackage.digestPath, vipPackage.signaturePath,
                                            storage, &error)) {
            fail(QStringLiteral("OPPO VIP 原生协议失败：%1").arg(error));
            return;
        }
    } else {
        if (!m_firehose->ping(&error)) {
            fail(QStringLiteral("Firehose 启动检查失败：%1").arg(error));
            return;
        }
        setState(SessionState::Configuring);
        if (!m_firehose->configure(storage, &error)) {
            fail(error);
            return;
        }
    }

    emit overallProgressChanged(2, 3);
    QString infoError;
    quint64 storageBytes = 0;
    if (!m_firehose->getStorageInfo(0, &storageBytes, &infoError)) {
        emit logMessage(QStringLiteral("存储信息查询跳过：%1").arg(infoError));
    } else {
        // 连接期就把 LUN0 容量收下来：「connect → flashXml」不读分区表也能解析
        // NUM_DISK_SECTORS；其余 LUN 在读分区表 / 刷写时按需探测。
        const quint64 size = sectorSize();
        if (storageBytes >= size) {
            m_lunSectorCounts.insert(0, storageBytes / size);
            emit logMessage(QStringLiteral("LUN0 容量已由 getstorageinfo 解析：%1 扇区")
                                .arg(storageBytes / size));
        }
    }
    emit overallProgressChanged(3, 3);

    if (m_deviceInfo.programmer.isEmpty()) {
        m_deviceInfo.programmer = QStringLiteral("已加载 Firehose");
    }
    m_firehose->enrichDeviceInfo(&m_deviceInfo);
    const QString identity = !m_deviceInfo.serial.isEmpty()
        ? m_deviceInfo.serial : m_deviceInfo.hwid;
    emit logMessage(QStringLiteral("设备信息：%1，CPUID %2，存储 %3%4")
                        .arg(m_deviceInfo.model.isEmpty() ? QStringLiteral("Qualcomm")
                                                         : m_deviceInfo.model,
                             identity.isEmpty() ? QStringLiteral("—") : identity,
                             m_deviceInfo.storage.isEmpty() ? storageName(storage).toUpper()
                                                            : m_deviceInfo.storage,
                             m_deviceInfo.storageProduct.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" / %1%2").arg(
                                       m_deviceInfo.storageVendor.isEmpty()
                                           ? QString() : m_deviceInfo.storageVendor + QLatin1Char(' '),
                                       m_deviceInfo.storageProduct)));
    emit deviceInfoChanged(m_deviceInfo);

    m_busy = false;
    setState(SessionState::Ready);
    emit logMessage(QStringLiteral("EDL 通道初始化完成"));
    emit operationFinished(true, QStringLiteral("设备连接成功"));
}

void EdlWorker::disconnectDevice()
{
    m_cancelled.store(true, std::memory_order_relaxed);
    m_firehose.reset();
    if (m_transport) m_transport->close();
    m_busy = false;
    m_deviceInfo = {};
    m_lunSectorCounts.clear();
    m_currentPartitions.clear();
    emit deviceInfoChanged(m_deviceInfo);
    emit partitionsReady(QVector<PartitionInfo>{});
    setState(SessionState::Disconnected);
    emit logMessage(QStringLiteral("EDL 连接已关闭"));
    emit operationFinished(true, QStringLiteral("连接已断开"));
}

void EdlWorker::readPartitionTable()
{
    if (!ensureReady(QStringLiteral("读取分区表"))) return;
    setState(SessionState::Reading, QStringLiteral("读取 GPT 分区表"));

    QVector<PartitionInfo> allPartitions;
    m_lunSectorCounts.clear();
    const quint32 lunCount = m_storage == StorageType::Ufs ? 6U : 1U;
    const quint64 gptSectors = m_storage == StorageType::Ufs ? 6ULL : 34ULL;
    QString lastError;
    bool streamDesynced = false;
    emit overallProgressChanged(0, lunCount);

    for (quint32 lun = 0; lun < lunCount; ++lun) {
        emit progressChanged(0, 0);
        emit overallProgressChanged(lun, lunCount);
        if (m_cancelled.load(std::memory_order_relaxed)) {
            fail(QStringLiteral("操作已停止"));
            return;
        }
        emit logMessage(QStringLiteral("读取 LUN %1 主 GPT").arg(lun));
        QByteArray gpt;
        QString error;
        if (!m_firehose->readToByteArray(&gpt, 0, gptSectors, lun, sectorSize(), &error)) {
            emit logMessage(QStringLiteral("LUN %1：%2").arg(lun).arg(error));
            lastError = error;
            if (error.contains(QStringLiteral("串口读取超时")) ||
                error.contains(QStringLiteral("等待协议帧超时"))) {
                emit logMessage(QStringLiteral("RAW 数据流已失去同步，停止继续读取后续 LUN"));
                streamDesynced = true;
                break;
            }
            harvestLunCapacity(lun);
            continue;
        }
        QVector<PartitionInfo> partitions;
        if (!GptParser::parse(gpt, sectorSize(), lun, &partitions, &error)) {
            emit logMessage(QStringLiteral("LUN %1 GPT：%2").arg(lun).arg(error));
            lastError = error;
            harvestLunCapacity(lun);
            continue;
        }
        // 只有通过 GPT 结构解析的头才可提供容量，垃圾 backupLba 不能污染缓存。
        const quint64 backupLba = qFromLittleEndian<quint64>(
            reinterpret_cast<const uchar*>(gpt.constData() + sectorSize() + 32));
        if (backupLba > 0 && backupLba < std::numeric_limits<quint64>::max()) {
            m_lunSectorCounts.insert(lun, backupLba + 1);
        } else {
            harvestLunCapacity(lun);
        }
        allPartitions += partitions;
    }

    if (allPartitions.isEmpty()) {
        if (streamDesynced) {
            // 协议失步：后续帧全部不可信，必须拆连接（fail-closed）。
            fail(lastError.isEmpty() ? QStringLiteral("没有读取到有效 GPT") : lastError);
            return;
        }
        // 设备上就是没有 GPT（空设备 / 分区表被清 / LUN 从未分区）。这不是协议
        // 错误，Firehose 会话本身是好的 —— **保持连接**，让用户直接「选择资料」
        // 载入刷机包写入。
        // 🔴 旧行为在这里 fail(closeTransport=true) 拆连接，而 toggleConnection
        //    又置了「连上自动读 GPT」→ connect → 读 GPT → 失败 → 断开 无限循环，
        //    空设备永远刷不进去（2026-10-13 用户报修：lun0-5 分区没数据时无法
        //    选择刷机并写入刷机包）。
        m_currentPartitions.clear();
        emit partitionsReady(QVector<PartitionInfo>{});
        const QString reason = lastError.isEmpty()
            ? QStringLiteral("各 LUN 均无有效分区数据")
            : lastError;
        fail(QStringLiteral("未读取到有效 GPT 分区表（%1）。设备可能为空或分区表已损坏；"
                            "Firehose 会话保持连接，可直接选择刷机包（rawprogram XML）写入。")
                 .arg(reason),
             false);
        return;
    }
    emit overallProgressChanged(lunCount, lunCount);
    m_currentPartitions = allPartitions;
    emit partitionsReady(allPartitions);
    finish(QStringLiteral("分区表读取完成，共 %1 个分区").arg(allPartitions.size()));
}

void EdlWorker::readSystemVersionInfo()
{
    if (!ensureReady(QStringLiteral("读取系统版本信息"))) return;
    if (m_currentPartitions.isEmpty()) {
        fail(QStringLiteral("请先读取 GPT 分区表"), false);
        return;
    }
    setState(SessionState::Reading, QStringLiteral("读取系统版本信息"));
    emit progressChanged(0, 0);
    emit overallProgressChanged(0, 1);
    emit logMessage(QStringLiteral("开始从设备分区读取系统版本信息…"));

    const auto findPartition = [this](const QStringList& names) -> const PartitionInfo* {
        for (const QString& name : names) {
            for (const PartitionInfo& partition : std::as_const(m_currentPartitions)) {
                if (partition.name.compare(name, Qt::CaseInsensitive) == 0) return &partition;
            }
        }
        return nullptr;
    };
    const auto byteReader = [this](const PartitionInfo& partition) {
        return [this, partition](quint64 offset, quint32 length, QString* error) -> QByteArray {
            if (length == 0 || offset > partition.sectorCount() * sectorSize() ||
                length > partition.sectorCount() * sectorSize() - offset) return {};
            const quint64 alignedStart = offset / sectorSize() * sectorSize();
            const quint64 end = offset + length;
            const quint64 alignedEnd = ((end + sectorSize() - 1) / sectorSize()) * sectorSize();
            const quint64 count = (alignedEnd - alignedStart) / sectorSize();
            QByteArray raw;
            if (!m_firehose->readToByteArray(&raw,
                    partition.firstLba + alignedStart / sectorSize(), count,
                    partition.lun, sectorSize(), error, partition.name,
                    partition.name + QStringLiteral(".img"))) return {};
            return raw.mid(static_cast<qsizetype>(offset - alignedStart), length);
        };
    };

    SystemVersionInfo version;
    QString error;
    bool found = false;
    if (const PartitionInfo* super = findPartition(
            {QStringLiteral("super"), QStringLiteral("super_a")})) {
        emit logMessage(QStringLiteral("解析 super 动态分区元数据…"));
        found = SystemVersionReader::readSuper(
            byteReader(*super), super->sectorCount() * sectorSize(), &version, &error,
            [this](const QString& status) { emit logMessage(status); });
    }
    if (!found) {
        const QStringList candidates{QStringLiteral("my_manifest"), QStringLiteral("odm"),
            QStringLiteral("my_product"), QStringLiteral("vendor"),
            QStringLiteral("product"), QStringLiteral("system_ext"),
            QStringLiteral("system")};
        for (const QString& base : candidates) {
            const PartitionInfo* partition = findPartition(
                {base, base + QStringLiteral("_a"), base + QStringLiteral("_b")});
            if (!partition) continue;
            emit logMessage(QStringLiteral("解析 %1 版本信息…").arg(partition->name));
            QString localError;
            if (SystemVersionReader::readPhysical(
                    byteReader(*partition), partition->sectorCount() * sectorSize(),
                    &version, &localError)) found = true;
        }
    }
    if (!found || version.isEmpty()) {
        fail(error.isEmpty() ? QStringLiteral("设备分区中未识别到系统版本属性") : error,
             false);
        return;
    }

    if (!version.brand.isEmpty()) m_deviceInfo.brand = version.brand.toUpper();
    m_deviceInfo.productName = !version.marketName.isEmpty()
        ? version.marketName : version.model;
    m_deviceInfo.androidVersion = version.androidVersion;
    m_deviceInfo.systemVersion = version.systemVersion;
    m_deviceInfo.securityPatch = version.securityPatch;
    m_deviceInfo.buildFingerprint = version.fingerprint;
    emit deviceInfoChanged(m_deviceInfo);

    const auto logField = [this](const QString& title, const QString& value) {
        if (!value.isEmpty()) emit logMessage(QStringLiteral("%1：%2").arg(title, value));
    };
    logField(QStringLiteral("设备名称"), m_deviceInfo.productName);
    logField(QStringLiteral("品牌"), version.brand);
    logField(QStringLiteral("设备代号"), version.device);
    logField(QStringLiteral("平台"), version.platform);
    logField(QStringLiteral("Android"), version.androidVersion);
    logField(QStringLiteral("系统版本"), version.systemVersion);
    logField(QStringLiteral("OTA 版本"), version.otaVersion);
    logField(QStringLiteral("安全补丁"), version.securityPatch);
    logField(QStringLiteral("地区"), version.region);
    logField(QStringLiteral("编译日期"), version.buildDate);
    logField(QStringLiteral("系统指纹"), version.fingerprint);
    if (!version.fileSystems.isEmpty())
        emit logMessage(QStringLiteral("文件系统：%1").arg(version.fileSystems.join(QStringLiteral(" / "))));
    emit overallProgressChanged(1, 1);
    finish(QStringLiteral("系统版本信息读取完成"));
}

void EdlWorker::backupPartition(const PartitionInfo& partition, const QString& outputPath)
{
    if (!ensureReady(QStringLiteral("备份分区"))) return;
    setState(SessionState::Reading, QStringLiteral("备份 %1").arg(partition.name));
    emit overallProgressChanged(0, 1);

    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("创建备份文件失败：%1").arg(output.errorString()), false);
        return;
    }

    QString error;
    if (!m_firehose->readToDevice(output, partition.firstLba, partition.sectorCount(),
                                  partition.lun, sectorSize(), &error,
                                  partition.name, partition.name + QStringLiteral(".bin"))) {
        output.cancelWriting();
        fail(error);
        return;
    }
    if (m_cancelled.load(std::memory_order_relaxed)) {
        output.cancelWriting();
        fail(QStringLiteral("操作已停止，未提交备份文件"));
        return;
    }
    if (!output.commit()) {
        fail(QStringLiteral("保存备份失败：%1").arg(output.errorString()), false);
        return;
    }
    emit overallProgressChanged(1, 1);
    finish(QStringLiteral("分区 %1 已备份到 %2").arg(partition.name, outputPath));
}

void EdlWorker::backupPartitions(const QVector<PartitionInfo>& partitions,
                                 const QString& outputDirectory, bool generateXml)
{
    if (!ensureReady(QStringLiteral("批量读取分区"))) return;
    if (partitions.isEmpty() || outputDirectory.isEmpty()) {
        fail(QStringLiteral("批量读取参数为空"), false);
        return;
    }
    QDir directory(outputDirectory);
    if (!directory.exists() && !QDir().mkpath(outputDirectory)) {
        fail(QStringLiteral("创建备份目录失败：%1").arg(outputDirectory), false);
        return;
    }
    runBackupPartitions(partitions, outputDirectory, generateXml);
}

void EdlWorker::runBackupPartitions(const QVector<PartitionInfo>& partitions,
                                    const QString& outputDirectory, bool generateXml)
{
    const QDir directory(outputDirectory);
    quint64 totalBytes = 0;
    for (const PartitionInfo& partition : partitions) {
        totalBytes = saturatingAdd(totalBytes, partition.sectorCount() * sectorSize());
    }
    quint64 completed = 0;
    QStringList usedNames;
    QStringList outputNames;
    setState(SessionState::Reading,
             QStringLiteral("批量读取 %1 个分区").arg(partitions.size()));
    emit logMessage(QStringLiteral("回读任务：%1 个分区，总计 %2 字节，保存到 %3")
                        .arg(partitions.size()).arg(totalBytes).arg(outputDirectory));
    emit progressChanged(0, 0);
    emit overallProgressChanged(0, totalBytes);
    QElapsedTimer progressThrottle;

    for (qsizetype index = 0; index < partitions.size(); ++index) {
        const PartitionInfo& partition = partitions.at(index);
        QString outputName = partition.name;
        outputName.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*]")),
                           QStringLiteral("_"));
        outputName += QStringLiteral(".img");
        if (usedNames.contains(outputName, Qt::CaseInsensitive)) {
            outputName = QStringLiteral("%1_lun%2.img").arg(partition.name).arg(partition.lun);
        }
        usedNames.push_back(outputName);
        outputNames.push_back(outputName);
        const QString outputPath = directory.filePath(outputName);
        // TMF 风格操作行（Start Read xxx.... → OK）
        const QString opBody = QStringLiteral("读取 %1（%2/%3，LUN %4）")
                                   .arg(partition.name)
                                   .arg(index + 1)
                                   .arg(partitions.size())
                                   .arg(partition.lun);
        emit logOperationBegin(opBody);
        const QString opEndBody = QStringLiteral("读取 %1").arg(partition.name);
        const auto opEnd = [this, &opEndBody](bool ok) {
            emit logOperationEnd(opEndBody, ok);
        };

        QSaveFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly)) {
            opEnd(false);
            fail(QStringLiteral("创建备份文件失败：%1").arg(output.errorString()), false);
            return;
        }
        const quint64 base = completed;
        const quint64 partitionBytes = partition.sectorCount() * sectorSize();
        emit progressChanged(0, partitionBytes);
        emit overallProgressChanged(completed, totalBytes);
        progressThrottle.invalidate();
        m_firehose->setProgressCallback([this, base, totalBytes, &progressThrottle](
                                            quint64 current, quint64 currentTotal) {
            const bool finalUpdate = current >= currentTotal;
            if (progressThrottle.isValid() && !finalUpdate &&
                progressThrottle.elapsed() < 33) {
                return;
            }
            if (progressThrottle.isValid()) progressThrottle.restart();
            else progressThrottle.start();
            emit progressChanged(current, currentTotal);
            emit overallProgressChanged(saturatingAdd(base, current), totalBytes);
        });
        QString error;
        if (!m_firehose->readToDevice(output, partition.firstLba, partition.sectorCount(),
                                      partition.lun, sectorSize(), &error,
                                      partition.name, outputName)) {
            output.cancelWriting();
            opEnd(false);
            fail(QStringLiteral("读取分区 %1 失败：%2").arg(partition.name, error));
            return;
        }
        if (m_cancelled.load(std::memory_order_relaxed)) {
            output.cancelWriting();
            opEnd(false);
            fail(QStringLiteral("操作已停止，未提交备份文件"));
            return;
        }
        if (!output.commit()) {
            opEnd(false);
            fail(QStringLiteral("保存备份失败：%1").arg(output.errorString()), false);
            return;
        }
        completed = saturatingAdd(completed, partition.sectorCount() * sectorSize());
        emit progressChanged(partitionBytes, partitionBytes);
        emit overallProgressChanged(completed, totalBytes);
        opEnd(true);
    }

    m_firehose->setProgressCallback([this](quint64 current, quint64 total) {
        emit progressChanged(current, total);
    });
    if (generateXml) {
        QStringList generatedPaths;
        QString xmlError;
        if (!RawProgramWriter::writeBackup(outputDirectory, partitions, outputNames,
                                           sectorSize(), &generatedPaths, &xmlError)) {
            fail(xmlError, false);
            return;
        }
        for (const QString& path : generatedPaths) {
            emit logMessage(QStringLiteral("已生成回读 XML：%1").arg(path));
        }
    } else {
        emit logMessage(QStringLiteral("生成 XML 已关闭，本次仅保存分区镜像"));
    }
    finish(QStringLiteral("批量读取完成：%1 个分区，保存到 %2")
               .arg(partitions.size()).arg(outputDirectory));
}

void EdlWorker::flashPartition(const PartitionInfo& partition, const QString& imagePath)
{
    // 单分区与批量共用 sparse 解码、容量校验和默认保护策略。
    flashPartitions({partition}, {imagePath}, false);
}

void EdlWorker::flashPartitions(const QVector<PartitionInfo>& requestedPartitions,
                                const QStringList& requestedImagePaths,
                                bool allowProtected)
{
    if (!ensureReady(QStringLiteral("批量写入分区"))) return;
    if (requestedPartitions.isEmpty() || requestedPartitions.size() != requestedImagePaths.size()) {
        fail(QStringLiteral("分区与镜像数量不匹配，拒绝部分执行"), false);
        return;
    }
    QVector<PartitionInfo> partitions;
    QStringList imagePaths;
    for (qsizetype index = 0; index < requestedPartitions.size() &&
                              index < requestedImagePaths.size(); ++index) {
        if (!allowProtected && isProtectedPartitionName(requestedPartitions.at(index).name)) {
            emit logMessage(QStringLiteral("保护分区：跳过写入 %1")
                                .arg(requestedPartitions.at(index).name));
            continue;
        }
        partitions.push_back(requestedPartitions.at(index));
        imagePaths.push_back(requestedImagePaths.at(index));
    }
    if (partitions.isEmpty() || partitions.size() != imagePaths.size()) {
        fail(QStringLiteral("分区与镜像数量不匹配"), false);
        return;
    }

    QString rangeError;
    if (!validateEraseRanges(partitions, &rangeError)) {
        fail(QStringLiteral("写入范围校验失败：%1").arg(rangeError), false);
        return;
    }
    quint64 totalBytes = 0;
    QVector<bool> sparseImages;
    QVector<qint64> sourceSizes;
    sparseImages.reserve(imagePaths.size());
    sourceSizes.reserve(imagePaths.size());
    for (qsizetype index = 0; index < imagePaths.size(); ++index) {
        const QFileInfo image(imagePaths.at(index));
        if (!image.isFile() || image.size() <= 0) {
            fail(QStringLiteral("镜像文件错误：%1").arg(imagePaths.at(index)), false);
            return;
        }
        QFile probe(image.absoluteFilePath());
        if (!probe.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("打开镜像失败：%1").arg(probe.errorString()), false);
            return;
        }
        sourceSizes.push_back(probe.size());
        const bool sparse = SparseImage::hasSparseMagic(probe);
        sparseImages.push_back(sparse);
        if (sparse) {
            SparseImageInfo sparseInfo;
            QString sparseError;
            if (!SparseImage::inspect(probe, &sparseInfo, &sparseError)) {
                fail(QStringLiteral("%1：%2").arg(image.fileName(), sparseError), false);
                return;
            }
            if (sparseInfo.blockSize % sectorSize() != 0 ||
                sparseInfo.expandedSize() > partitions.at(index).sectorCount() * sectorSize()) {
                fail(QStringLiteral("Sparse 镜像 %1 展开后超过分区 %2 容量")
                         .arg(image.fileName(), partitions.at(index).name), false);
                return;
            }
            for (const SparseExtent& extent : sparseInfo.extents) {
                if (extent.type != SparseExtentType::Skip) {
                    totalBytes = saturatingAdd(
                        totalBytes, extent.blockCount * sparseInfo.blockSize);
                }
            }
        } else {
            const quint64 bytes = static_cast<quint64>(image.size());
            if (bytes > partitions.at(index).sectorCount() * sectorSize()) {
                fail(QStringLiteral("镜像 %1 超过分区 %2 容量")
                         .arg(image.fileName(), partitions.at(index).name), false);
                return;
            }
            totalBytes = saturatingAdd(totalBytes, bytes);
        }
    }
    if (totalBytes == 0) {
        fail(QStringLiteral("镜像中没有可传输的数据"), false);
        return;
    }

    quint64 completed = 0;
    setState(SessionState::Writing,
             QStringLiteral("批量写入 %1 个分区").arg(partitions.size()));
    emit progressChanged(0, 0);
    emit overallProgressChanged(0, totalBytes);
    for (qsizetype index = 0; index < partitions.size(); ++index) {
        const PartitionInfo& partition = partitions.at(index);
        const QString& imagePath = imagePaths.at(index);
        QFile input(imagePath);
        if (!input.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("打开镜像失败：%1").arg(input.errorString()), false);
            return;
        }
        if (input.size() != sourceSizes.at(index) ||
            SparseImage::hasSparseMagic(input) != sparseImages.at(index)) {
            fail(QStringLiteral("镜像在预检后发生变化，拒绝部分写入：%1").arg(imagePath), false);
            return;
        }
        // TMF 风格操作行：先起一行「写入 boot（1/12，LUN 0）....」，写完后在同一处
        // 补 OK / Error 并带耗时。原来是「写完才打一行」，中途卡住时看不出在写哪个分区。
        const QString opBody = QStringLiteral("写入 %1（%2/%3，LUN %4）")
                                   .arg(partition.name)
                                   .arg(index + 1)
                                   .arg(partitions.size())
                                   .arg(partition.lun);
        emit logOperationBegin(opBody);
        // 收尾行只写短名：长串进度信息在开始行已经有了，收尾行要读的是 OK/Error 和耗时。
        const QString opEndBody = QStringLiteral("写入 %1").arg(partition.name);
        const auto opEnd = [this, &opEndBody](bool ok) {
            emit logOperationEnd(opEndBody, ok);
        };
        QString error;
        if (sparseImages.at(index)) {
            SparseImageInfo sparseInfo;
            if (!SparseImage::inspect(input, &sparseInfo, &error)) {
                opEnd(false);
                fail(QStringLiteral("%1：%2").arg(QFileInfo(imagePath).fileName(), error), false);
                return;
            }
            if (sparseInfo.blockSize % sectorSize() != 0 ||
                sparseInfo.expandedSize() > partition.sectorCount() * sectorSize()) {
                opEnd(false);
                fail(QStringLiteral("Sparse 镜像在预检后变化或超过分区容量：%1")
                         .arg(partition.name), false);
                return;
            }
            const QVector<SparseExtent> mergedExtents =
                SparseImage::coalesce(sparseInfo.extents, sparseInfo.blockSize);
            emit logMessage(QStringLiteral("Android Sparse：%1 个块，展开 %2 字节")
                                .arg(sparseInfo.totalBlocks).arg(sparseInfo.expandedSize()));
            for (const SparseExtent& extent : mergedExtents) {
                if (extent.type == SparseExtentType::Skip) continue;
                const quint64 extentBytes = extent.blockCount * sparseInfo.blockSize;
                const quint64 targetSector = partition.firstLba +
                    extent.outputBlock * sparseInfo.blockSize / sectorSize();
                const bool ok = extent.type == SparseExtentType::Raw
                    ? programFileRange(input, extent.fileOffset, extentBytes, targetSector,
                                       partition.lun, sectorSize(), partition.name,
                                       QFileInfo(imagePath).fileName(), &completed,
                                       totalBytes, &error)
                    : programFillRange(extent.fillValue, extentBytes, targetSector,
                                       partition.lun, sectorSize(), partition.name,
                                       QFileInfo(imagePath).fileName(), &completed,
                                       totalBytes, &error);
                if (!ok) {
                    opEnd(false);
                    fail(QStringLiteral("写入分区 %1 失败：%2").arg(partition.name, error));
                    return;
                }
            }
        } else {
            const quint64 imageBytes = static_cast<quint64>(input.size());
            const quint64 base = completed;
            m_firehose->setProgressCallback([this, base, totalBytes](quint64 current,
                                                                     quint64 currentTotal) {
                emit progressChanged(current, currentTotal);
                emit overallProgressChanged(saturatingAdd(base, current), totalBytes);
            });
            if (!m_firehose->program(input, imageBytes, partition.firstLba,
                                     partition.sectorCount(), partition.lun, sectorSize(), &error,
                                     partition.name, QFileInfo(imagePath).fileName())) {
                opEnd(false);
                fail(QStringLiteral("写入分区 %1 失败：%2").arg(partition.name, error));
                return;
            }
            completed = saturatingAdd(completed, imageBytes);
            emit progressChanged(imageBytes, imageBytes);
            emit overallProgressChanged(completed, totalBytes);
        }
        opEnd(true);
    }

    m_firehose->setProgressCallback([this](quint64 current, quint64 total) {
        emit progressChanged(current, total);
    });
    finish(QStringLiteral("批量写入完成：%1 个分区").arg(partitions.size()));
}

bool EdlWorker::programFileRange(QFile& input, quint64 fileOffset, quint64 byteCount,
                                 quint64 targetStartSector, quint32 lun,
                                 quint32 xmlSectorSize, const QString& label,
                                 const QString& filename, quint64* completed,
                                 quint64 total, QString* error)
{
    if (input.size() < 0 || fileOffset > static_cast<quint64>(input.size()) ||
        byteCount == 0 || byteCount > static_cast<quint64>(input.size()) - fileOffset) {
        if (error) *error = QStringLiteral("镜像偏移或读取长度越界，文件可能在预检后发生变化");
        return false;
    }
    if (!input.seek(static_cast<qint64>(fileOffset))) {
        if (error) *error = QStringLiteral("定位镜像数据失败：%1").arg(input.errorString());
        return false;
    }
    return programDeviceRange(input, byteCount, targetStartSector, lun, xmlSectorSize,
                              label, filename, completed, total, error);
}

bool EdlWorker::programDeviceRange(QIODevice& input, quint64 byteCount,
                                   quint64 targetStartSector, quint32 lun,
                                   quint32 xmlSectorSize, const QString& label,
                                   const QString& filename, quint64* completed,
                                   quint64 total, QString* error)
{
    if (xmlSectorSize == 0 || byteCount == 0 || !completed ||
        byteCount > std::numeric_limits<quint64>::max() - (xmlSectorSize - 1)) {
        if (error) *error = QStringLiteral("写入分片参数错误或长度溢出");
        return false;
    }
    const quint64 sectors = byteCount / xmlSectorSize + (byteCount % xmlSectorSize != 0);
    if (sectors > std::numeric_limits<quint64>::max() - targetStartSector) {
        if (error) *error = QStringLiteral("写入分片扇区范围溢出");
        return false;
    }
    quint64 rangeCompleted = 0;
    while (rangeCompleted < byteCount) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            if (error) *error = QStringLiteral("操作已停止");
            return false;
        }
        const quint64 chunkBytes = qMin(sliceBytes(), byteCount - rangeCompleted);
        const quint64 chunkSectors = (chunkBytes + xmlSectorSize - 1) / xmlSectorSize;
        const quint64 overallBase = *completed;
        const quint64 taskBase = rangeCompleted;
        m_firehose->setProgressCallback([this, overallBase, taskBase, byteCount, total](
                                            quint64 current, quint64) {
            emit progressChanged(saturatingAdd(taskBase, current), byteCount);
            emit overallProgressChanged(saturatingAdd(overallBase, current), total);
        });
        if (!m_firehose->program(input, chunkBytes,
                                 targetStartSector + rangeCompleted / xmlSectorSize,
                                 chunkSectors, lun, xmlSectorSize, error, label, filename)) {
            return false;
        }
        rangeCompleted += chunkBytes;
        *completed = saturatingAdd(*completed, chunkBytes);
        emit progressChanged(rangeCompleted, byteCount);
        emit overallProgressChanged(*completed, total);
    }
    return true;
}

bool EdlWorker::programFillRange(quint32 fillValue, quint64 byteCount,
                                 quint64 targetStartSector, quint32 lun,
                                 quint32 xmlSectorSize, const QString& label,
                                 const QString& filename, quint64* completed,
                                 quint64 total, QString* error)
{
    FillDevice fill(fillValue, byteCount);
    return programDeviceRange(fill, byteCount, targetStartSector, lun, xmlSectorSize,
                              label, filename, completed, total, error);
}

void EdlWorker::flashXml(const QStringList& xmlPaths, bool allowProtected, bool keepUserData)
{
    if (!ensureReady(QStringLiteral("XML 刷机"))) return;

    RawProgramPlan plan;
    QString error;
    if (!RawProgramParser::parse(xmlPaths, &plan, &error)) {
        fail(error, false);
        return;
    }
    // 「保留用户数据」（移植自 TMFTool 的「保数据」）：把 userdata / metadata 从
    // 计划里摘掉，等价于「刷系统但不清数据」。必须在重复目标检查与保护分区过滤
    // **之前**做 —— 否则日志里会出现「userdata 被写两次」这种不存在的警告。
    if (keepUserData) {
        QStringList skipped;
        int removed = 0;
        for (qsizetype index = plan.entries.size(); index > 0; --index) {
            const RawProgramEntry& entry = plan.entries.at(index - 1);
            const QString name = normalizedPartitionName(
                !entry.label.isEmpty() ? entry.label : entry.filename);
            if (name != QLatin1String("userdata") && name != QLatin1String("metadata")) continue;
            plan.entries.removeAt(index - 1);
            ++removed;
            if (!skipped.contains(name, Qt::CaseInsensitive)) skipped.push_back(name);
        }
        if (removed > 0) {
            emit logMessage(QStringLiteral(
                "保留用户数据：已从刷写计划中移除 %1 条（%2），用户数据不会被清除。")
                                .arg(removed)
                                .arg(skipped.join(QStringLiteral("、"))));
        } else {
            emit logMessage(QStringLiteral(
                "保留用户数据：计划中没有 userdata / metadata 条目，本次无额外跳过。"));
        }
    }
    // 标准 XML 与非标准 XML 可混选，混选/包内重复时同一 LUN+起始扇区会被写两次。
    // 这里按**最终执行顺序**报一次，事后才能解释「为什么这个分区是另一份镜像」。
    {
        QHash<QString, QString> owners;
        QStringList duplicates;
        for (const RawProgramEntry& entry : std::as_const(plan.entries)) {
            if (!entry.startSectorResolved) continue;
            const QString key = QStringLiteral("%1/%2").arg(entry.lun).arg(entry.startSector);
            const QString owner = QStringLiteral("%1(%2)")
                                      .arg(QFileInfo(entry.xmlPath).fileName(),
                                           entry.label.isEmpty() ? entry.filename : entry.label);
            auto it = owners.constFind(key);
            if (it == owners.constEnd()) {
                owners.insert(key, owner);
            } else if (it.value() != owner) {
                duplicates.push_back(QStringLiteral("LUN %1 起始扇区 %2：%3 → 后被 %4 覆盖")
                                         .arg(entry.lun).arg(entry.startSector)
                                         .arg(it.value(), owner));
            }
        }
        if (!duplicates.isEmpty()) {
            QStringList shown = duplicates.mid(0, 10);
            if (duplicates.size() > shown.size()) {
                shown.push_back(QStringLiteral("……另有 %1 处未列出")
                                    .arg(duplicates.size() - shown.size()));
            }
            emit logMessage(QStringLiteral(
                "【注意】计划中有 %1 处同一目标被重复写入，按 XML 顺序后者覆盖前者：%2")
                                .arg(duplicates.size())
                                .arg(shown.join(QStringLiteral("；"))));
        }
    }
    QVector<RawProgramEntry> protectedRanges;
    QHash<QString, int> removedProtected;
    for (qsizetype index = plan.entries.size(); index > 0; --index) {
        const RawProgramEntry& entry = plan.entries.at(index - 1);
        if (!allowProtected && isProtectedPartitionMapping(entry.label, entry.filename)) {
            protectedRanges.push_back(entry);
            removedProtected[normalizedPartitionName(
                !entry.label.isEmpty() ? entry.label : entry.filename)]++;
            plan.entries.removeAt(index - 1);
        }
    }
    if (!allowProtected) {
        for (const PartitionInfo& partition : std::as_const(m_currentPartitions)) {
            if (!isProtectedPartitionName(partition.name)) continue;
            RawProgramEntry range;
            range.label = partition.name;
            range.lun = partition.lun;
            range.startSector = partition.firstLba;
            range.startSectorResolved = true;
            range.sectorCount = partition.sectorCount();
            range.sectorSize = sectorSize();
            protectedRanges.push_back(range);
        }
    }
    for (auto it = removedProtected.cbegin(); it != removedProtected.cend(); ++it) {
        emit logMessage(QStringLiteral("保护分区：已跳过 %1 的 %2 个 XML program 条目")
                            .arg(it.key()).arg(it.value()));
    }

    const auto entryStartSector = [this](const RawProgramEntry& entry,
                                         quint64* startSector) {
        if (entry.startSectorResolved) {
            *startSector = entry.startSector;
            return true;
        }
        // 动态扇区表达式先要该 LUN 的总扇区数：读分区表时从 GPT 备份头拿到的值
        // 优先；空设备（该 LUN 没有 GPT）时 resolveLunCapacity 向 loader 逐 LUN
        // 问 getstorageinfo。两条路都断 → false → 调用方 fail-closed 中止，
        // 绝不允许把 0 当容量算偏移。
        quint64 diskSectors = 0;
        if (!resolveLunCapacity(entry.lun, &diskSectors)) return false;
        return resolveSectorExpression(entry.startSectorExpression, diskSectors,
                                       startSector);
    };

    const auto rangeEnd = [](quint64 start, quint64 count) {
        if (count == 0) return start;
        return count - 1 > std::numeric_limits<quint64>::max() - start
            ? std::numeric_limits<quint64>::max() : start + count - 1;
    };
    const auto protectedConflict = [&](const RawProgramEntry& entry, quint64 start,
                                       QString* protectedName) {
        const quint64 end = rangeEnd(start, entry.sectorCount);
        for (const RawProgramEntry& protectedEntry : std::as_const(protectedRanges)) {
            if (entry.lun != protectedEntry.lun) continue;
            quint64 protectedStart = 0;
            if (!entryStartSector(protectedEntry, &protectedStart)) continue;
            const quint64 protectedEnd = rangeEnd(protectedStart,
                                                  protectedEntry.sectorCount);
            if (start <= protectedEnd && protectedStart <= end) {
                if (protectedName) *protectedName = protectedEntry.label;
                return true;
            }
        }
        return false;
    };

    for (const RawProgramEntry& protectedEntry : std::as_const(protectedRanges)) {
        quint64 start = 0;
        if (entryStartSector(protectedEntry, &start)) {
            emit logMessage(QStringLiteral("保护范围已锁定：%1，LUN %2，扇区 %3-%4")
                                .arg(protectedEntry.label).arg(protectedEntry.lun)
                                .arg(start).arg(rangeEnd(start, protectedEntry.sectorCount)));
        }
    }

    if (!allowProtected) {
        for (qsizetype index = plan.entries.size(); index > 0; --index) {
            const RawProgramEntry& entry = plan.entries.at(index - 1);
            quint64 start = 0;
            if (!entryStartSector(entry, &start)) continue;
            QString protectedName;
            if (!protectedConflict(entry, start, &protectedName)) continue;
            emit logMessage(QStringLiteral("保护分区：XML 条目 %1 与 %2 的物理范围重叠，已跳过")
                                .arg(entry.label, protectedName));
            plan.entries.removeAt(index - 1);
        }
    }

    QStringList preflightDetails;
    if (!RawProgramParser::inspect(plan, sectorSize(), &preflightDetails, &error)) {
        fail(QStringLiteral("刷写前源包预检失败：%1").arg(error), false);
        return;
    }
    quint64 totalBytes = 0;
    for (RawProgramEntry& entry : plan.entries) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            fail(QStringLiteral("操作已停止（尚未开始写入）"), false);
            return;
        }
        quint64 resolvedStart = 0;
        if (!entryStartSector(entry, &resolvedStart)) {
            fail(QStringLiteral("LUN %1 容量未知，未能解析动态扇区 %2：请重新读取分区表，"
                                "或确认设备已返回该 LUN 的存储容量。")
                     .arg(entry.lun).arg(entry.startSectorExpression), false);
            return;
        }
        quint64 diskSectors = 0;
        if (!resolveLunCapacity(entry.lun, &diskSectors) || resolvedStart >= diskSectors ||
            entry.sectorCount == 0 || entry.sectorCount > diskSectors - resolvedStart) {
            fail(QStringLiteral("XML 条目 %1 的 LUN %2 容量未知或写入范围越界")
                     .arg(entry.filename).arg(entry.lun), false);
            return;
        }
        entry.startSector = resolvedStart;
        entry.startSectorResolved = true;
        QString protectedName;
        if (protectedConflict(entry, resolvedStart, &protectedName)) {
            fail(QStringLiteral("XML 条目 %1 与受保护分区 %2 的物理范围重叠，任务已终止")
                     .arg(entry.label, protectedName), false);
            return;
        }
        if (entry.sectorSize != sectorSize()) {
            fail(QStringLiteral("%1 的扇区大小为 %2，当前存储配置为 %3")
                     .arg(entry.filename).arg(entry.sectorSize).arg(sectorSize()), false);
            return;
        }
        if (entry.sectorCount > std::numeric_limits<quint64>::max() / entry.sectorSize) {
            fail(QStringLiteral("XML 写入范围溢出：%1").arg(entry.filename), false);
            return;
        }

        if (entry.dynamicSuper) {
            DynamicSuperPlan dynamicPlan;
            if (!DynamicSuperPlanner::create(entry.xmlPath, entry.sourcePath,
                                             &dynamicPlan, &error)) {
                fail(QStringLiteral("super META 计划错误：%1").arg(error), false);
                return;
            }
            if (dynamicPlan.blockDeviceSize > entry.sectorCount * entry.sectorSize) {
                fail(QStringLiteral("super_def 容量大于 XML 中的 super 分区"), false);
                return;
            }
            totalBytes = saturatingAdd(totalBytes, dynamicPlan.totalWriteBytes());
            continue;
        }

        QFile file(entry.sourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("打开镜像失败：%1").arg(file.errorString()), false);
            return;
        }
        entry.sourceSizeAtPreflight = file.size();
        const bool sparse = entry.sparse || SparseImage::hasSparseMagic(file);
        if (sparse) {
            if (entry.fileSectorOffset != 0) {
                fail(QStringLiteral("sparse 镜像要求 file_sector_offset 为 0：%1")
                         .arg(entry.filename), false);
                return;
            }
            SparseImageInfo sparseInfo;
            if (!SparseImage::inspect(file, &sparseInfo, &error)) {
                fail(QStringLiteral("%1：%2").arg(entry.filename, error), false);
                return;
            }
            if (sparseInfo.blockSize % entry.sectorSize != 0 ||
                sparseInfo.expandedSize() > entry.sectorCount * entry.sectorSize) {
                fail(QStringLiteral("sparse 展开范围超出 XML 目标：%1").arg(entry.filename), false);
                return;
            }
            // 相邻同类 extent 先合并：逐 chunk 一次 program 命令 = 逐 chunk 一次
            // XML 往返 + 一次 ACK 等待，万级 chunk 的镜像上这是主要额外耗时。
            const QVector<SparseExtent> mergedExtents =
                SparseImage::coalesce(sparseInfo.extents, sparseInfo.blockSize);
            quint64 sparseTransferBytes = 0;
            for (const SparseExtent& extent : sparseInfo.extents) {
                if (extent.type == SparseExtentType::Skip) continue;
                sparseTransferBytes = saturatingAdd(
                    sparseTransferBytes, extent.blockCount * sparseInfo.blockSize);
            }
            totalBytes = saturatingAdd(totalBytes, sparseTransferBytes);
        } else {
            const quint64 offset = entry.fileSectorOffset * entry.sectorSize;
            if (file.size() <= 0 || offset >= static_cast<quint64>(file.size())) {
                fail(QStringLiteral("镜像偏移超出当前文件：%1").arg(entry.filename), false);
                return;
            }
            const quint64 available = static_cast<quint64>(file.size()) - offset;
            totalBytes = saturatingAdd(totalBytes,
                                       qMin(available, entry.sectorCount * entry.sectorSize));
        }
    }
    if (totalBytes == 0) {
        fail(QStringLiteral("XML 中没有可传输的数据"), false);
        return;
    }

    setState(SessionState::Writing, QStringLiteral("按 rawprogram XML 刷写"));
    // 这一行在界面上是**区段标题**（居中带横线），标记「这批刷写从哪开始」。
    // plan.warnings 不在这里重复 —— 选包时 MainWindow 已经逐条打过，
    // 同一条「混选重复目标」在日志里出现两遍只会让用户以为出了两次问题。
    emit logMessage(QStringLiteral("开始刷写：%1 个 XML，%2 个 program 条目")
                        .arg(plan.xmlPaths.size()).arg(plan.entries.size()));
    if (plan.dynamicSuperCount > 0) {
        emit logMessage(QStringLiteral("检测到 %1 个 super 条目：其中 %2 个走 META 免合并写入")
                            .arg(plan.superEntryCount).arg(plan.dynamicSuperCount));
    } else if (plan.superEntryCount > 1) {
        emit logMessage(QStringLiteral("检测到 %1 个 super 分段条目：按 XML 逐段直写")
                            .arg(plan.superEntryCount));
    }

    emit progressChanged(0, 0);
    emit overallProgressChanged(0, totalBytes);
    quint64 completed = 0;
    int activeIndex = 0;
    const int activeCount = plan.entries.size();

    const auto restoreProgress = [this] {
        if (!m_firehose) return;
        m_firehose->setProgressCallback([this](quint64 current, quint64 total) {
            emit progressChanged(current, total);
        });
    };

    for (const RawProgramEntry& entry : plan.entries) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            restoreProgress();
            fail(QStringLiteral("操作已停止"));
            return;
        }
        quint64 targetStartSector = 0;
        if (!entryStartSector(entry, &targetStartSector)) {
            restoreProgress();
            fail(QStringLiteral("解析动态扇区失败：%1").arg(entry.startSectorExpression), false);
            return;
        }
        ++activeIndex;
        QString sourceName = entry.filename;
        const QString resolvedFileName = QFileInfo(entry.sourcePath).fileName();
        if (!entry.dynamicSuper && !resolvedFileName.isEmpty() &&
            resolvedFileName.compare(entry.filename, Qt::CaseInsensitive) != 0) {
            sourceName += QStringLiteral(" [%1]").arg(resolvedFileName);
        }
        // TMF 风格操作行（Start Write xxx.... → OK）。这一层是**最外层**：super 的
        // 多个子镜像 / 多个 extent 都落在同一次操作里，所以只能在这一层起收尾对，
        // 内部细节继续走普通日志行（嵌套 begin 会把 LogView 的计时覆盖掉）。
        OperationScope op(
            [this](const QString& body) { emit logOperationBegin(body); },
            [this](const QString& body, bool ok) { emit logOperationEnd(body, ok); },
            QStringLiteral("刷写 %1（%2/%3，LUN %4）")
                .arg(entry.label.isEmpty() ? sourceName : entry.label)
                .arg(activeIndex)
                .arg(activeCount)
                .arg(entry.lun),
            QStringLiteral("刷写 %1").arg(entry.label.isEmpty() ? sourceName : entry.label));
        emit logMessage(QStringLiteral("XML %1/%2：%3 → %4，LUN %5，起始扇区 %6")
                            .arg(activeIndex).arg(activeCount)
                            .arg(sourceName, entry.label.isEmpty()
                                                       ? QStringLiteral("(无标签)") : entry.label)
                            .arg(entry.lun).arg(targetStartSector));

        if (entry.dynamicSuper) {
            DynamicSuperPlan dynamicPlan;
            if (!DynamicSuperPlanner::create(entry.xmlPath, entry.sourcePath,
                                             &dynamicPlan, &error)) {
                restoreProgress();
                fail(QStringLiteral("super META 计划错误：%1").arg(error), false);
                return;
            }
            if (dynamicPlan.blockDeviceSize > entry.sectorCount * entry.sectorSize) {
                restoreProgress();
                fail(QStringLiteral("super META 在预检后发生变化或容量越界"), false);
                return;
            }
            emit logMessage(QStringLiteral(
                "super 走 META 免合并写入：LP %1.%2，%3 个槽位，%4 个镜像，共 %5 字节")
                .arg(dynamicPlan.metadataMajorVersion).arg(dynamicPlan.metadataMinorVersion)
                .arg(dynamicPlan.metadataSlotCount).arg(dynamicPlan.images.size())
                .arg(dynamicPlan.totalWriteBytes()));
            emit logMessage(QStringLiteral("写入 super metadata：%1 字节")
                                .arg(dynamicPlan.metadataImage.size()));
            QBuffer metadataBuffer(&dynamicPlan.metadataImage);
            metadataBuffer.open(QIODevice::ReadOnly);
            if (!programDeviceRange(metadataBuffer,
                                    static_cast<quint64>(dynamicPlan.metadataImage.size()),
                                    targetStartSector, entry.lun, entry.sectorSize,
                                    QStringLiteral("super"), dynamicPlan.metadataFilename,
                                    &completed, totalBytes, &error)) {
                restoreProgress();
                fail(QStringLiteral("super metadata 写入失败：%1").arg(error));
                return;
            }

            for (qsizetype imageIndex = 0; imageIndex < dynamicPlan.images.size();
                 ++imageIndex) {
                const DynamicSuperImage& image = dynamicPlan.images.at(imageIndex);
                emit logMessage(QStringLiteral("super %1/%2：%3 ← %4，分配 %5 字节")
                                    .arg(imageIndex + 1).arg(dynamicPlan.images.size())
                                    .arg(image.partitionName, QFileInfo(image.sourcePath).fileName())
                                    .arg(image.allocatedSize));
                DynamicImageDevice source(image);
                if (!source.initialize(&error)) {
                    restoreProgress();
                    fail(QStringLiteral("动态分区 %1：%2").arg(image.partitionName, error),
                         false);
                    return;
                }
                for (const DynamicSuperExtent& extent : image.extents) {
                    if (extent.physicalOffset % entry.sectorSize != 0 ||
                        extent.byteCount % entry.sectorSize != 0 ||
                        extent.physicalOffset > dynamicPlan.blockDeviceSize ||
                        extent.byteCount > dynamicPlan.blockDeviceSize - extent.physicalOffset) {
                        restoreProgress();
                        fail(QStringLiteral("动态分区 extent 未按 Firehose 扇区对齐：%1")
                                 .arg(image.partitionName), false);
                        return;
                    }
                    const quint64 targetSector = targetStartSector +
                        extent.physicalOffset / entry.sectorSize;
                    if (!programDeviceRange(source, extent.byteCount, targetSector,
                                            entry.lun, entry.sectorSize,
                                            QStringLiteral("super"),
                                            dynamicPlan.metadataFilename,
                                            &completed, totalBytes, &error)) {
                        restoreProgress();
                        fail(QStringLiteral("动态分区 %1 写入失败：%2")
                                 .arg(image.partitionName, error));
                        return;
                    }
                }
            }
            op.ok();
            continue;
        }

        QFile file(entry.sourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            restoreProgress();
            fail(QStringLiteral("打开镜像失败：%1").arg(file.errorString()), false);
            return;
        }
        if (file.size() != entry.sourceSizeAtPreflight) {
            restoreProgress();
            fail(QStringLiteral("镜像长度在预检后发生变化，拒绝部分写入：%1")
                     .arg(entry.filename), false);
            return;
        }
        const QString payloadFilename = QFileInfo(entry.sourcePath).fileName().isEmpty()
            ? entry.filename : QFileInfo(entry.sourcePath).fileName();
        const bool sparse = entry.sparse || SparseImage::hasSparseMagic(file);
        if (sparse) {
            SparseImageInfo sparseInfo;
            if (!SparseImage::inspect(file, &sparseInfo, &error)) {
                restoreProgress();
                fail(QStringLiteral("%1：%2").arg(entry.filename, error), false);
                return;
            }
            if (entry.fileSectorOffset != 0 || sparseInfo.blockSize % entry.sectorSize != 0 ||
                sparseInfo.expandedSize() > entry.sectorCount * entry.sectorSize) {
                restoreProgress();
                fail(QStringLiteral("sparse 在预检后发生变化或展开范围越界：%1")
                         .arg(entry.filename), false);
                return;
            }
            // 与规划循环（上方 totalBytes 统计）同一口径：相邻同类 extent
            // 先合并再逐段 program —— 两处必须用同一个 coalesce，否则按
            // 合并后的段数估的进度和实际传输量对不上。
            const QVector<SparseExtent> mergedExtents =
                SparseImage::coalesce(sparseInfo.extents, sparseInfo.blockSize);
            emit logMessage(QStringLiteral(
                "Android sparse：%1 个块 / %2 个 chunk（相邻同类合并为 %3 段传输），展开 %4 字节，不生成临时 raw 文件")
                .arg(sparseInfo.totalBlocks).arg(sparseInfo.extents.size())
                .arg(mergedExtents.size()).arg(sparseInfo.expandedSize()));
            for (const SparseExtent& extent : mergedExtents) {
                const quint64 extentBytes = extent.blockCount * sparseInfo.blockSize;
                const quint64 targetSector = targetStartSector +
                    extent.outputBlock * sparseInfo.blockSize / entry.sectorSize;
                bool ok = true;
                if (extent.type == SparseExtentType::Skip) {
                    continue;
                } else if (extent.type == SparseExtentType::Raw) {
                    ok = programFileRange(file, extent.fileOffset, extentBytes, targetSector,
                                          entry.lun, entry.sectorSize, entry.label,
                                          payloadFilename, &completed, totalBytes, &error);
                } else {
                    ok = programFillRange(extent.fillValue, extentBytes, targetSector,
                                          entry.lun, entry.sectorSize, entry.label,
                                          payloadFilename, &completed, totalBytes, &error);
                }
                if (!ok) {
                    restoreProgress();
                    fail(QStringLiteral("XML 条目 %1 写入失败：%2").arg(entry.filename, error));
                    return;
                }
            }
        } else {
            const quint64 offset = entry.fileSectorOffset * entry.sectorSize;
            if (file.size() <= 0 || offset >= static_cast<quint64>(file.size())) {
                restoreProgress();
                fail(QStringLiteral("镜像在预检后发生变化，偏移超出当前文件：%1")
                         .arg(entry.filename), false);
                return;
            }
            const quint64 bytes = qMin(static_cast<quint64>(file.size()) - offset,
                                       entry.sectorCount * entry.sectorSize);
            if (!programFileRange(file, offset, bytes, targetStartSector, entry.lun,
                                  entry.sectorSize, entry.label, payloadFilename,
                                  &completed, totalBytes, &error)) {
                restoreProgress();
                fail(QStringLiteral("XML 条目 %1 写入失败：%2").arg(entry.filename, error));
                return;
            }
        }
        op.ok();
    }
    QStringList patchPaths;
    const QRegularExpression rawprogramName(
        QStringLiteral("^rawprogram([0-9]+)\\.xml$"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString& xmlPath : plan.xmlPaths) {
        const QFileInfo xmlInfo(xmlPath);
        const QRegularExpressionMatch match = rawprogramName.match(xmlInfo.fileName());
        if (!match.hasMatch()) continue;
        const QString patchPath = xmlInfo.dir().filePath(
            QStringLiteral("patch%1.xml").arg(match.captured(1)));
        if (QFileInfo::exists(patchPath) &&
            !patchPaths.contains(patchPath, Qt::CaseInsensitive)) {
            patchPaths.push_back(patchPath);
        }
    }

    int appliedPatches = 0;
    for (const QString& patchPath : patchPaths) {
        emit logMessage(QStringLiteral("Patch：%1").arg(QFileInfo(patchPath).fileName()));
        QFile patchFile(patchPath);
        if (!patchFile.open(QIODevice::ReadOnly)) {
            restoreProgress();
            fail(QStringLiteral("打开 Patch XML 失败：%1").arg(patchFile.errorString()), false);
            return;
        }
        QXmlStreamReader reader(&patchFile);
        while (!reader.atEnd()) {
            reader.readNext();
            if (!reader.isStartElement() ||
                reader.name().compare(QLatin1String("patch"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            const QXmlStreamAttributes attrs = reader.attributes();
            if (attrs.value(QLatin1String("filename")).toString().compare(
                    QStringLiteral("DISK"), Qt::CaseInsensitive) != 0) {
                continue;
            }
            quint64 patchSectorSize = 0;
            quint64 byteOffset = 0;
            quint64 lun = 0;
            quint64 sizeInBytes = 0;
            if (!parseUnsignedText(attrs.value(QLatin1String("SECTOR_SIZE_IN_BYTES")).toString(),
                                   &patchSectorSize) ||
                !parseUnsignedText(attrs.value(QLatin1String("byte_offset")).toString(),
                                   &byteOffset) ||
                !parseUnsignedText(attrs.value(QLatin1String("physical_partition_number")).toString(),
                                   &lun) ||
                !parseUnsignedText(attrs.value(QLatin1String("size_in_bytes")).toString(),
                                   &sizeInBytes) || lun > std::numeric_limits<quint32>::max()) {
                restoreProgress();
                fail(QStringLiteral("Patch XML 参数错误：%1").arg(QFileInfo(patchPath).fileName()),
                     false);
                return;
            }
            quint64 diskSectors = 0;
            if (!resolveLunCapacity(static_cast<quint32>(lun), &diskSectors)) {
                restoreProgress();
                fail(QStringLiteral("Patch 表达式解析失败：LUN %1 容量未知").arg(lun),
                     false);
                return;
            }
            QString startSector;
            QString value;
            if (!resolvePatchExpression(
                    attrs.value(QLatin1String("start_sector")).toString(), diskSectors,
                    &startSector) ||
                !resolvePatchExpression(attrs.value(QLatin1String("value")).toString(),
                                        diskSectors, &value)) {
                restoreProgress();
                fail(QStringLiteral("Patch 表达式解析失败：LUN %1").arg(lun), false);
                return;
            }
            if (!m_firehose->patch(static_cast<quint32>(patchSectorSize), byteOffset,
                                   static_cast<quint32>(lun), sizeInBytes,
                                   startSector, value, &error)) {
                restoreProgress();
                fail(QStringLiteral("Patch %1 应用失败：%2")
                         .arg(QFileInfo(patchPath).fileName(), error));
                return;
            }
            ++appliedPatches;
        }
        if (reader.hasError()) {
            restoreProgress();
            fail(QStringLiteral("Patch XML 解析失败：%1").arg(reader.errorString()), false);
            return;
        }
    }
    if (appliedPatches > 0) {
        emit logMessage(QStringLiteral("GPT Patch 应用完成：%1 条 DISK patch")
                            .arg(appliedPatches));
    }

    emit logMessage(QStringLiteral("设置启动分区…"));
    const quint32 bootableValue = m_storage == StorageType::Ufs ? 1U : 0U;
    if (m_firehose->setBootableStorageDrive(bootableValue, &error)) {
        emit logMessage(QStringLiteral("设置启动分区完成：value=%1").arg(bootableValue));
    } else {
        emit logMessage(QStringLiteral("设置启动分区未确认，保留已完成刷写：%1").arg(error));
    }

    restoreProgress();
    finish(QStringLiteral("XML 刷机完成：%1 个 program 条目，super 全程免合并")
               .arg(activeCount));
}

bool EdlWorker::validateEraseRange(const PartitionInfo& partition, QString* error)
{
    const quint64 requested = partition.sectorCount();
    if (requested == 0 || partition.lastLba < partition.firstLba || sectorSize() == 0 ||
        requested > std::numeric_limits<quint64>::max() / sectorSize()) {
        if (error) {
            *error = QStringLiteral("分区 %1 的范围为空、倒置或字节长度溢出，已拒绝操作").arg(partition.name);
        }
        return false;
    }
    quint64 capacity = 0;
    if (!resolveLunCapacity(partition.lun, &capacity) || capacity == 0) {
        if (error) {
            *error = QStringLiteral(
                "无法确定 LUN %1 的容量，已中止擦除 %2（不允许在容量未知时下发 erase）")
                .arg(partition.lun).arg(partition.name);
        }
        return false;
    }
    if (partition.firstLba >= capacity || requested > capacity - partition.firstLba) {
        if (error) {
            *error = QStringLiteral(
                "分区 %1 的擦除范围越界，已中止：LUN %2 容量 %3 扇区，"
                "请求擦除 [%4, %5)（%6 扇区 / %7）")
                .arg(partition.name).arg(partition.lun).arg(capacity)
                .arg(partition.firstLba).arg(partition.firstLba + requested)
                .arg(requested).arg(describeBytes(requested * sectorSize()));
        }
        return false;
    }
    return true;
}

void EdlWorker::erasePartition(const PartitionInfo& partition)
{
    if (!ensureReady(QStringLiteral("擦除分区"))) return;

    QString error;
    if (!validateEraseRange(partition, &error)) {
        fail(error, false); // 本地范围问题：Firehose 会话本身是好的，别拆连接
        return;
    }

    setState(SessionState::Erasing, QStringLiteral("擦除 %1").arg(partition.name));
    const quint64 totalBytes = partition.sectorCount() * sectorSize();
    emit progressChanged(0, totalBytes);
    emit overallProgressChanged(0, totalBytes);

    const FirehoseClient::EraseOutcome outcome = m_firehose->erase(
        partition.firstLba, partition.sectorCount(), partition.lun, sectorSize(),
        &error, partition.name, partition.name + QStringLiteral(".bin"));
    if (outcome == FirehoseClient::EraseOutcome::Failed) {
        fail(error);
        return;
    }
    if (outcome == FirehoseClient::EraseOutcome::NotLanded) {
        // 「假擦除」：loader 回了 ACK 但数据还在。绝不能显示「擦除完成」。
        fail(error, false);
        return;
    }
    emit progressChanged(totalBytes, totalBytes);
    emit overallProgressChanged(totalBytes, totalBytes);
    finish(QStringLiteral("分区 %1 擦除完成%2")
               .arg(partition.name, eraseVerificationSuffix(outcome)));
}

void EdlWorker::erasePartitions(const QVector<PartitionInfo>& requestedPartitions,
                                bool allowProtected)
{
    if (!ensureReady(QStringLiteral("批量擦除分区"))) return;
    QVector<PartitionInfo> partitions;
    for (const PartitionInfo& partition : requestedPartitions) {
        if (!allowProtected && isProtectedPartitionName(partition.name)) {
            emit logMessage(QStringLiteral("保护分区：跳过擦除 %1").arg(partition.name));
        } else {
            partitions.push_back(partition);
        }
    }
    if (partitions.isEmpty()) {
        fail(QStringLiteral("批量擦除列表为空"), false);
        return;
    }

    // 范围校验必须一次性做完再动手：擦到一半才发现越界，会留下「擦了一半」的
    // 半成品状态，比直接拒绝更危险。
    QString rangeError;
    if (!validateEraseRanges(partitions, &rangeError)) {
        fail(rangeError, false);
        return;
    }
    runErasePartitions(partitions);
}

bool EdlWorker::validateEraseRanges(const QVector<PartitionInfo>& partitions, QString* error)
{
    for (const PartitionInfo& partition : partitions) {
        QString rangeError;
        if (!validateEraseRange(partition, &rangeError)) {
            if (error) *error = rangeError;
            return false;
        }
    }
    return true;
}

void EdlWorker::runErasePartitions(const QVector<PartitionInfo>& partitions)
{
    quint64 plannedBytes = 0;
    for (const PartitionInfo& partition : partitions) {
        const quint64 bytes = partition.sectorCount() * sectorSize();
        plannedBytes = bytes > std::numeric_limits<quint64>::max() - plannedBytes
            ? std::numeric_limits<quint64>::max()
            : plannedBytes + bytes;
    }

    setState(SessionState::Erasing,
             QStringLiteral("批量擦除 %1 个分区，共 %2")
                 .arg(partitions.size()).arg(describeBytes(plannedBytes)));
    // 旧行为在这里 setProgressCallback({}) —— 整个擦除只剩 0% 与 100% 两个状态，
    // 界面看起来像「点一下就完成」，是「假擦除」观感的来源之一。现在按分区字节
    // 数推进，并保留回调。
    m_firehose->setProgressCallback([this](quint64 current, quint64 total) {
        emit progressChanged(current, total);
    });
    emit progressChanged(0, plannedBytes);
    emit overallProgressChanged(0, static_cast<quint64>(partitions.size()));

    quint64 doneBytes = 0;
    quint64 unverifiedCount = 0;
    for (qsizetype index = 0; index < partitions.size(); ++index) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            fail(QStringLiteral("操作已停止"), false);
            return;
        }
        const PartitionInfo& partition = partitions.at(index);
        const quint64 bytes = partition.sectorCount() * sectorSize();
        emit progressChanged(doneBytes, plannedBytes);
        // TMF 风格操作行（Start Erase xxx.... → OK）
        const QString opBody = QStringLiteral("擦除 %1（%2/%3，LUN %4，%5 扇区）")
                                   .arg(partition.name)
                                   .arg(index + 1)
                                   .arg(partitions.size())
                                   .arg(partition.lun)
                                   .arg(partition.sectorCount());
        emit logOperationBegin(opBody);
        const QString opEndBody = QStringLiteral("擦除 %1").arg(partition.name);
        const auto opEnd = [this, &opEndBody](bool ok) {
            emit logOperationEnd(opEndBody, ok);
        };
        QString error;
        const FirehoseClient::EraseOutcome outcome = m_firehose->erase(
            partition.firstLba, partition.sectorCount(), partition.lun, sectorSize(),
            &error, partition.name, partition.name + QStringLiteral(".bin"));
        if (outcome == FirehoseClient::EraseOutcome::Failed) {
            opEnd(false);
            fail(QStringLiteral("擦除分区 %1 失败：%2").arg(partition.name, error));
            return;
        }
        if (outcome == FirehoseClient::EraseOutcome::NotLanded) {
            opEnd(false);
            fail(QStringLiteral("已处理 %1/%2 个分区后中止（%3）——%4")
                     .arg(index).arg(partitions.size()).arg(partition.name, error),
                 false);
            return;
        }
        if (outcome == FirehoseClient::EraseOutcome::Unverified) ++unverifiedCount;
        doneBytes += bytes;
        emit progressChanged(doneBytes, plannedBytes);
        emit overallProgressChanged(static_cast<quint64>(index + 1),
                                    static_cast<quint64>(partitions.size()));
        opEnd(true);
    }
    finish(QStringLiteral("批量擦除完成：%1 个分区%2")
               .arg(partitions.size())
               .arg(unverifiedCount == 0
                        ? QStringLiteral("（全部已回读校验）")
                        : QStringLiteral("（其中 %1 个结果未校验）").arg(unverifiedCount)));
}

void EdlWorker::rebootDevice(bool toEdl)
{
    const QString destination = toEdl ? QStringLiteral("9008 模式")
                                      : QStringLiteral("系统");
    if (!ensureReady(QStringLiteral("重启到%1").arg(destination))) return;
    setState(SessionState::Rebooting, QStringLiteral("重启到%1").arg(destination));
    emit progressChanged(0, 1);
    emit overallProgressChanged(0, 1);
    QString error;
    bool acknowledged = false;
    if (!m_firehose->reboot(toEdl, &error, &acknowledged)) {
        fail(error);
        return;
    }
    emit progressChanged(1, 1);
    emit overallProgressChanged(1, 1);
    closeSessionAfterReboot(destination, acknowledged);
}

void EdlWorker::closeSessionAfterReboot(const QString& destination, bool acknowledged)
{
    if (!acknowledged) {
        // fire-and-close：命令已写出但设备没回最终 ACK。不能承诺「设备一定在
        // 重启」——否则用户会拿这个前提去判断下一步还能不能发引导。
        emit logMessage(QStringLiteral(
            "重启命令已提交但未获设备确认（设备可能切换端口时直接断开）。"
            "若设备仍停留在 EDL，可直接重新点击连接重新下发引导。"));
    }
    m_firehose.reset();
    m_transport->close();
    m_deviceInfo = {};
    m_lunSectorCounts.clear();
    m_currentPartitions.clear();
    emit deviceInfoChanged(m_deviceInfo);
    emit partitionsReady(QVector<PartitionInfo>{});
    m_busy = false;
    setState(SessionState::Disconnected, QStringLiteral("设备正在重启到%1").arg(destination));
    emit operationFinished(true, QStringLiteral("重启到%1命令已提交%2")
                               .arg(destination,
                                    acknowledged ? QStringLiteral("（设备已确认）")
                                                 : QStringLiteral("（未获确认）")));
}

const PartitionInfo* EdlWorker::findPartition(const QStringList& names) const
{
    for (const QString& name : names) {
        for (const PartitionInfo& partition : m_currentPartitions) {
            if (partition.name.compare(name, Qt::CaseInsensitive) == 0) return &partition;
        }
    }
    return nullptr;
}

QVector<PartitionInfo> EdlWorker::matchPartitionsByPreset(const QStringList& preset) const
{
    QVector<PartitionInfo> matched;
    for (const PartitionInfo& partition : m_currentPartitions) {
        const QString normalized = normalizedPartitionName(partition.name);
        for (const QString& candidate : preset) {
            if (normalized.compare(candidate, Qt::CaseInsensitive) == 0) {
                matched.push_back(partition);
                break;
            }
        }
    }
    return matched;
}

void EdlWorker::rebootWithBcb(const QString& bcbCommand)
{
    // BCB 命令 → 人话目标名。只认这两个；别的命令 ensureReady 前就拒绝，
    // 绝不把一个看不懂的串写进 misc（写错目标的代价是设备起不来）。
    const QString destination = bcbCommand == QLatin1String("boot-fastboot")
        ? QStringLiteral("FastbootD")
        : QStringLiteral("Recovery");
    if (bcbCommand != QLatin1String("boot-recovery") &&
        bcbCommand != QLatin1String("boot-fastboot")) {
        emit operationFinished(false, QStringLiteral("不支持的 BCB 命令：%1").arg(bcbCommand));
        return;
    }
    if (!ensureReady(QStringLiteral("重启到%1").arg(destination))) return;
    const PartitionInfo* misc = findPartition({QStringLiteral("misc")});
    if (!misc) {
        fail(QStringLiteral("未找到 misc 分区——请先读取 GPT 分区表"), false);
        return;
    }
    const QByteArray payload = buildBcbPayload(bcbCommand);
    if (payload.isEmpty()) {
        fail(QStringLiteral("BCB 载荷构造失败：%1").arg(bcbCommand), false);
        return;
    }

    setState(SessionState::Writing, QStringLiteral("写入 BCB（重启到%1）").arg(destination));
    emit progressChanged(0, 1);
    emit overallProgressChanged(0, 1);
    QBuffer buffer;
    buffer.setData(payload);
    buffer.open(QIODevice::ReadOnly);
    QString error;
    if (!m_firehose->program(buffer, static_cast<quint64>(payload.size()),
                             misc->firstLba, misc->sectorCount(), misc->lun,
                             sectorSize(), &error, QStringLiteral("misc"),
                             QStringLiteral("bcb.bin"))) {
        fail(error);
        return;
    }
    emit logMessage(QStringLiteral("BCB 已写入 misc 分区（命令 %1），发送重启命令…")
                        .arg(bcbCommand));
    bool acknowledged = false;
    if (!m_firehose->reboot(false, &error, &acknowledged)) {
        fail(error);
        return;
    }
    emit progressChanged(1, 1);
    emit overallProgressChanged(1, 1);
    closeSessionAfterReboot(destination, acknowledged);
}

void EdlWorker::factoryReset()
{
    if (!ensureReady(QStringLiteral("恢复出厂"))) return;
    const QVector<PartitionInfo> targets =
        matchPartitionsByPreset(factoryResetPartitionNames());
    if (targets.isEmpty()) {
        fail(QStringLiteral("未找到 userdata / metadata 分区——请先读取 GPT 分区表"),
             false);
        return;
    }
    QString rangeError;
    if (!validateEraseRanges(targets, &rangeError)) {
        fail(rangeError, false);
        return;
    }
    emit logMessage(QStringLiteral(
        "恢复出厂：将擦除 %1 个分区（userdata / metadata），其余分区不动")
            .arg(targets.size()));
    runErasePartitions(targets);
}

void EdlWorker::enableOemUnlock()
{
    if (!ensureReady(QStringLiteral("强开 OEM 解锁"))) return;
    const PartitionInfo* devinfo = findPartition({QStringLiteral("devinfo")});
    if (!devinfo) {
        fail(QStringLiteral("未找到 devinfo 分区——该功能仅支持带 devinfo 的机型"),
             false);
        return;
    }
    setState(SessionState::Reading, QStringLiteral("读取 devinfo 分区"));
    emit overallProgressChanged(0, 1);
    QByteArray sector;
    QString error;
    if (!m_firehose->readToByteArray(&sector, devinfo->firstLba, 1, devinfo->lun,
                                     sectorSize(), &error, QStringLiteral("devinfo"),
                                     QStringLiteral("devinfo.bin"))) {
        fail(error);
        return;
    }
    const DevinfoUnlockResult result = applyDevinfoUnlock(&sector, &error);
    if (result == DevinfoUnlockResult::Rejected) {
        fail(error, false);
        return;
    }
    if (result == DevinfoUnlockResult::AlreadyUnlocked) {
        finish(QStringLiteral("OEM 解锁已处于开启状态，无需修改"));
        return;
    }

    setState(SessionState::Writing, QStringLiteral("写入 devinfo 解锁开关"));
    QBuffer buffer;
    buffer.setData(sector);
    buffer.open(QIODevice::ReadOnly);
    if (!m_firehose->program(buffer, static_cast<quint64>(sector.size()),
                             devinfo->firstLba, 1, devinfo->lun, sectorSize(), &error,
                             QStringLiteral("devinfo"), QStringLiteral("devinfo.bin"))) {
        fail(error);
        return;
    }
    emit logMessage(QStringLiteral("devinfo 解锁字节（偏移 0x%1）已置 1")
                        .arg(DevinfoUnlockByteOffset, 0, 16));
    bool acknowledged = false;
    if (!m_firehose->reboot(false, &error, &acknowledged)) {
        // 解锁字节已落盘：重启失败不否定解锁结果。报「失败」会让用户以为没生效，
        // 悄悄吞掉又丢了重启错误 —— 两者都取中间态：明说已开启、需手动重启。
        emit logMessage(QStringLiteral("警告：解锁字节已写入，但重启命令失败：%1")
                            .arg(error));
        finish(QStringLiteral("OEM 解锁已开启，但自动重启失败（%1）——请手动重启设备")
                   .arg(error));
        return;
    }
    closeSessionAfterReboot(QStringLiteral("系统"), acknowledged);
}

void EdlWorker::backupBasebandPartitions(const QString& outputDirectory)
{
    if (!ensureReady(QStringLiteral("备份基带分区"))) return;
    if (outputDirectory.isEmpty()) {
        fail(QStringLiteral("备份目录为空"), false);
        return;
    }
    const QVector<PartitionInfo> targets =
        matchPartitionsByPreset(basebandBackupPartitionNames());
    if (targets.isEmpty()) {
        fail(QStringLiteral("未找到基带相关分区（modem / modemst1 / modemst2 / fsg / "
                            "fsc / bluetooth）——请先读取 GPT 分区表"),
             false);
        return;
    }
    QDir directory(outputDirectory);
    if (!directory.exists() && !QDir().mkpath(outputDirectory)) {
        fail(QStringLiteral("创建备份目录失败：%1").arg(outputDirectory), false);
        return;
    }
    emit logMessage(QStringLiteral("基带备份：%1 个分区 → %2")
                        .arg(targets.size()).arg(outputDirectory));
    // 不生成 XML：这是抢救性备份，镜像原样落盘比可重刷 XML 更重要。
    runBackupPartitions(targets, outputDirectory, false);
}

void EdlWorker::readStorageInfo()
{
    if (!ensureReady(QStringLiteral("读取存储信息"))) return;
    setState(SessionState::Reading, QStringLiteral("读取存储信息"));
    const quint32 lunCount = m_storage == StorageType::Ufs ? 6U : 1U;
    emit overallProgressChanged(0, lunCount);
    quint64 totalBytes = 0;
    quint32 okCount = 0;
    for (quint32 lun = 0; lun < lunCount; ++lun) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            fail(QStringLiteral("操作已停止"));
            return;
        }
        emit progressChanged(0, 0);
        emit overallProgressChanged(lun, lunCount);
        quint64 bytes = 0;
        QString error;
        if (!m_firehose->getStorageInfo(lun, &bytes, &error)) {
            emit logMessage(QStringLiteral("LUN %1 存储信息读取失败：%2")
                                .arg(lun).arg(error));
            continue;
        }
        ++okCount;
        totalBytes = saturatingAdd(totalBytes, bytes);
        const QString type = m_firehose->detectedStorageType();
        const QString product = m_firehose->detectedStorageProduct();
        emit logMessage(QStringLiteral("LUN %1：%2 %3，容量 %4 字节（%5）")
                            .arg(lun)
                            .arg(type.isEmpty() ? QStringLiteral("存储") : type,
                                 product.isEmpty() ? QStringLiteral("型号未知") : product)
                            .arg(bytes)
                            .arg(describeBytes(bytes)));
        // 顺手把容量灌进缓存：后面写 GPT / 擦 LUN 还要用（resolveLunCapacity 命中即用）。
        if (bytes >= sectorSize()) {
            m_lunSectorCounts.insert(lun, bytes / sectorSize());
        }
    }
    emit overallProgressChanged(lunCount, lunCount);
    if (okCount == 0) {
        fail(QStringLiteral("所有 LUN 的存储信息均读取失败"), false);
        return;
    }
    finish(QStringLiteral("存储信息读取完成：%1/%2 个 LUN，合计 %3")
               .arg(okCount).arg(lunCount).arg(describeBytes(totalBytes)));
}

void EdlWorker::eraseLun(quint32 lun)
{
    if (!ensureReady(QStringLiteral("擦除磁盘 LUN"))) return;
    quint64 sectors = 0;
    if (!resolveLunCapacity(lun, &sectors)) {
        fail(QStringLiteral("无法获取 LUN %1 的容量——请先读取 GPT 分区表，"
                            "或确认该 LUN 存在").arg(lun),
             false);
        return;
    }
    setState(SessionState::Erasing, QStringLiteral("擦除 LUN %1（整盘）").arg(lun));
    const quint64 totalBytes = sectors * sectorSize();
    // 整盘按 2 GiB 分片：loader 对超大 num_partition_sectors 的支持不一致，
    // 分片同时让「停止」有机会在片间生效（单条命令一旦下去就停不下来）。
    quint64 sliceSectors = (2ULL << 30) / sectorSize();
    if (sliceSectors == 0) sliceSectors = sectors;
    emit progressChanged(0, totalBytes);
    emit overallProgressChanged(0, 1);
    m_firehose->setProgressCallback([this](quint64 current, quint64 total) {
        emit progressChanged(current, total);
    });

    const QString label = QStringLiteral("LUN %1").arg(lun);
    const QString filename = QStringLiteral("lun%1.bin").arg(lun);
    quint64 doneSectors = 0;
    quint64 unverified = 0;
    while (doneSectors < sectors) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            fail(QStringLiteral("操作已停止（已擦除 %1 / %2）")
                     .arg(describeBytes(doneSectors * sectorSize()),
                          describeBytes(totalBytes)),
                 false);
            return;
        }
        const quint64 count = qMin<quint64>(sliceSectors, sectors - doneSectors);
        QString error;
        const FirehoseClient::EraseOutcome outcome = m_firehose->erase(
            doneSectors, count, lun, sectorSize(), &error, label, filename);
        if (outcome == FirehoseClient::EraseOutcome::Failed) {
            fail(QStringLiteral("擦除 LUN %1 失败（已完成 %2）：%3")
                     .arg(lun).arg(describeBytes(doneSectors * sectorSize())).arg(error));
            return;
        }
        if (outcome == FirehoseClient::EraseOutcome::NotLanded) {
            fail(QStringLiteral("已擦除 %1 后中止（LUN %2）——%3")
                     .arg(describeBytes(doneSectors * sectorSize())).arg(lun).arg(error),
                 false);
            return;
        }
        if (outcome == FirehoseClient::EraseOutcome::Unverified) ++unverified;
        doneSectors += count;
        emit progressChanged(doneSectors * sectorSize(), totalBytes);
    }
    finish(QStringLiteral("LUN %1 整盘擦除完成（%2）%3")
               .arg(lun)
               .arg(describeBytes(totalBytes))
               .arg(unverified == 0
                        ? QStringLiteral("（全部已回读校验）")
                        : QStringLiteral("（其中 %1 个分片未校验）").arg(unverified)));
}

void EdlWorker::writeGptFile(const QString& path, quint32 lun)
{
    if (!ensureReady(QStringLiteral("写入 GPT"))) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("打开 GPT 文件失败：%1").arg(file.errorString()), false);
        return;
    }
    const qint64 fileSize = file.size();
    if (fileSize < 2 * static_cast<qint64>(sectorSize()) ||
        fileSize % static_cast<qint64>(sectorSize()) != 0) {
        fail(QStringLiteral("GPT 文件大小非法：%1 字节（需为 %2 的整数倍且至少 2 扇区）")
                 .arg(fileSize).arg(sectorSize()),
             false);
        return;
    }
    const QByteArray data = file.readAll();
    // fail-closed：LBA1 必须是 "EFI PART" 签名。选错文件（比如拿 rawprogram 当
    // GPT）会把分区表写花，而这类损坏在日志里毫无痕迹 —— 宁可拒绝。
    if (data.mid(static_cast<qsizetype>(sectorSize()), 8) !=
        QByteArrayLiteral("EFI PART")) {
        fail(QStringLiteral("不是有效的 GPT 文件：LBA1 缺少 EFI PART 签名"), false);
        return;
    }

    setState(SessionState::Writing,
             QStringLiteral("写入 GPT 到 LUN %1").arg(lun));
    emit progressChanged(0, 1);
    emit overallProgressChanged(0, 1);
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    const quint64 sectors = static_cast<quint64>(data.size()) / sectorSize();
    QString error;
    if (!m_firehose->program(buffer, static_cast<quint64>(data.size()), 0, sectors,
                             lun, sectorSize(), &error, QStringLiteral("GPT"),
                             QFileInfo(path).fileName())) {
        fail(error);
        return;
    }
    // 分区表已经变了：缓存的分区列表和 LUN 容量全部作废，逼后续操作重新读表。
    m_currentPartitions.clear();
    m_lunSectorCounts.remove(lun);
    emit partitionsReady(QVector<PartitionInfo>{});
    emit progressChanged(1, 1);
    emit overallProgressChanged(1, 1);
    finish(QStringLiteral("GPT 已写入 LUN %1（%2 扇区，来自 %3）——请重新读取分区表")
               .arg(lun)
               .arg(sectors)
               .arg(QFileInfo(path).fileName()));
}

void EdlWorker::readGptFile(const QString& path, quint32 lun)
{
    if (!ensureReady(QStringLiteral("读取 GPT"))) return;
    setState(SessionState::Reading, QStringLiteral("读取 LUN %1 的 GPT").arg(lun));
    emit progressChanged(0, 1);
    emit overallProgressChanged(0, 1);
    const quint64 gptSectors = m_storage == StorageType::Ufs ? 6ULL : 34ULL;
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        fail(QStringLiteral("创建 GPT 文件失败：%1").arg(output.errorString()), false);
        return;
    }
    QString error;
    if (!m_firehose->readToDevice(output, 0, gptSectors, lun, sectorSize(), &error,
                                  QStringLiteral("GPT"), QFileInfo(path).fileName())) {
        output.cancelWriting();
        fail(error);
        return;
    }
    if (m_cancelled.load(std::memory_order_relaxed)) {
        output.cancelWriting();
        fail(QStringLiteral("操作已停止，未提交备份文件"));
        return;
    }
    if (!output.commit()) {
        fail(QStringLiteral("保存 GPT 文件失败：%1").arg(output.errorString()), false);
        return;
    }
    emit progressChanged(1, 1);
    emit overallProgressChanged(1, 1);
    finish(QStringLiteral("LUN %1 的 GPT 已导出到 %2（%3 扇区）")
               .arg(lun)
               .arg(QDir::toNativeSeparators(path))
               .arg(gptSectors));
}

void EdlWorker::requestStop()
{
    m_cancelled.store(true, std::memory_order_relaxed);
}

} // namespace swt::edl
