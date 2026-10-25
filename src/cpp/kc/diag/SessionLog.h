#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  SessionLog — 会话日志落盘（2026-10-12，「上传日志给开发者」的数据源）
//
//  LogView 的历史只进内存（MaxLines=2000 封顶、切页/关程序即丢），用户报障时
//  手上什么都没有。这里把每一行日志同步追加到按日切割的文件里：
//      %LOCALAPPDATA%\KunClientState\logs\session-YYYYMMDD.log
//  单文件 8MB 封顶（超出停止写入 —— 刷机页 TRACE 输出能把磁盘写满，保命优先），
//  保留 7 天，构造时清更老的。
//
//  线程安全：QMutex 保护内部状态。实际调用点都在 UI 线程
//  （LogView::enqueue 已把其它线程的行汇回 UI 线程）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QDateTime>
#include <QMutex>
#include <QString>

class QFile;

namespace kc::diag {

class SessionLog {
public:
    static SessionLog& instance();

    ~SessionLog();

    SessionLog(const SessionLog&) = delete;
    SessionLog& operator=(const SessionLog&) = delete;

    /// 追加一行。level 传展示用标签（"INFO"/"WARN"/…），time 为日志产生时间。
    void append(const QDateTime& time, const QString& level, const QString& text);

    /// 日志目录（不存在则创建）。
    static QString logDir();

    /// 当前会话日志文件路径（还没有产生任何日志时可能为空）。
    QString currentFilePath() const;

    /// 仅单测：把日志目录指到临时目录，避免测试写进真实用户状态目录。
    static void setDirForTest(const QString& dir);

private:
    SessionLog();

    void pruneOld();
    void writeSessionHeader();

    mutable QMutex m_mutex;
    QFile* m_file = nullptr;
    QString m_path;
    QString m_day;              // 当前文件对应的日子（yyyyMMdd）
    qint64 m_size = 0;
    bool m_headerWritten = false;
    static QString s_dirOverride;   // 仅测试用
};

} // namespace kc::diag
