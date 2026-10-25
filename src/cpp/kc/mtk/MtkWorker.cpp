#include "kc/mtk/MtkWorker.h"

#include "kc/mtk/MtkDeviceSession.h"
#include "kc/mtk/MtkXflash.h"

#include <QFile>
#include <QSaveFile>
#include <QtEndian>

#include <limits>
#include <memory>
#include <vector>

namespace kc::mtk {
namespace {

QString humanBytes(quint64 bytes)
{
    static const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(bytes)
                     : QStringLiteral("%1 %2").arg(value, 0, 'f', 2).arg(QLatin1String(units[unit]));
}

bool validateJobs(const QVector<MtkJob>& jobs, QVector<DaTarget>* targets,
                  int* failedRow, QString* error)
{
    if (jobs.isEmpty()) {
        *error = QStringLiteral("作业列表为空");
        return false;
    }
    for (const MtkJob& job : jobs) {
        *failedRow = job.row;
        DaTarget target;
        if (!regionToTarget(job.region, job.storage, &target)) {
            *error = QStringLiteral("%1：无法把 region「%2」映射到 DA 目标").arg(job.name, job.region);
            return false;
        }
        if (job.size == 0 || job.addr % 512 != 0 || job.size % 512 != 0
            || job.size > static_cast<quint64>(std::numeric_limits<qint64>::max())
            || job.size > std::numeric_limits<quint64>::max() - job.addr) {
            *error = QStringLiteral("%1：分区容量为零、未按 512 对齐或地址/长度溢出").arg(job.name);
            return false;
        }
        targets->append(target);
    }
    *failedRow = -1;
    return true;
}

bool isSparse(QFile& file)
{
    const QByteArray magic = file.peek(4);
    return magic.size() == 4 && qFromLittleEndian<quint32>(magic.constData()) == 0xED26FF3Au;
}

} // namespace

MtkWorker::MtkWorker(QObject* parent)
    : QObject(parent)
{
}

MtkWorker::~MtkWorker()
{
    delete m_session;
}

void MtkWorker::closeSession()
{
    delete m_session;
    m_session = nullptr;
    m_daReady = false;
    resetCancel();
}

void MtkWorker::releaseFailedSession()
{
    if (m_session) {
        delete m_session;
        m_session = nullptr;
        emit logLine(QStringLiteral("warning"),
                     QStringLiteral("MTK 会话已释放，禁止复用失败/取消后的 RAW 通道；请重新连接设备"));
    }
    m_daReady = false;
}

void MtkWorker::openAndBoot(const QString& comPort, const QString& daPath, const QString& preloaderPath)
{
    resetCancel();
    m_comPort = comPort;
    if (!daPath.isEmpty()) m_daPath = daPath;
    m_preloaderPath = preloaderPath;
    delete m_session;
    m_session = nullptr;
    m_daReady = false;

    QString error;
    m_session = MtkDeviceSession::openBrom(comPort, &error, [this] { return cancelled(); });
    if (!m_session) {
        emit logLine(QStringLiteral("error"), error);
        emit connectionResult(false, 0, comPort, error);
        return;
    }
    m_session->setCancelFn([this] { return cancelled(); });
    connect(m_session, &MtkDeviceSession::logLine, this, &MtkWorker::logLine);
    connect(m_session, &MtkDeviceSession::progressChanged, this, &MtkWorker::progressChanged);
    emit logLine(QStringLiteral("success"),
                 QStringLiteral("BROM 握手成功：%1（HW code 0x%2）")
                     .arg(comPort).arg(m_session->hwCode(), 4, 16, QLatin1Char('0')));
    emit connectionResult(true, m_session->hwCode(), comPort, QString());

    if (m_daPath.isEmpty()) {
        emit logLine(QStringLiteral("info"), QStringLiteral("尚未指定 DA 文件，无法刷写分区"));
        return;
    }
    const bool ok = m_session->bootDa(m_daPath, m_preloaderPath, &error);
    if (!ok && cancelled()) error = QStringLiteral("已取消");
    m_daReady = ok;
    if (!ok) releaseFailedSession();
    emit daResult(ok, ok ? QString() : error);
}

void MtkWorker::bootDa(const QString& daPath, const QString& preloaderPath)
{
    resetCancel();
    if (!daPath.isEmpty()) m_daPath = daPath;
    m_preloaderPath = preloaderPath;
    QString error;
    const bool ok = ensureReady(&error);
    m_daReady = ok;
    if (!ok) emit logLine(QStringLiteral("error"), error);
    emit daResult(ok, ok ? QString() : error);
}

bool MtkWorker::ensureReady(QString* error)
{
    if (cancelled()) {
        if (error) *error = QStringLiteral("已取消");
        releaseFailedSession();
        return false;
    }
    if (m_session && m_session->xflash() && !m_session->xflash()->isUsable())
        releaseFailedSession();
    if (!m_session) {
        if (m_comPort.isEmpty()) {
            if (error) *error = QStringLiteral("尚未连接设备");
            return false;
        }
        m_session = MtkDeviceSession::openBrom(m_comPort, error, [this] { return cancelled(); });
        if (!m_session) return false;
        m_session->setCancelFn([this] { return cancelled(); });
        connect(m_session, &MtkDeviceSession::logLine, this, &MtkWorker::logLine);
        connect(m_session, &MtkDeviceSession::progressChanged, this, &MtkWorker::progressChanged);
    }
    if (!m_session->daReady()) {
        if (m_daPath.isEmpty()) {
            if (error) *error = QStringLiteral("刷写分区需要 DA 高速通道，请先选择 DA 文件");
            return false;
        }
        if (!m_session->bootDa(m_daPath, m_preloaderPath, error)) {
            if (cancelled() && error) *error = QStringLiteral("已取消");
            releaseFailedSession();
            return false;
        }
    }
    m_daReady = true;
    return true;
}

void MtkWorker::writeJobs(const QVector<MtkJob>& jobs)
{
    resetCancel();
    QString error;
    int failedRow = -1;
    QVector<DaTarget> targets;
    auto fail = [&](const QString& message, int row) {
        if (row >= 0) emit rowStatus(row, QStringLiteral("失败"));
        emit logLine(QStringLiteral("error"), message);
        emit jobsFinished(false, message);
    };
    if (!validateJobs(jobs, &targets, &failedRow, &error)) {
        fail(error, failedRow);
        return;
    }

    // 整批镜像在任何 WRITE_DATA 之前检查并保持打开，避免后续路径被替换。
    struct Source {
        std::unique_ptr<QFile> file;
        quint64 size;
    };
    std::vector<Source> sources;
    sources.reserve(static_cast<size_t>(jobs.size()));
    for (const MtkJob& job : jobs) {
        if (cancelled()) {
            releaseFailedSession();
            fail(QStringLiteral("已取消（写入前预检）"), job.row);
            return;
        }
        auto file = std::make_unique<QFile>(job.filePath);
        if (job.filePath.isEmpty() || !file->open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("%1：无法读取镜像 %2").arg(job.name, job.filePath), job.row);
            return;
        }
        const qint64 length = file->size();
        if (length <= 0 || file->isSequential()
            || static_cast<quint64>(length) > static_cast<quint64>(std::numeric_limits<qint64>::max()) - 511) {
            fail(QStringLiteral("%1：镜像为空或长度无效").arg(job.name), job.row);
            return;
        }
        const quint64 size = static_cast<quint64>(length);
        const quint64 padded = (size + 511) & ~quint64(511);
        if (padded > job.size || padded > std::numeric_limits<quint64>::max() - job.addr) {
            fail(QStringLiteral("%1：镜像含 512 补齐后为 %2 字节，超过分区容量 %3")
                     .arg(job.name).arg(padded).arg(job.size), job.row);
            return;
        }
        if (file->peek(qMin<qint64>(4, length)).size() != qMin<qint64>(4, length)) {
            fail(QStringLiteral("%1：无法读取镜像头").arg(job.name), job.row);
            return;
        }
        if (isSparse(*file)) {
            fail(QStringLiteral("%1：不支持 Android sparse 镜像，请先解码为 raw；不可将 sparse 容器直接写入")
                     .arg(job.name), job.row);
            return;
        }
        sources.push_back(Source{std::move(file), size});
    }
    if (!ensureReady(&error)) {
        fail(error, -1);
        return;
    }

    int done = 0;
    for (int i = 0; i < jobs.size(); ++i) {
        const MtkJob& job = jobs.at(i);
        Source& source = sources.at(static_cast<size_t>(i));
        if (cancelled()) {
            releaseFailedSession();
            fail(QStringLiteral("已取消（完成 %1/%2）").arg(done).arg(jobs.size()), job.row);
            return;
        }
        if (source.file->size() != static_cast<qint64>(source.size)
            || !source.file->seek(0) || isSparse(*source.file)) {
            fail(QStringLiteral("%1：镜像在预检后发生变化或无法定位").arg(job.name), job.row);
            return;
        }
        const DaTarget& target = targets.at(i);
        if (job.row >= 0) emit rowStatus(job.row, QStringLiteral("写入中…"));
        emit logLine(QStringLiteral("info"),
                     QStringLiteral("[%1] 开始写入：0x%2，%3 字节 → %4（parttype=%5）")
                         .arg(job.name).arg(job.addr, 16, 16, QLatin1Char('0'))
                         .arg(source.size).arg(target.label).arg(target.partType));
        if (!m_session->xflash()->writeFromDevice(job.addr, source.size, target.storage,
                                                   target.partType, source.file.get(), &error)) {
            if (cancelled()) error = QStringLiteral("已取消");
            releaseFailedSession();
            fail(QStringLiteral("%1：%2").arg(job.name, error), job.row);
            return;
        }
        ++done;
        if (job.row >= 0) emit rowStatus(job.row, QStringLiteral("完成"));
        emit logLine(QStringLiteral("success"),
                     QStringLiteral("[%1] 写入完成（%2 字节，末扇区补零）").arg(job.name).arg(source.size));
    }
    emit jobsFinished(true, QStringLiteral("下载完成：%1 个分区").arg(done));
}

void MtkWorker::readJobs(const QVector<MtkJob>& jobs)
{
    resetCancel();
    QString error;
    int failedRow = -1;
    QVector<DaTarget> targets;
    auto fail = [&](const QString& message, int row) {
        if (row >= 0) emit rowStatus(row, QStringLiteral("失败"));
        emit logLine(QStringLiteral("error"), message);
        emit jobsFinished(false, message);
    };
    if (!validateJobs(jobs, &targets, &failedRow, &error)) {
        fail(error, failedRow);
        return;
    }
    for (const MtkJob& job : jobs) {
        if (job.filePath.isEmpty()) {
            fail(QStringLiteral("%1：读回目标路径为空").arg(job.name), job.row);
            return;
        }
    }
    if (!ensureReady(&error)) {
        fail(error, -1);
        return;
    }
    int done = 0;
    for (int i = 0; i < jobs.size(); ++i) {
        const MtkJob& job = jobs.at(i);
        if (cancelled()) {
            releaseFailedSession();
            fail(QStringLiteral("已取消（完成 %1/%2）").arg(done).arg(jobs.size()), job.row);
            return;
        }
        QSaveFile out(job.filePath);
        out.setDirectWriteFallback(false);
        if (!out.open(QIODevice::WriteOnly)) {
            fail(QStringLiteral("%1：无法原子保存到 %2：%3")
                     .arg(job.name, job.filePath, out.errorString()), job.row);
            return;
        }
        const DaTarget& target = targets.at(i);
        if (job.row >= 0) emit rowStatus(job.row, QStringLiteral("读取中…"));
        emit logLine(QStringLiteral("info"),
                     QStringLiteral("[%1] 开始读回：0x%2，%3 字节 ← %4")
                         .arg(job.name).arg(job.addr, 16, 16, QLatin1Char('0'))
                         .arg(job.size).arg(target.label));
        if (!m_session->xflash()->readToDevice(job.addr, job.size, target.storage,
                                                target.partType, &out, &error)) {
            if (cancelled()) error = QStringLiteral("已取消");
            out.cancelWriting();
            releaseFailedSession();
            fail(QStringLiteral("%1：%2").arg(job.name, error), job.row);
            return;
        }
        if (cancelled()) {
            out.cancelWriting();
            releaseFailedSession();
            fail(QStringLiteral("已取消（未提交读回文件）"), job.row);
            return;
        }
        if (!out.commit()) {
            fail(QStringLiteral("%1：读回文件提交失败：%2").arg(job.name, out.errorString()), job.row);
            return;
        }
        ++done;
        if (job.row >= 0) emit rowStatus(job.row, QStringLiteral("已读回"));
        emit logLine(QStringLiteral("success"),
                     QStringLiteral("[%1] 读回 %2 字节 → %3").arg(job.name).arg(job.size).arg(job.filePath));
    }
    emit jobsFinished(true, QStringLiteral("读回完成：%1 个分区").arg(done));
}

void MtkWorker::formatJobs(const QVector<MtkJob>& jobs)
{
    resetCancel();
    QString error;
    int failedRow = -1;
    QVector<DaTarget> targets;
    auto fail = [&](const QString& message, int row) {
        if (row >= 0) emit rowStatus(row, QStringLiteral("失败"));
        emit logLine(QStringLiteral("error"), message);
        emit jobsFinished(false, message);
    };
    if (!validateJobs(jobs, &targets, &failedRow, &error)) {
        fail(error, failedRow);
        return;
    }
    if (!ensureReady(&error)) {
        fail(error, -1);
        return;
    }
    int done = 0;
    for (int i = 0; i < jobs.size(); ++i) {
        const MtkJob& job = jobs.at(i);
        if (cancelled()) {
            releaseFailedSession();
            fail(QStringLiteral("已取消（完成 %1/%2）").arg(done).arg(jobs.size()), job.row);
            return;
        }
        const DaTarget& target = targets.at(i);
        if (job.row >= 0) emit rowStatus(job.row, QStringLiteral("擦除中…"));
        emit logLine(QStringLiteral("warning"),
                     QStringLiteral("[%1] 开始擦除：0x%2，%3 → %4（parttype=%5）")
                         .arg(job.name).arg(job.addr, 16, 16, QLatin1Char('0'))
                         .arg(humanBytes(job.size)).arg(target.label).arg(target.partType));
        if (!m_session->xflash()->format(job.addr, job.size, target.storage, target.partType, &error)) {
            if (cancelled()) error = QStringLiteral("已取消");
            releaseFailedSession();
            fail(QStringLiteral("%1：%2").arg(job.name, error), job.row);
            return;
        }
        ++done;
        if (job.row >= 0) emit rowStatus(job.row, QStringLiteral("已擦除"));
        emit logLine(QStringLiteral("success"),
                     QStringLiteral("[%1] 擦除完成（%2）").arg(job.name).arg(humanBytes(job.size)));
    }
    emit jobsFinished(true, QStringLiteral("擦除完成：%1 个分区").arg(done));
}

} // namespace kc::mtk
