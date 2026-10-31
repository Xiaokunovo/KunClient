#include "kc/tools/FlashQueue.h"

namespace kc::tools {
namespace {

/// 状态迁移是否合法。集中一处判断，免得八个公开方法各写一套口径。
bool isAllowedTransition(QueueState from, QueueState to)
{
    if (from == to) return false;   // 原地打转不算迁移

    switch (from) {
    case QueueState::Pending:
        return to == QueueState::Running || to == QueueState::Skipped;
    case QueueState::Running:
        return to == QueueState::Done || to == QueueState::Failed
               || to == QueueState::Pending;   // Pending = 中断回落（resetOne）
    case QueueState::Done:
    case QueueState::Failed:
    case QueueState::Skipped:
        // 终态只能靠 reset 回到 Pending，其余一律拒绝。
        return to == QueueState::Pending;
    }
    return false;
}

} // namespace

QString queueStateName(QueueState state)
{
    switch (state) {
    case QueueState::Pending: return QStringLiteral("等待");
    case QueueState::Running: return QStringLiteral("进行中");
    case QueueState::Done: return QStringLiteral("成功");
    case QueueState::Failed: return QStringLiteral("失败");
    case QueueState::Skipped: return QStringLiteral("已跳过");
    }
    return {};
}

bool queueStateFromName(const QString& name, QueueState* out)
{
    if (!out) return false;

    // 中文名与英文名都认：中文给界面/导出，英文给日志/配置（不受编码影响）。
    const QString text = name.trimmed();
    QueueState parsed;
    if (text == QStringLiteral("等待") || text == QStringLiteral("pending")) {
        parsed = QueueState::Pending;
    } else if (text == QStringLiteral("进行中") || text == QStringLiteral("running")) {
        parsed = QueueState::Running;
    } else if (text == QStringLiteral("成功") || text == QStringLiteral("done")) {
        parsed = QueueState::Done;
    } else if (text == QStringLiteral("失败") || text == QStringLiteral("failed")) {
        parsed = QueueState::Failed;
    } else if (text == QStringLiteral("已跳过") || text == QStringLiteral("skipped")) {
        parsed = QueueState::Skipped;
    } else {
        return false;
    }

    *out = parsed;   // 只在认出来之后写，失败路径不碰调用方的值
    return true;
}

bool FlashQueue::transition(int index, QueueState to, const QString& message)
{
    if (index < 0 || index >= m_items.size()) return false;
    QueueItem& item = m_items[index];
    if (!isAllowedTransition(item.state, to)) return false;

    item.state = to;
    // message 的归属：进 Running 时清掉上一次的失败原因（重试场景）；
    // 其余迁移由调用方决定写什么（传空就清空）。
    item.message = message;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  入队 / 出队
// ─────────────────────────────────────────────────────────────────────────────

bool FlashQueue::add(const QueueItem& item)
{
    const QString serial = item.serial.trimmed();
    // 没有序列号就定位不到机器；批量刷机里「刷了谁」必须可追溯。
    if (serial.isEmpty()) return false;
    // 同一台机器排两次，两次的 Done/Failed 会互相覆盖，现场分不清结果。
    if (indexOf(serial) >= 0) return false;

    QueueItem entry = item;
    entry.serial = serial;
    entry.state = QueueState::Pending;   // 入队一律从 Pending 起，防止外部塞个 Done 进来
    entry.message.clear();
    m_items.push_back(entry);
    return true;
}

bool FlashQueue::remove(const QString& serial)
{
    const int index = indexOf(serial);
    if (index < 0) return false;
    // 运行中的项不能删：worker 还在跑，回报回来会找不到落点。
    if (m_items.at(index).state == QueueState::Running) return false;
    m_items.removeAt(index);
    return true;
}

void FlashQueue::clear()
{
    m_items.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
//  取下一个 / 推进状态
// ─────────────────────────────────────────────────────────────────────────────

int FlashQueue::nextIndex() const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).state == QueueState::Pending) return i;
    }
    return -1;
}

bool FlashQueue::next(QueueItem* out)
{
    const int index = nextIndex();
    if (index < 0) return false;

    m_items[index].state = QueueState::Running;
    m_items[index].message.clear();
    if (out) *out = m_items.at(index);
    return true;
}

bool FlashQueue::markRunning(const QString& serial)
{
    return transition(indexOf(serial), QueueState::Running, QString());
}

bool FlashQueue::markDone(const QString& serial, const QString& message)
{
    return transition(indexOf(serial), QueueState::Done, message);
}

bool FlashQueue::markFailed(const QString& serial, const QString& message)
{
    // 🔴 只改这一项：不中断、不清空、不移动其它项。下一台该刷的照刷 ——
    // 多设备批量和「快捷指令的单流程失败即中止」是两种作业单位，别合并口径。
    return transition(indexOf(serial), QueueState::Failed, message);
}

bool FlashQueue::skip(const QString& serial)
{
    return transition(indexOf(serial), QueueState::Skipped, QString());
}

bool FlashQueue::resetOne(const QString& serial)
{
    return transition(indexOf(serial), QueueState::Pending, QString());
}

void FlashQueue::reset()
{
    for (QueueItem& item : m_items) {
        item.state = QueueState::Pending;
        item.message.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  查询
// ─────────────────────────────────────────────────────────────────────────────

int FlashQueue::indexOf(const QString& serial) const
{
    const QString key = serial.trimmed();
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).serial == key) return i;
    }
    return -1;
}

bool FlashQueue::item(const QString& serial, QueueItem* out) const
{
    const int index = indexOf(serial);
    if (index < 0) return false;
    if (out) *out = m_items.at(index);
    return true;
}

QueueSummary FlashQueue::summary() const
{
    QueueSummary s;
    s.total = m_items.size();
    for (const QueueItem& item : m_items) {
        switch (item.state) {
        case QueueState::Pending:
        case QueueState::Running:
            break;   // 不算进任何一列，但已经在 total 里
        case QueueState::Done: ++s.done; break;
        case QueueState::Failed: ++s.failed; break;
        case QueueState::Skipped: ++s.skipped; break;
        }
    }
    return s;
}

bool FlashQueue::hasPending() const
{
    return nextIndex() >= 0;
}

} // namespace kc::tools
