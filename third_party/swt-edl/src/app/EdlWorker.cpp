#include "app/EdlWorker.h"

#include "core/DeviceIdentity.h"
#include "core/PartitionProtection.h"
#include "protocol/Firehose.h"
#include "protocol/Sahara.h"
#include "storage/GptParser.h"
#include "storage/DynamicSuper.h"
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
#include <limits>
#include <utility>

namespace swt::edl {
namespace {

constexpr quint64 XmlSliceBytes = 64ULL * 1024ULL * 1024ULL;

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
        for (quint64 index = 0; index < count; ++index) {
            const unsigned shift = static_cast<unsigned>(((m_position + index) & 3ULL) * 8ULL);
            data[index] = static_cast<char>((m_value >> shift) & 0xFFU);
        }
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
                for (quint64 index = 0; index < count; ++index) {
                    const unsigned shift = static_cast<unsigned>(((within + index) & 3ULL) * 8ULL);
                    data[produced + index] = static_cast<char>(
                        (extent.fillValue >> shift) & 0xFFU);
                }
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

bool resolveSectorExpression(QString expression, quint64 diskSectors, quint64* value)
{
    expression = expression.trimmed();
    while (expression.endsWith(QLatin1Char('.'))) expression.chop(1);
    const QString token = QStringLiteral("NUM_DISK_SECTORS");
    if (!expression.startsWith(token, Qt::CaseInsensitive) || diskSectors == 0) {
        return false;
    }
    QString remainder = expression.mid(token.size()).trimmed();
    if (remainder.isEmpty()) {
        *value = diskSectors;
        return true;
    }
    const QChar operation = remainder.front();
    remainder.removeFirst();
    while (remainder.endsWith(QLatin1Char('.'))) remainder.chop(1);
    bool ok = false;
    const quint64 operand = remainder.trimmed().toULongLong(&ok);
    if (!ok) return false;
    if (operation == QLatin1Char('-') && operand <= diskSectors) {
        *value = diskSectors - operand;
        return true;
    }
    if (operation == QLatin1Char('+') &&
        operand <= std::numeric_limits<quint64>::max() - diskSectors) {
        *value = diskSectors + operand;
        return true;
    }
    return false;
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
            if (!m_firehose->getStorageInfo(0, &storageError)) {
                emit logMessage(QStringLiteral("Firehose 存储尚未就绪，自动执行 Configure"));
                setState(SessionState::Configuring);
                if (!m_firehose->configure(storage, &error)) {
                    fail(error);
                    return;
                }
                storageError.clear();
                if (!m_firehose->getStorageInfo(0, &storageError)) {
                    emit logMessage(QStringLiteral("存储信息查询跳过：%1").arg(storageError));
                }
            } else {
                emit logMessage(QStringLiteral("Firehose 存储已配置，可直接读取分区表"));
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
    } else {
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
    if (!m_firehose->getStorageInfo(0, &infoError)) {
        emit logMessage(QStringLiteral("存储信息查询跳过：%1").arg(infoError));
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
                                 : QStringLiteral(" / %1").arg(m_deviceInfo.storageProduct)));
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
                break;
            }
            continue;
        }
        if (gpt.size() >= static_cast<qsizetype>(sectorSize()) + 40) {
            const quint64 backupLba = qFromLittleEndian<quint64>(
                reinterpret_cast<const uchar*>(gpt.constData() + sectorSize() + 32));
            if (backupLba > 0 && backupLba < std::numeric_limits<quint64>::max()) {
                m_lunSectorCounts.insert(lun, backupLba + 1);
            }
        }
        QVector<PartitionInfo> partitions;
        if (!GptParser::parse(gpt, sectorSize(), lun, &partitions, &error)) {
            emit logMessage(QStringLiteral("LUN %1 GPT：%2").arg(lun).arg(error));
            lastError = error;
            continue;
        }
        allPartitions += partitions;
    }

    if (allPartitions.isEmpty()) {
        fail(lastError.isEmpty() ? QStringLiteral("没有读取到有效 GPT") : lastError);
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
        emit logMessage(QStringLiteral("读取 %1/%2：%3（LUN %4）→ %5")
                            .arg(index + 1).arg(partitions.size())
                            .arg(partition.name).arg(partition.lun).arg(outputName));

        QSaveFile output(outputPath);
        if (!output.open(QIODevice::WriteOnly)) {
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
            fail(QStringLiteral("读取分区 %1 失败：%2").arg(partition.name, error));
            return;
        }
        if (!output.commit()) {
            fail(QStringLiteral("保存备份失败：%1").arg(output.errorString()), false);
            return;
        }
        completed = saturatingAdd(completed, partition.sectorCount() * sectorSize());
        emit progressChanged(partitionBytes, partitionBytes);
        emit overallProgressChanged(completed, totalBytes);
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
    if (!ensureReady(QStringLiteral("写入分区"))) return;
    setState(SessionState::Writing, QStringLiteral("写入 %1").arg(partition.name));
    emit overallProgressChanged(0, 1);

    QFile input(imagePath);
    if (!input.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("打开镜像失败：%1").arg(input.errorString()), false);
        return;
    }

    QString error;
    if (!m_firehose->program(input, input.size(), partition.firstLba,
                             partition.sectorCount(), partition.lun, sectorSize(), &error,
                             partition.name, QFileInfo(imagePath).fileName())) {
        fail(error);
        return;
    }
    emit overallProgressChanged(1, 1);
    finish(QStringLiteral("分区 %1 写入完成").arg(partition.name));
}

void EdlWorker::flashPartitions(const QVector<PartitionInfo>& requestedPartitions,
                                const QStringList& requestedImagePaths,
                                bool allowProtected)
{
    if (!ensureReady(QStringLiteral("批量写入分区"))) return;
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

    quint64 totalBytes = 0;
    QVector<bool> sparseImages;
    sparseImages.reserve(imagePaths.size());
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
        emit logMessage(QStringLiteral("写入 %1/%2：%3 → %4（LUN %5）")
                            .arg(index + 1).arg(partitions.size())
                            .arg(QFileInfo(imagePath).fileName(), partition.name)
                            .arg(partition.lun));
        QString error;
        if (sparseImages.at(index)) {
            SparseImageInfo sparseInfo;
            if (!SparseImage::inspect(input, &sparseInfo, &error)) {
                fail(QStringLiteral("%1：%2").arg(QFileInfo(imagePath).fileName(), error), false);
                return;
            }
            emit logMessage(QStringLiteral("Android Sparse：%1 个块，展开 %2 字节")
                                .arg(sparseInfo.totalBlocks).arg(sparseInfo.expandedSize()));
            for (const SparseExtent& extent : sparseInfo.extents) {
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
                fail(QStringLiteral("写入分区 %1 失败：%2").arg(partition.name, error));
                return;
            }
            completed = saturatingAdd(completed, imageBytes);
            emit progressChanged(imageBytes, imageBytes);
            emit overallProgressChanged(completed, totalBytes);
        }
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
    quint64 rangeCompleted = 0;
    while (rangeCompleted < byteCount) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            if (error) *error = QStringLiteral("操作已停止");
            return false;
        }
        const quint64 chunkBytes = qMin(XmlSliceBytes, byteCount - rangeCompleted);
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

void EdlWorker::flashXml(const QStringList& xmlPaths, bool allowProtected)
{
    if (!ensureReady(QStringLiteral("XML 刷机"))) return;

    RawProgramPlan plan;
    QString error;
    if (!RawProgramParser::parse(xmlPaths, &plan, &error)) {
        fail(error, false);
        return;
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
        return resolveSectorExpression(entry.startSectorExpression,
                                       m_lunSectorCounts.value(entry.lun, 0),
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

    quint64 totalBytes = 0;
    for (const RawProgramEntry& entry : plan.entries) {
        quint64 resolvedStart = 0;
        if (!entryStartSector(entry, &resolvedStart)) {
            fail(QStringLiteral("LUN %1 容量未知，未能解析动态扇区：%2。请重新读取分区表")
                     .arg(entry.lun).arg(entry.startSectorExpression), false);
            return;
        }
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
            quint64 sparseTransferBytes = 0;
            for (const SparseExtent& extent : sparseInfo.extents) {
                if (extent.type == SparseExtentType::Skip) continue;
                sparseTransferBytes = saturatingAdd(
                    sparseTransferBytes, extent.blockCount * sparseInfo.blockSize);
            }
            totalBytes = saturatingAdd(totalBytes, sparseTransferBytes);
        } else {
            const quint64 offset = entry.fileSectorOffset * entry.sectorSize;
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
    emit logMessage(QStringLiteral("XML 计划已载入：%1 个 XML，%2 个 program 条目")
                        .arg(plan.xmlPaths.size()).arg(plan.entries.size()));
    for (const QString& warning : plan.warnings) emit logMessage(warning);
    if (plan.hasSplitSuper()) {
        emit logMessage(QStringLiteral("检测到 %1 个 super 条目：启用免合并分段刷写")
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
            emit logMessage(QStringLiteral(
                "当前 super.img 为空：开启免合并写入（META），LP %1.%2，%3 个槽位，%4 个镜像")
                .arg(dynamicPlan.metadataMajorVersion).arg(dynamicPlan.metadataMinorVersion)
                .arg(dynamicPlan.metadataSlotCount).arg(dynamicPlan.images.size()));
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
                        extent.byteCount % entry.sectorSize != 0) {
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
            continue;
        }

        QFile file(entry.sourcePath);
        if (!file.open(QIODevice::ReadOnly)) {
            restoreProgress();
            fail(QStringLiteral("打开镜像失败：%1").arg(file.errorString()), false);
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
            emit logMessage(QStringLiteral("Android sparse：%1 个块，展开 %2 字节，不生成临时 raw 文件")
                                .arg(sparseInfo.totalBlocks).arg(sparseInfo.expandedSize()));
            for (const SparseExtent& extent : sparseInfo.extents) {
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
            const quint64 diskSectors = m_lunSectorCounts.value(static_cast<quint32>(lun), 0);
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

void EdlWorker::erasePartition(const PartitionInfo& partition)
{
    if (!ensureReady(QStringLiteral("擦除分区"))) return;
    setState(SessionState::Erasing, QStringLiteral("擦除 %1").arg(partition.name));
    emit progressChanged(0, 1);
    emit overallProgressChanged(0, 1);

    QString error;
    if (!m_firehose->erase(partition.firstLba, partition.sectorCount(), partition.lun,
                           sectorSize(), &error, partition.name,
                           partition.name + QStringLiteral(".bin"))) {
        fail(error);
        return;
    }
    emit progressChanged(1, 1);
    emit overallProgressChanged(1, 1);
    finish(QStringLiteral("分区 %1 擦除完成").arg(partition.name));
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
    setState(SessionState::Erasing,
             QStringLiteral("批量擦除 %1 个分区").arg(partitions.size()));
    m_firehose->setProgressCallback({});
    emit progressChanged(0, 1);
    emit overallProgressChanged(0, static_cast<quint64>(partitions.size()));
    for (qsizetype index = 0; index < partitions.size(); ++index) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            fail(QStringLiteral("操作已停止"), false);
            return;
        }
        const PartitionInfo& partition = partitions.at(index);
        emit progressChanged(0, 1);
        emit logMessage(QStringLiteral("擦除 %1/%2：%3（LUN %4，起始扇区 %5）")
                            .arg(index + 1).arg(partitions.size())
                            .arg(partition.name).arg(partition.lun).arg(partition.firstLba));
        QString error;
        if (!m_firehose->erase(partition.firstLba, partition.sectorCount(), partition.lun,
                               sectorSize(), &error, partition.name,
                               partition.name + QStringLiteral(".bin"))) {
            fail(QStringLiteral("擦除分区 %1 失败：%2").arg(partition.name, error));
            return;
        }
        emit progressChanged(1, 1);
        emit overallProgressChanged(static_cast<quint64>(index + 1),
                                    static_cast<quint64>(partitions.size()));
    }
    m_firehose->setProgressCallback([this](quint64 current, quint64 total) {
        emit progressChanged(current, total);
    });
    finish(QStringLiteral("批量擦除完成：%1 个分区").arg(partitions.size()));
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
    if (!m_firehose->reboot(toEdl, &error)) {
        fail(error);
        return;
    }
    emit progressChanged(1, 1);
    emit overallProgressChanged(1, 1);
    m_firehose.reset();
    m_transport->close();
    m_deviceInfo = {};
    m_lunSectorCounts.clear();
    m_currentPartitions.clear();
    emit deviceInfoChanged(m_deviceInfo);
    emit partitionsReady(QVector<PartitionInfo>{});
    m_busy = false;
    setState(SessionState::Disconnected, QStringLiteral("设备正在重启到%1").arg(destination));
    emit operationFinished(true, QStringLiteral("重启到%1命令已发送").arg(destination));
}

void EdlWorker::requestStop()
{
    m_cancelled.store(true, std::memory_order_relaxed);
}

} // namespace swt::edl
