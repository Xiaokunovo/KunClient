#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  DeviceHomePage — 设备主页（设备总览）
//
//  移植自 tests/src/KunClient.App/Pages/DeviceHomePage.{xaml,xaml.cs}（2026-10-11
//  补齐 P2 缺口：21 项设备读数 + 快捷操作区）。
//   · 模式检测（ADB / Fastboot / EDL9008 / 未连接）并显示；
//   · 设备信息 21 项读数（ADB：型号/Android/版本/品牌/产品/代号/平台/内核/补丁/
//     基带/内存/分辨率/密度/槽位/A-B/电量/电池温度/存储/Root/引导/USB调试；
//     Fastboot：product + unlocked，其余占位；EDL/未连接：全占位）——
//     2026-10-16 由 QTableWidget 改为**三列键值网格**，收进一张卡，卡内滚动；
//   · 快捷操作：**模式自适配**的一组按钮（2026-10-12 用户口径「做成几个按钮
//     支持 adb fastboot，现在有的太杂了」）—— 原来 ADB 快捷 ×5 / Fastboot 快捷 ×5
//     / 系统快捷 ×4 三行，重启系统 / 重启Recovery / 重启Bootloader 各写两遍，
//     用户还得自己判断该点哪一行。现在一份按钮按当前模式自动下发 adb 或 fastboot
//     （DeviceOp → runDeviceOp）；仅 Fastboot 存在的解锁 BL / 引导状态会明确挡回。
//   · 免BL进EDL（adb reboot edl / fastboot oem edl + reboot edl，对齐 C#
//     BtnEnterEdl_Click：不实现读 token 免解 BL）；设备管理器（devmgmt.msc）；
//     命令行跳转（navigationRequested("CommandConsole")）；
//   · 自动刷新开关（5s 一轮）+ 手动刷新；UsbDeviceNotifier 插拔去抖重探。
//
//  检测与快捷操作在线程池上执行（adb/fastboot 同步阻塞），结果通过 runOnUi 回
//  UI 线程。日志区与其它页一致（LogView 线程安全 append*）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include "kc/devices/UsbStateStickiness.h"

#include <QHash>
#include <QPointer>
#include <QStringList>

class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;

namespace kc {
class DeviceWatcher;
}

namespace kc::ui {

class NotificationBar;

class DeviceHomePage : public BasePage {
    Q_OBJECT

public:
signals:
    /// 请求外壳跳转 —— 「命令行 ›」发 navigationRequested("CommandConsole")。
    void navigationRequested(const QString& key);

public:
    explicit DeviceHomePage(QWidget* parent = nullptr);
    ~DeviceHomePage() override;

    void onActivated() override;
    void onDeactivated() override;

private:
    enum class Mode { None, Adb, Recovery, Fastboot, Edl };

    /// 模式无关的设备动作：同一个按钮，ADB 走 adb、Fastboot 走 fastboot。
    enum class DeviceOp {
        Reboot,
        RebootRecovery,
        RebootBootloader,
        RebootEdl,
        RebootFastbootD,
        UnlockBl,      // 仅 Fastboot
        BootState,     // 仅 Fastboot
        EnterEdl,      // 复用 enterEdl()，含 fastboot oem edl 两段式
    };

    void buildUi();
    /// 设备读数区（2026-10-16 重做：QTableWidget → 三列键值网格，收进一张卡）。
    /// 返回该卡的容器控件；父级布局把它当普通 widget 挂上。
    QWidget* buildInfoCard();
    void refreshAsync();                      // 触发一轮检测（带去抖闸门）
    void detect();                            // 线程池执行体
    void applyInfo(const QHash<QString, QString>& values);   // UI 线程：回填读数
    void applyMode(Mode mode);                // UI 线程：更新模式显示
    void clearInfo();
    void setModeLabel(Mode mode);

    // 快捷操作（线程池执行 adb/fastboot；UI 线程只做日志三明治的起头）
    void runAdbReboot(const QString& args);
    void runFastbootOp(const QString& args);
    /// 单一入口：按当前模式把设备动作分派给 adb 或 fastboot。
    void runDeviceOp(DeviceOp op);
    /// fastboot getvar unlocked → 日志里给出「已解锁 / 已锁定」结论。
    void queryBootState();
    void enterEdl();
    void openDeviceManager();
    void toggleAutoRefresh();
    void runShellAsync(const QString& program, const QStringList& args,
                       const QString& section, const QString& doneHint);

    Mode m_mode = Mode::None;
    bool m_detectRunning = false;
    bool m_detectPending = false;
    bool m_autoRefreshOn = false;
    /// 连接状态粘滞：探测抖动时不让标签来回闪（模式切换立即采纳）。
    kc::UsbStateStickiness m_stickiness;
    /// 最近一次「抢 adb 服务（5037）」的结论。未连接时顶到提示行 ——
    /// 「5037 被别的 adb 占着」和「线没插好」在界面上长得一模一样，必须区分开。
    /// 设备一连上就清空，避免过时信息一直挂着。
    QString m_adbHint;

    NotificationBar* m_notify = nullptr;
    QLabel* m_modeLabel = nullptr;
    QLabel* m_hintLabel = nullptr;
    QLabel* m_dot = nullptr;
    /// 快捷操作卡右上角的提示：当前会走哪条通道（applyMode 里刷新）。
    QLabel* m_modeNote = nullptr;
    QGridLayout* m_infoGrid = nullptr;
    QPushButton* m_refreshButton = nullptr;
    QPushButton* m_autoRefreshButton = nullptr;
    QTimer* m_usbDebounce = nullptr;
    QTimer* m_autoRefresh = nullptr;

    DeviceWatcher* m_watcher = nullptr;

    // 读数区：key → 值标签（applyInfo / clearInfo 统一走这张表）。
    QStringList m_infoKeys;                   // 固定 21 项，决定网格填充顺序
    QHash<QString, QLabel*> m_infoValues;
};

} // namespace kc::ui
