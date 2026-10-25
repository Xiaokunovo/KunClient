#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ShellStatus — 外壳状态栏读数中枢（移植自 tests/src/KunClient.App/Services/ShellStatus.cs）。
//
//  线程模型：worker 线程直接调静态 setter（内部加锁写状态 + emit changed()），
//  changed() 跨线程连到 MainShell 时自动走 QueuedConnection，槽里再读 snapshot()。
//  状态只有一份、锁内一致 —— 不存在「两台探测互相打断」的读数抽风问题。
//
//  字段语义与 C# 逐字对齐：
//   · 无读数占位符是 "-"（不是空白）；
//   · report() 只传要改的字段（std::optional 空值 = 不动）；
//   · partitionPercent = -1 表示「单分区操作，第二条进度条别画」（绝不能 Clamp 成 0，
//     否则出现一条卡死的 0% 分区条，看起来像 bug —— C# 2026-10-02 同款修复）；
//   · endTask() 进度停在 100%，任务名/分区名保留（用户还要看「刚才做了什么」）；
//   · reset() 回空闲态。
// ─────────────────────────────────────────────────────────────────────────────

#include <QMutex>
#include <QObject>
#include <QString>
#include <optional>

namespace kc::ui {

class ShellStatus : public QObject {
    Q_OBJECT

public:
    struct Snapshot {
        QString device = QStringLiteral("-");
        QString mode = QStringLiteral("-");
        QString task = QStringLiteral("-");
        QString partition = QStringLiteral("-");
        QString speed = QStringLiteral("-");
        QString totalSize = QStringLiteral("-");
        QString hint = QStringLiteral("就绪");
        /// EDL 链路读数（2026-10-16 从内嵌 EDL 窗底部三格并入外壳状态栏）：
        /// · protocol —— 「协议通信状态：正常 / 异常」的取值；
        /// · link —— 「等待连接 / 已连接 / 连接失败」；
        /// · linkState —— idle | ok | failed，供 QSS 属性选择器给「状态:」段着色。
        QString protocol = QStringLiteral("正常");
        QString link = QStringLiteral("等待连接");
        QString linkState = QStringLiteral("idle");
        QString elapsed;
        double percent = 0;
        double partitionPercent = 0;
        bool partitionVisible = true;
        /// 是否有任务在跑（beginTask → true，endTask/reset → false）。
        /// 外壳据此决定「底部进度条要不要占位」：空闲时收起 —— 2026-10-12 用户口径，
        /// 主页空闲时那条 0.00% 的空进度条太碍眼。
        bool busy = false;
    };

    static ShellStatus& instance();

    /// 全量快照（锁内一致，MainShell 槽里取）。
    Snapshot snapshot() const;

    static void setDevice(const QString& device, const QString& mode, const QString& hint = {});
    /// EDL 链路三格上报（协议文本 / 链路文本 / 链路态 idle|ok|failed）。
    /// 由内嵌 EDL 窗的 updateState() 调用 —— 原先这三个值画在 EDL 窗自己的页脚，
    /// 2026-10-16 用户要求并进外壳状态栏，于是走这条中枢。
    static void setEdlLink(const QString& protocol, const QString& link,
                           const QString& linkState);
    static void beginTask(const QString& task);
    static void report(const std::optional<QString>& task = std::nullopt,
                       const std::optional<QString>& partition = std::nullopt,
                       const std::optional<QString>& speed = std::nullopt,
                       const std::optional<QString>& totalSize = std::nullopt,
                       const std::optional<double>& percent = std::nullopt,
                       const std::optional<double>& partitionPercent = std::nullopt);
    static void endTask();
    static void reset();
    static void setElapsed(const QString& elapsed);

signals:
    /// 任一读数变化。跨线程连接自动排队到接收方线程。
    void changed();

private:
    explicit ShellStatus();

    void raise();   // 锁外调用

    mutable QMutex m_mutex;
    Snapshot m_state;
};

} // namespace kc::ui
