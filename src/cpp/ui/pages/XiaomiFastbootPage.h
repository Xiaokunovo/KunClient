#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  XiaomiFastbootPage — 小米线刷（类 MiFlash）
//
//  移植自 tests/src/KunClient.App/Pages/XiaomiFastbootPage.{xaml,xaml.cs}。
//  C# 原页核心流程：
//   · 选 ROM：解压后的线刷包目录（根目录含 flash_all.bat + images），或直接选
//     官方 .tgz（自动 extractTarGz 解压到同目录，解压后处理「带一层根目录」的包）；
//   · DetectRom：扫 flash_all 三档脚本（有/缺）+ images 目录镜像计数；
//   · 刷写方式三档（与 MiFlash 一致）：
//       全部清除 = flash_all.bat（清数据）
//       保留用户数据 = flash_all_except_storage.bat
//       全部清除并回锁 = flash_all_lock.bat（清数据 + 回锁 BL，二次确认带砖头警告）
//   · 开始刷写：确认 → 确保平台工具 → fastboot devices 在位 → 统计脚本里
//     `fastboot … flash …` 行数作进度分母 → executeScriptStreaming 流式跑 bat，
//     输出逐行进日志；行内出现 FAILED 判失败；"Finished. Total time" 计一步推进进度；
//     Sending/Writing 'xxx' 更新当前阶段读数。
//   · 停止：原子取消标志 → executeScriptStreaming 终止子进程。
//  WPF 的 MembershipService 扣额度 / DeviceStateHub 广播不移植。
//
//  脚本执行走 QThreadPool（同步阻塞实现），日志经 LogView 线程安全 append* 直写，
//  控件更新过 runOnUi。忙碌闸门 + 停止。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QAtomicInt>
#include <QString>
#include <QStringList>
#include <QVector>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;

namespace kc::ui {

class NotificationBar;

class XiaomiFastbootPage : public BasePage {
    Q_OBJECT

public:
    explicit XiaomiFastbootPage(QWidget* parent = nullptr);

    void onActivated() override;

private:
    /// 刷写方式 → 官方脚本映射（与 C# ScriptOptions 三档一致）。
    struct ScriptOption {
        QString title;
        QString file;
        bool wipesData;
        bool locksBl;
    };

    void buildUi();
    void detectRom();
    void browseFolder();
    void browseTgz();
    void detectDevice();
    void startFlash();
    void stopFlash();
    void setBusy(bool busy);
    void setProgress(int percent);
    void setStage(const QString& text);

    // ── 线程池执行体（阻塞实现，禁止在 UI 线程调用）────────────────────────
    void detectDeviceCore(qint64 startedMs);
    void flashCore(const QString& scriptPath, const QString& scriptTitle, const QString& romDir);

    NotificationBar* m_notify = nullptr;
    QLineEdit* m_romPathEdit = nullptr;
    QLabel* m_romInfoLabel = nullptr;
    QLabel* m_deviceInfoLabel = nullptr;
    QComboBox* m_scriptCombo = nullptr;     // 三档刷写方式（与 C# CmbScript 一致）
    QProgressBar* m_progress = nullptr;
    QLabel* m_pctLabel = nullptr;
    QLabel* m_stageLabel = nullptr;

    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_browseFolderButton = nullptr;
    QPushButton* m_browseTgzButton = nullptr;
    QPushButton* m_rescanButton = nullptr;
    QPushButton* m_detectButton = nullptr;
    QVector<QPushButton*> m_busyButtons;

    int m_scriptIndex = 0;                  // 当前刷写方式下标（循环切换）
    QString m_romDir;
    QAtomicInt m_busy{0};
    QAtomicInt m_cancel{0};
};

} // namespace kc::ui
