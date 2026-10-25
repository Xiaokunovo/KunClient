#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FileTransferPage — 文件传输（ADB push / pull）
//
//  移植自 tests/src/KunClient.App/Pages/FileTransferPage.{xaml,xaml.cs}。
//  C# 原页核心（此处保留）：
//   · 推送：本地文件 → 设备路径（adb push "local" "remote"）；
//   · 拉取：设备路径 → 本地文件（保存对话框，adb pull "remote" "local"）；
//   · 批量安装 APK：目录选择器 → 逐个 `install -r`，按输出含 Success 计数；
//   · 设备文件浏览：`shell ls -la "{path}"` 逐行进列表，上级目录回退；
//   · 拖放传输：拖入文件批量 push（一批一次口径）；
//   · 传输进度：不确定进度条 + 状态文字；
//   · 设备存储余量：`shell df /storage/emulated/0` 解析可用空间（C# 无此读数，
//     Qt 版补上 —— 任务要求；C# 同页没有 df 调用）；
//   · 远端路径历史：QComboBox 可编辑下拉记录最近用过的路径（C# 是纯输入框，
//     Qt 版按任务要求补历史行）。
//  C# 的 MembershipService 额度守卫与 DeviceStateHub/ShellStatus 左栏同步不移植。
//
//  所有 adb 调用走 QThreadPool（PlatformToolsManager 是同步阻塞实现，超时 600s），
//  控件更新过 runOnUi。忙碌闸门：任一传输进行中禁用全部动作按钮。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QListWidget;

namespace kc::ui {

class NotificationBar;

class FileTransferPage : public BasePage {
    Q_OBJECT

public:
    explicit FileTransferPage(QWidget* parent = nullptr);

    void onActivated() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildUi();
    void pushFiles(const QStringList& files);      // 按钮单文件 / 拖放多文件共用
    void pullFile();
    void batchInstall();
    void browseDevice();
    void refreshStorage();

    // ── 线程池执行体（阻塞实现，禁止在 UI 线程调用）────────────────────────
    void runAdbCore(const QString& adbArgs, const QString& desc, int timeoutMs);
    void pushFilesCore(const QStringList& files, const QString& remote);
    void pullCore(const QString& remote, const QString& local);
    void batchInstallCore(const QString& dir);
    void browseCore(const QString& path);
    void storageCore();

    void setBusy(bool busy);
    void setProgress(bool busy, const QString& text);   // UI 线程
    void rememberHistory(QComboBox* combo, const QString& text);

    NotificationBar* m_notify = nullptr;
    QLineEdit* m_pushLocal = nullptr;
    QComboBox* m_pushRemote = nullptr;
    QComboBox* m_pullRemote = nullptr;
    QLineEdit* m_batchDir = nullptr;
    QLineEdit* m_devicePath = nullptr;
    QListWidget* m_deviceFiles = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QLabel* m_progressText = nullptr;
    QLabel* m_storageValue = nullptr;
    QLabel* m_dropZone = nullptr;

    QVector<QPushButton*> m_busyButtons;
    QAtomicInt m_busy{0};
};

} // namespace kc::ui
