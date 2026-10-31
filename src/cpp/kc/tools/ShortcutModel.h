#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  ShortcutModel — 快捷指令：动作表 / 文本序列化 / 校验 / 执行编排
//
//  这是 iOS「快捷指令」的极简版：一串步骤按顺序跑完。**不支持分支与循环** ——
//  这个功能的全部价值是「省点几下」，把「连设备 → 读 GPT → 备份」这种每天重复
//  三遍的动作收成一次点击。一旦允许条件与跳转，配置就会变成没人敢改的脚本，
//  收益反而归零。
//
//  这一层**不碰设备**，只做四件纯逻辑的事：
//    ① 动作表：枚举 ↔ 中文名 ↔ 是否需要参数（界面下拉、导出、解析共用一份口径）；
//    ② 序列化：一行一条的自定义文本（为什么不用 JSON 见 formatShortcut 的注释）；
//    ③ 校验与「执行计划」文本（给人看的 `1. 连接设备` / `2. 回读全部分区 → D:/backup`）；
//    ④ 执行编排 ShortcutRunner：逐步回调 + **失败即中止**。
//
//  真正的设备动作由页面注入 StepFn（页面把动作转交给外壳 / EDL 页），所以本文件
//  只依赖 QtCore，可以脱离 UI 直接单测（见 tests/ 下同目录模块的写法）。
//
//  🔴 失败即中止是刻意的：刷机流程里「读 GPT 失败还继续刷」是灾难。runner 在第一个
//     返回 false 的步骤处立刻停下，completedSteps 就是「已经安全完成的步数」。
// ─────────────────────────────────────────────────────────────────────────────

namespace kc::tools {

/// 一个步骤能做的事。**顺序即执行顺序**，不支持分支 / 循环 ——
/// 快捷指令的价值是「省点几下」，不是变成编程语言。
enum class ShortcutAction {
    Connect,        ///< 连接设备（参数：无）
    ReadGpt,        ///< 读取 GPT 分区表（无）
    BackupAll,      ///< 回读全部分区（参数：输出目录）
    FlashXml,       ///< XML 刷机（参数：XML 路径）
    VerifyChecksum, ///< 校验文件摘要（参数：文件路径）
    Reboot,         ///< 重启设备（参数：system / edl）
    Wait,           ///< 等待 N 秒（参数：秒数）
};

/// 动作 → 中文名（用于界面与导出）。未知值返回空串。
QString shortcutActionName(ShortcutAction action);
/// 中文名 → 动作（界面读配置时用）。认不出返回 false。
bool shortcutActionFromName(const QString& name, ShortcutAction* out);
/// 全部动作（界面下拉用，顺序 = 枚举顺序）。
QVector<ShortcutAction> allShortcutActions();
/// 该动作是否需要参数。
bool shortcutActionNeedsParameter(ShortcutAction action);
/// 参数提示文案（如「输出目录」「XML 路径」）；不需要参数返回空串。
QString shortcutActionParameterHint(ShortcutAction action);

struct ShortcutStep {
    ShortcutAction action = ShortcutAction::Connect;
    QString parameter;
};

struct Shortcut {
    QString name;
    QStringList tags;          ///< 标签（「常用」「备份」…），界面用来分组
    QVector<ShortcutStep> steps;
};

/// 序列化：一行一条，格式 `名称 | 标签1,标签2 | 动作1:参数1; 动作2:参数2`
/// 为什么用自定义文本而不是 JSON：这份配置是给用户看 / 手改的（设置目录里直接打开），
/// 一行的可读性远胜嵌套 JSON，解析器也只要几十行。
///
/// 转义：名称 / 标签里的 `%` `|` `;` `:`（标签另加 `,`）与参数里的 `%` `|` `;`
/// 一律写成 `%25` `%7C` `%3B` `%3A`（`,` 写 `%2C`），解析时反转。不转义的话
/// 名字里一个 `|` 就会把整行切成四段，存进去就读不回来了。
/// `%` 必须**第一个**转：否则名字里本来就有的 "%7C" 会在解析时被还原成 "|"。
/// 名称 / 标签里的换行会被压成空格 —— 一行一条是这套格式的硬约束。
QString formatShortcut(const Shortcut& shortcut);
/// 解析一行；格式不合法返回 false 且不改 *out。
/// 宽容解析：标签段可以空（甚至整段省略，此时 `名称 | 步骤…` 也认），
/// 步骤段为空则返回 false（一条没有步骤的指令没有意义）；参数里允许出现 `:`
/// （只按第一个 `:` 切）。
bool parseShortcut(const QString& line, Shortcut* out);
/// 整表 ↔ 多行文本（每条一行，空行与 `#` 注释行跳过；解析不了的行静默丢弃，
/// 坏一行不该把整份配置带崩 —— 与 FlashHistory 的 JSONL 同一套宽容口径）。
QString formatShortcutList(const QVector<Shortcut>& shortcuts);
QVector<Shortcut> parseShortcutList(const QString& text);

/// 校验：名称为空 / 步骤为空 → 返回 false 并填中文原因。
/// ⚠️ 不校验参数是否填写：内置的「标准备份流程」里 BackupAll / VerifyChecksum
/// 就是空参数（运行时再向用户要路径），这里卡住会把内置指令直接判死。
bool validateShortcut(const Shortcut& shortcut, QString* error);

/// 把步骤表翻译成给人看的执行计划（每行一步，带序号）。
/// 例：`1. 连接设备` / `2. 回读全部分区 → D:/backup` / `3. 等待 5 秒`。
QStringList describePlan(const Shortcut& shortcut);

/// 执行结果。
struct ShortcutRunResult {
    bool ok = false;
    int completedSteps = 0;
    QString failedStep;    ///< 失败那一步的描述（空 = 全部成功）
    QString error;
};

/// 执行器：页面把「每个动作实际做什么」注入进来。
/// runStep 返回 false 表示该步失败，整条指令中止（**不继续后面步骤** —— 刷机流程里
/// 「读 GPT 失败还继续刷」是灾难）。
class ShortcutRunner {
public:
    using StepFn = std::function<bool(const ShortcutStep&, QString*)>;
    /// index 为**0 基**步骤序号（界面显示时 +1），total 为总步数。
    using ProgressFn = std::function<void(int index, int total, const QString& description)>;

    void setStepHandler(StepFn fn) { m_step = std::move(fn); }
    void setProgressHandler(ProgressFn fn) { m_progress = std::move(fn); }

    /// 同步执行（调用方负责放到工作线程或接受阻塞）。
    ShortcutRunResult run(const Shortcut& shortcut);

private:
    StepFn m_step;
    ProgressFn m_progress;
};

} // namespace kc::tools
