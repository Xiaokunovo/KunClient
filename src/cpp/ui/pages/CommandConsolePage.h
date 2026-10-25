#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  CommandConsolePage — 命令控制台
//
//  移植自 tests/src/KunClient.App/Pages/CommandConsolePage.{xaml,xaml.cs}。
//  C# 原版是「每条命令开一个 cmd.exe /c」的一次性执行；本移植按交互控制台的
//  形态落地（任务要求）：**每 种模式一个持久 QProcess**，输出流式进只读控制台：
//   · adb shell —— adb shell 交互会话；
//   · fastboot —— fastboot 交互会话（stdin 逐条喂）；
//   · 原样执行(cmd) —— cmd.exe /Q /K（chcp 65001，UTF-8 输出）。
//  共同点与 C# 一致：
//   · 子进程 PATH 前置本工具自带的 platform-tools 目录（adb/fastboot 免安装）；
//   · ↑/↓ 翻命令历史；连续重复的命令不重复记；
//   · 控制台最多保留 3000 块（防 logcat 类长输出堆爆内存）；
//   · 停止 / 清屏 / 系统CMD / 检测工具（工具缺失时从服务器拉取）。
//  模式切换：杀掉当前会话进程并重启新模式的会话。
//  QProcess 读写全走信号槽（主线程事件循环），不需要线程池。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QPointer>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;

namespace kc::ui {

class NotificationBar;

class CommandConsolePage : public BasePage {
    Q_OBJECT

public:
    explicit CommandConsolePage(QWidget* parent = nullptr);
    ~CommandConsolePage() override;

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();

    // ── 会话管理 ───────────────────────────────────────────────────────────
    void startSession(bool announce);
    void stopSession(const QString& reason);
    void onModeChanged();
    void sendInput();
    void navigateHistory(int delta);

    // ── 控制台输出 ─────────────────────────────────────────────────────────
    void appendOut(const QString& text);

    // ── 工具就绪（检测工具按钮 / 首次进入页面静默跑一次）──────────────────
    void checkTools(bool silent);

    QString currentMode() const;   // "adb" / "fastboot" / "cmd"
    void setHint(const QString& text);
    void setSessionButtons();

    NotificationBar* m_notify = nullptr;
    QPlainTextEdit* m_console = nullptr;
    QComboBox* m_modeCombo = nullptr;
    QLineEdit* m_inputEdit = nullptr;
    QPushButton* m_runButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_restartButton = nullptr;
    QPushButton* m_checkToolsButton = nullptr;
    QLabel* m_hintLabel = nullptr;

    QPointer<QProcess> m_proc;
    QString m_procMode;                 // 正在跑的会话模式（与 currentMode() 同口径）
    QStringList m_history;
    int m_historyIndex = 0;
    bool m_toolsReady = false;
    bool m_checkedOnce = false;
    QAtomicInt m_checking{0};
};

} // namespace kc::ui
