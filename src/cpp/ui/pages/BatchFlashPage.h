#pragma once

#include "kc/tools/FlashQueue.h"
#include "ui/pages/BasePage.h"

#include <QStringList>

// ─────────────────────────────────────────────────────────────────────────────
//  BatchFlashPage —— 批量刷机队列页（2026-10-31）
//
//  场景：手上有几台机器要刷同一个包，或者一台一台刷同一个包的不同版本。
//  现在只能「连一台 → 刷 → 断开 → 再连下一台」，重复劳动，而且容易漏刷。
//
//  ── 为什么是「排队 + 逐个提示」而不是「同时刷多台」───────────────────────
//    本项目的 EDL 层是**单设备**架构（一个串口选择、一个 worker、一条连接）。
//    要同时刷多台得把整条链路重构成多实例 —— 那是伤筋动骨的改动，风险远大于
//    这个功能的收益。所以这里做的是「排好队，逐个提示你换设备」：
//    真正的价值是**不会漏、不用记**，而不是「并行加速」。
//
//  🔴 本页**不碰设备**：需要设备的动作一律 emit requestDeviceAction() 转交外壳。
//     与 ShortcutsPage 同一条路子 —— 页面拿不到内嵌 EDL 窗，也不该拿。
// ─────────────────────────────────────────────────────────────────────────────

class QLabel;
class QPushButton;
class QTableWidget;

namespace kc::ui {

class LogView;

class BatchFlashPage : public BasePage {
    Q_OBJECT

public:
    explicit BatchFlashPage(QWidget* parent = nullptr);

    /// 外壳告诉本页「当前连上的设备是谁」（换设备后调）。
    /// 用于把队列里 Pending 的项自动对到当前设备上。
    void setCurrentDevice(const QString& summary);

signals:
    /// 请求外壳执行一个设备动作（与 ShortcutsPage 同口径）。
    /// action: connect / flash / erase / reboot
    void requestDeviceAction(const QString& action, const QString& parameter);

private:
    void rebuildTable();
    void refreshSummary();

    /// 把「当前设备」加进队列（serial 从设备摘要里取）。
    void addCurrentDevice();
    /// 给队列里所有 Pending 项设同一个包。
    void setPackageForPending();
    /// 开始 / 继续：取下一个 Pending，转交外壳去刷。
    void startOrContinue();
    /// 把某一项标成成功 / 失败（由外壳的刷机结果回调）。
    void markItem(bool success, const QString& message);

public:
    /// 外壳在刷机结束后调用，把结果写回队列。
    void onFlashFinished(bool success, const QString& message);

private:
    kc::tools::FlashQueue m_queue;
    QString m_currentDevice;      ///< 当前连上的设备摘要
    QString m_currentSerial;      ///< 正在刷的那一项的序列号

    QTableWidget* m_table = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_deviceLabel = nullptr;
    QPushButton* m_addButton = nullptr;
    QPushButton* m_packageButton = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_resetButton = nullptr;
    QPushButton* m_clearButton = nullptr;
    LogView* m_log = nullptr;
};

} // namespace kc::ui
