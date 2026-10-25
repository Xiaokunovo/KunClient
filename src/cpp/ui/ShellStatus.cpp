#include "ui/ShellStatus.h"

#include <QDateTime>

namespace kc::ui {

namespace {

QString blank(const QString& value)
{
    return value.trimmed().isEmpty() ? QStringLiteral("-") : value.trimmed();
}

} // namespace

ShellStatus& ShellStatus::instance()
{
    static ShellStatus s;
    return s;
}

ShellStatus::ShellStatus() = default;

ShellStatus::Snapshot ShellStatus::snapshot() const
{
    QMutexLocker locker(&m_mutex);
    return m_state;
}

void ShellStatus::raise()
{
    emit changed();
}

void ShellStatus::setDevice(const QString& device, const QString& mode, const QString& hint)
{
    ShellStatus& s = instance();
    {
        QMutexLocker locker(&s.m_mutex);
        s.m_state.device = blank(device);
        s.m_state.mode = blank(mode);
        if (!hint.isEmpty()) s.m_state.hint = hint;
    }
    s.raise();
}

void ShellStatus::setEdlLink(const QString& protocol, const QString& link,
                             const QString& linkState)
{
    ShellStatus& s = instance();
    {
        QMutexLocker locker(&s.m_mutex);
        s.m_state.protocol = blank(protocol);
        s.m_state.link = blank(link);
        s.m_state.linkState = linkState.isEmpty() ? QStringLiteral("idle") : linkState;
    }
    s.raise();
}

void ShellStatus::beginTask(const QString& task)
{
    ShellStatus& s = instance();
    {
        QMutexLocker locker(&s.m_mutex);
        s.m_state.task = blank(task);
        s.m_state.partition = QStringLiteral("-");
        s.m_state.speed = QStringLiteral("-");
        s.m_state.totalSize = QStringLiteral("-");
        s.m_state.percent = 0;
        s.m_state.partitionVisible = true;   // 新任务默认显示第二条，首个 partition 上报可再收起
        s.m_state.busy = true;               // 任务开始 → 底部进度条占位
    }
    s.raise();
}

void ShellStatus::report(const std::optional<QString>& task, const std::optional<QString>& partition,
                         const std::optional<QString>& speed, const std::optional<QString>& totalSize,
                         const std::optional<double>& percent, const std::optional<double>& partitionPercent)
{
    ShellStatus& s = instance();
    bool dirty = false;
    {
        QMutexLocker locker(&s.m_mutex);
        Snapshot& st = s.m_state;
        if (task && *task != st.task) { st.task = blank(*task); dirty = true; }
        if (partition && *partition != st.partition) { st.partition = blank(*partition); dirty = true; }
        if (speed && *speed != st.speed) { st.speed = blank(*speed); dirty = true; }
        if (totalSize && *totalSize != st.totalSize) { st.totalSize = blank(*totalSize); dirty = true; }
        if (percent && qAbs(*percent - st.percent) >= 0.01) {
            st.percent = qBound(0.0, *percent, 100.0);
            dirty = true;
        }
        if (partitionPercent && qAbs(*partitionPercent - st.partitionPercent) >= 0.01) {
            // -1 =「单分区操作，第二条进度条别显示」：负值直接吃掉，绝不 Clamp（-1 → 0 会
            // 出现一条卡死的 0% 分区条，C# 2026-10-02 同款修复）。
            if (*partitionPercent < 0) {
                if (st.partitionVisible) { st.partitionVisible = false; dirty = true; }
            } else {
                if (!st.partitionVisible) { st.partitionVisible = true; dirty = true; }
                st.partitionPercent = qBound(0.0, *partitionPercent, 100.0);
                dirty = true;
            }
        }
    }
    if (dirty) s.raise();
}

void ShellStatus::endTask()
{
    ShellStatus& s = instance();
    {
        QMutexLocker locker(&s.m_mutex);
        s.m_state.speed = QStringLiteral("-");
        s.m_state.percent = 100;
        s.m_state.partitionPercent = 100;
        s.m_state.partitionVisible = true;
        s.m_state.busy = false;              // 任务结束 → 进度条收起（读数行仍留着终值）
    }
    s.raise();
}

void ShellStatus::reset()
{
    ShellStatus& s = instance();
    {
        QMutexLocker locker(&s.m_mutex);
        s.m_state.task = QStringLiteral("-");
        s.m_state.partition = QStringLiteral("-");
        s.m_state.speed = QStringLiteral("-");
        s.m_state.totalSize = QStringLiteral("-");
        s.m_state.elapsed.clear();
        s.m_state.percent = 0;
        s.m_state.partitionPercent = 0;
        s.m_state.partitionVisible = true;
        s.m_state.busy = false;
    }
    s.raise();
}

void ShellStatus::setElapsed(const QString& elapsed)
{
    ShellStatus& s = instance();
    {
        QMutexLocker locker(&s.m_mutex);
        s.m_state.elapsed = elapsed;
    }
    s.raise();
}

} // namespace kc::ui
