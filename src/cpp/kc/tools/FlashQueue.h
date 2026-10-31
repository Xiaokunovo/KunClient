#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FlashQueue —— 批量刷机队列（2026-10-31）
//
//  多设备批量刷机的「排程 + 记账」层。UI 把待刷的设备排进队列，worker 一台一台
//  取、一台一台回报，界面照着 state 渲染。
//
//  这一层**不碰设备、不碰文件、不碰定时器**：它只维护一张表和一个游标，回答
//  「下一个该刷谁」「这台刷完了该记什么」。真正的刷机由页面拿 serial + packagePath
//  去走现有的单机入口。这样整块逻辑可以脱离 UI 直接单测（见 tests/FlashQueueTest.cpp）。
//
//  🔴 失败**不阻塞**后续 —— 这是与「快捷指令」刻意不同的口径，别改：
//     · 快捷指令（ShortcutModel）是**单设备**的一条流程：「读 GPT → 备份 → 刷写」，
//       前一步失败后面全都没意义，继续跑只会把设备搞成半砖，所以失败即中止。
//     · 这里是**多设备批量**：10 台机器里第 3 台失败，没有理由让剩下 7 台干等。
//       一台失败只把那一台标 Failed，游标继续往后走 —— 现场的人要的是「能刷的
//       先刷完」，失败的机器单独拎出来重试（reset 后重新入队）。
//     两个模块的「失败」语义不同，是因为它们面对的是**不同的作业单位**：一条流程
//     vs 一批机器。合并口径会让其中一边变得危险。
//
//  状态机（只允许这些迁移，其余一律拒绝并返回 false）：
//      Pending  → Running / Skipped
//      Running  → Done / Failed / Pending（markRunning 被打断时用 reset 回落）
//      Done / Failed / Skipped → Pending（仅 reset / resetOne）
//  「拒绝」而不是「静默接受」：UI 的按钮可用性靠它判断，静默接受会把竞态藏起来。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QVector>

namespace kc::tools {

/// 队列项状态。**顺序即界面排序权重**（Pending 在最上）。
enum class QueueState {
    Pending,   ///< 待处理（唯一能被 next() 取到的状态）
    Running,   ///< 正在刷
    Done,      ///< 成功
    Failed,    ///< 失败（不阻塞后续）
    Skipped,   ///< 用户主动跳过
};

/// 状态 → 中文名（界面直接用）。未知值返回空串。
QString queueStateName(QueueState state);
/// 中文名 → 状态（读配置 / 日志回放时用）。认不出返回 false 且不改 *out。
bool queueStateFromName(const QString& name, QueueState* out);

struct QueueItem {
    QString serial;       ///< 设备序列号（批量场景的唯一键）
    QString model;        ///< 机型，可为空（界面只做展示）
    QString packagePath;  ///< 要刷的包（XML / 镜像 / 目录）
    QueueState state = QueueState::Pending;
    QString message;      ///< 失败原因 / 完成信息，由 markDone / markFailed 写入
};

/// 汇总。total 恒等于四项之和。
struct QueueSummary {
    int total = 0;
    int done = 0;
    int failed = 0;
    int skipped = 0;
};

class FlashQueue {
public:
    FlashQueue() = default;

    // ── 入队 / 出队 ──────────────────────────────────────────────────────

    /// 追加一项（state 强制为 Pending，message 清空）。
    /// serial 为空 → 拒绝（批量场景没有序列号就无法定位是哪台机器）。
    /// 序列号重复 → 拒绝（同一台机器排两次会互相覆盖结果）。
    bool add(const QueueItem& item);
    /// 按序列号删除。运行中的项**拒绝删除**（刷到一半删掉会让 worker 的回报
    /// 找不到落点）。找不到返回 false。
    bool remove(const QString& serial);

    /// 清空整张表（含运行中的项）。只给「取消整批」用。
    void clear();

    // ── 取下一个 / 推进状态 ──────────────────────────────────────────────

    /// 下一个 Pending 项的下标；没有则 -1。**只看 Pending**：Running 正在跑、
    /// Done/Failed/Skipped 都已终结 —— 失败项因此天然不会挡住后面的项。
    int nextIndex() const;
    /// 取下一个 Pending 项并**就地**置为 Running。没有可取的返回 false。
    /// 成功时 *out 被填成该项（含 serial / packagePath），可直接拿去刷。
    bool next(QueueItem* out);

    /// 把指定项置 Running（外部已经自行挑好项时用）。状态不对返回 false。
    bool markRunning(const QString& serial);
    /// 置 Done 并写 message。只有 Running 项能被标完成。
    bool markDone(const QString& serial, const QString& message = QString());
    /// 置 Failed 并写 message。**不影响队列里其它项**（见文件头注释）。
    bool markFailed(const QString& serial, const QString& message);
    /// 用户主动跳过：Pending → Skipped。运行中不能跳过（先取消再跳过）。
    bool skip(const QString& serial);
    /// 把一项从任何终态 / 运行态拉回 Pending（重试用）。message 清空。
    bool resetOne(const QString& serial);
    /// 全部重置为 Pending 并清 message（整批重刷）。
    void reset();

    // ── 查询 ────────────────────────────────────────────────────────────

    /// 全部项，保持入队顺序。
    const QVector<QueueItem>& items() const { return m_items; }
    int size() const { return m_items.size(); }
    bool isEmpty() const { return m_items.isEmpty(); }

    /// 按序列号找下标；找不到 -1。
    int indexOf(const QString& serial) const;
    /// 按序列号取一项（拷贝）；找不到返回 false 且不改 *out。
    bool item(const QString& serial, QueueItem* out) const;

    /// 汇总：total / done / failed / skipped。
    /// Running 与 Pending 不进任何一列，但算进 total（total = 全部项数）。
    QueueSummary summary() const;

    /// 是否还有 Pending（即「还能不能继续刷」）。Running 不算 —— 它已经在跑了。
    bool hasPending() const;

private:
    /// 状态迁移的唯一入口：非法迁移返回 false 且不改任何字段。
    bool transition(int index, QueueState to, const QString& message);

    QVector<QueueItem> m_items;
};

} // namespace kc::tools
