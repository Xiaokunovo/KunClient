#pragma once

#include "kc/tools/ShortcutModel.h"
#include "ui/pages/BasePage.h"

// ─────────────────────────────────────────────────────────────────────────────
//  ShortcutsPage —— 快捷指令页（2026-10-29）
//
//  把「连设备 → 读 GPT → 备份」这类每天重复的步骤串成一条指令，点一次跑完。
//  模型层（kc/tools/ShortcutModel）只负责动作表 / 文本序列化 / 执行编排；这一层
//  负责三件事：
//   ① 编辑：左列指令表 + 右列「名称 / 标签 / 步骤表格」；
//   ② 持久化：整表以一行一条的文本存进 QSettings 的 `shortcuts/text`（用户可以直接
//      在设置目录里打开改，比嵌套 JSON 好读 —— 见 ShortcutModel.h 顶部注释）；
//   ③ 执行：ShortcutRunner 注入步骤回调，**跑在后台线程**（QThreadPool），
//      日志经 runOnUi 回到 UI 线程。
//
//  🔴 本页**不碰设备**：页面拿不到内嵌 EDL 窗，需要设备的动作（连接 / 读 GPT /
//     刷写 / 备份 / 重启）一律 `emit requestDeviceAction(action, parameter)` 转交
//     外壳，由外壳再派给 EDL 页。返回值算成功 —— 转交本身就是这一步的完成。
//     「转交了」和「做完了」在日志里必须写清楚，否则用户会以为备份已经落盘。
//
//  🔴 离线能做的只有两件：校验文件摘要（真算 SHA-256 并比对同名 .sha256）与
//     等待 N 秒。其余动作没有设备就无从谈起，不要在这里假装执行。
// ─────────────────────────────────────────────────────────────────────────────

class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;

namespace kc::ui {

class LogView;
class NotificationBar;

class ShortcutsPage : public BasePage {
    Q_OBJECT

public:
    explicit ShortcutsPage(QWidget* parent = nullptr);

    /// 外壳调用：运行指定名字的指令（辅助触控菜单 / 探针用）。找不到返回 false。
    bool runShortcutByName(const QString& name);

    /// 默认内置指令（首次使用时灌进 QSettings）：一条「标准备份流程」。
    static QVector<kc::tools::Shortcut> builtinShortcuts();

signals:
    /// 请求外壳执行「需要设备」的动作。
    /// action 取 shortcutActionName() 的中文名（「连接设备」/「读取 GPT 分区表」/
    /// 「回读全部分区」/「XML 刷机」/「重启设备」），parameter 可为空。
    /// 外壳负责转给 EDL 页 / 设备主页；本页不等结果 —— 快捷指令是编排，不是设备状态机。
    void requestDeviceAction(const QString& action, const QString& parameter);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void reloadList();
    void loadEditor(int index);
    void saveCurrentToModel();
    void saveToSettings();
    void addStepRow(const kc::tools::ShortcutStep& step);
    kc::tools::Shortcut currentEditorShortcut() const;
    void runCurrent();
    void appendLog(const QString& text);

    // ── 上面那组职责的拆分，外壳接线时不用关心 ────────────────────────────
    /// 编辑器当前显示的步骤（动作列是 cellWidget，取法与 currentEditorShortcut 共用）。
    QVector<kc::tools::ShortcutStep> currentEditorSteps() const;
    /// 清空步骤表（含动作列的下拉框，见 .cpp 里的说明）。
    void clearStepRows();
    /// 只刷新左列某一条的标题（改名 / 增删步骤后即时反映，不重建整个列表）。
    void refreshListItem(int index);
    /// 没有选中项时把编辑器整块置灰。
    void updateEditorEnabled();
    /// 「说明」列跟着动作列下拉走。
    void updateStepHint(int row);
    /// 上移 / 下移（delta = ±1）：整表重建，见 .cpp 里的说明。
    void moveStepRow(int delta);

    /// 在工作线程里执行一步（runCurrent 把它注入 ShortcutRunner）。
    /// 返回 false = 这一步失败，整条指令中止。
    bool executeStep(const kc::tools::ShortcutStep& step, QString* error);
    /// UI 线程收尾：复位运行态、把结果写进日志与通知条。
    void finishRun(const kc::tools::Shortcut& shortcut, const kc::tools::ShortcutRunResult& result);

    QVector<kc::tools::Shortcut> m_shortcuts;
    int m_current = -1;

    QListWidget* m_list = nullptr;
    QLineEdit* m_nameEdit = nullptr;
    QLineEdit* m_tagsEdit = nullptr;
    QTableWidget* m_steps = nullptr;
    QPushButton* m_runButton = nullptr;
    LogView* m_log = nullptr;

    NotificationBar* m_notify = nullptr;
    /// 编辑器那一块（名称 / 标签 / 步骤工具条 / 表格）。没有选中项时整块置灰，
    /// 免得用户对着空表点「上移」。
    QWidget* m_editorPane = nullptr;

    /// 后台执行中：挡住重复点「运行」（同一条指令并发跑两遍会互相抢设备）。
    bool m_running = false;

    /// 「正在往控件里灌数据」的守卫：loadEditor 会触发 textChanged / itemChanged /
    /// currentIndexChanged，不挡住的话每灌一格都会回写一次模型。
    bool m_syncing = false;
};

} // namespace kc::ui
