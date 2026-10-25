#include "kc/diag/SessionLog.h"

#include "kc/config/AppStatePaths.h"
#include "kc/version.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSysInfo>

namespace kc::diag {
namespace {

constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;   // 单文件封顶
constexpr int kKeepDays = 7;                        // 保留期

QString dayOf(const QDateTime& t)
{
    return t.toString(QStringLiteral("yyyyMMdd"));
}

} // namespace

QString SessionLog::s_dirOverride;

SessionLog& SessionLog::instance()
{
    static SessionLog s;
    return s;
}

SessionLog::SessionLog()
{
    pruneOld();
}

SessionLog::~SessionLog()
{
    delete m_file;
}

QString SessionLog::logDir()
{
    if (!s_dirOverride.isEmpty()) {
        QDir().mkpath(s_dirOverride);
        return s_dirOverride;
    }
    const QString dir = QDir(AppStatePaths::Root()).filePath(QStringLiteral("logs"));
    QDir().mkpath(dir);
    return dir;
}

void SessionLog::setDirForTest(const QString& dir)
{
    s_dirOverride = dir;
}

QString SessionLog::currentFilePath() const
{
    QMutexLocker lock(&m_mutex);
    return m_path;
}

void SessionLog::pruneOld()
{
    QDir dir(logDir());
    const QFileInfoList files =
        dir.entryInfoList({ QStringLiteral("session-*.log") }, QDir::Files);
    const QDateTime deadline = QDateTime::currentDateTime().addDays(-kKeepDays);
    for (const QFileInfo& fi : files) {
        if (fi.lastModified() < deadline) QFile::remove(fi.absoluteFilePath());
    }
}

void SessionLog::append(const QDateTime& time, const QString& level, const QString& text)
{
    QMutexLocker lock(&m_mutex);

    const QString day = dayOf(time);
    if (!m_file || day != m_day) {
        // 跨日（或首行）：换文件。旧句柄先关掉再开新的。
        delete m_file;
        m_file = nullptr;
        m_headerWritten = false;
        m_path = QDir(logDir()).filePath(QStringLiteral("session-") + day +
                                         QStringLiteral(".log"));
        m_day = day;
        m_size = QFileInfo(m_path).size();
        m_file = new QFile(m_path);
        if (!m_file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            // 写不了日志不能影响主流程：静默降级为「不落盘」。
            delete m_file;
            m_file = nullptr;
            return;
        }
    }
    if (!m_file) return;
    if (m_size >= kMaxFileBytes) return;   // 单文件封顶：停止写入，不轮转

    if (!m_headerWritten) {
        writeSessionHeader();
        m_headerWritten = true;
    }

    const QString line = QStringLiteral("[%1] [%2] %3\n")
                             .arg(time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                  level, text);
    const QByteArray utf8 = line.toUtf8();
    m_file->write(utf8);
    m_file->flush();
    m_size += utf8.size();
}

void SessionLog::writeSessionHeader()
{
    if (!m_file) return;
    const QString header = QStringLiteral(
                               "==== KunClient %1 (EDL %2) | %3 | %4 | Qt %5 ====\n")
                               .arg(QString::fromLatin1(kc::kVersionString),
                                    QString::fromLatin1(kc::kEdlEngineVersion),
                                    QDateTime::currentDateTime().toString(
                                        QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                    QSysInfo::prettyProductName(),
                                    QString::fromLatin1(qVersion()));
    const QByteArray utf8 = header.toUtf8();
    m_file->write(utf8);
    m_file->flush();
    m_size += utf8.size();
}

} // namespace kc::diag
