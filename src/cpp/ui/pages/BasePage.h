#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  BasePage — 所有页面基类
//
//  移植自 tests/src/KunClient.App/Services/BasePage.cs（172 行）。
//  提供页面通用能力，页面子类不用各自实现一遍：
//   · 页面内通知条（NotificationBar）访问 —— notify() / success() / error() / hideNotify()；
//   · 页面日志目标（LogView）访问 —— logInfo() / logSuccess() / logWarning() / logError()；
//   · 跨线程 UI 分发 —— runOnUi()（后台线程回调里读写控件前必须过这里，
//     QWidget 不是线程安全的，跨线程直接摸控件 = 随机崩溃 / 绘制错乱）；
//   · 搜索/分区工具栏开关（右侧胶囊按钮展开 / 收起输入区）；
//   · 生命周期钩子 onActivated() / onDeactivated()（页面被显示 / 隐藏时调用）；
//   · 标题 setTitle() / title()。
//
//  ⚠️ C# 的 FindName("NotifyBar") / FindName("TxtLog") 通过可视树查找控件，
//     Qt 里换成子类在构造函数里 **注册**（registerNotifyBar / registerLogView），
//     或直接依赖 pageHeader() 内置的通知条与日志。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QWidget>

#include <functional>

class QHBoxLayout;
class QPushButton;
class QLineEdit;

namespace kc::ui {

class NotificationBar;
class PageHeaderBar;
class LogView;

class BasePage : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle)

public:
    explicit BasePage(QWidget* parent = nullptr);
    ~BasePage() override;

    /// 页面标题（外壳侧栏 / 页头显示）。
    QString title() const { return m_title; }
    virtual void setTitle(const QString& title);

    // ── 通知条 ────────────────────────────────────────────────────────────
    /// 页面通知条（可能为空，见 registerNotifyBar 注释）。仅在 UI 线程读。
    NotificationBar* notifyBar() const { return m_notifyBar; }
    /// 页面子类在构造期把通知条注册进来（等价于 C# 的 FindName("NotifyBar")）。
    void registerNotifyBar(NotificationBar* bar) { m_notifyBar = bar; }

    /// 一条通知（text 为空则忽略）。level 见 NotifyLevel。
    /// 内部已切 UI 线程，可在任意线程调用。
    void notify(const QString& text, int level, int autoHideMs = 4000);

    // ── 日志 ──────────────────────────────────────────────────────────────
    /// 页面日志框（等价于 C# 的 FindName("TxtLog")）。仅在 UI 线程读。
    LogView* logView() const { return m_logView; }
    /// 页面子类在构造期把日志框注册进来。
    void registerLogView(LogView* view) { m_logView = view; }

    // ── 生命周期 ──────────────────────────────────────────────────────────
    /// 页面被外壳显示时调用（进入页面 / 切回该页）。
    /// 外壳必须在合适时机调用它 —— 页面缓存常驻，切回来不会再走 showEvent 的首次路径。
    virtual void onActivated() {}
    /// 页面被外壳隐藏时调用。
    virtual void onDeactivated() {}

    // ── 外壳状态上报（可选）────────────────────────────────────────────────
    /// 上报设备 / 模式 / 提示给外壳底部状态栏。无外壳时安全忽略。
    void setShellStatus(const QString& device, const QString& mode, const QString& hint = QString());

signals:
    /// 标题变化（外壳据此刷新侧栏 / 页头）。
    void windowTitleChanged(const QString& title);
    /// 设备 / 模式读数变化，外壳据此刷新底部状态栏。
    void shellStatusChanged(const QString& device, const QString& mode, const QString& hint);

public slots:
    /// 跨线程 UI 分发：已在 UI 线程则同步执行，否则排队到 UI 线程执行。
    void runOnUi(std::function<void()> action);

protected:
    /// 「搜索分区」胶囊按钮的开关行为：btn->property("target") 写它要开关的控件，
    /// 可见则收起并清空其中输入框（取消过滤），不可见则展开并聚焦。
    void searchToggle(QPushButton* button);

    /// 给页面铺一张玻璃底（2026-10-17）。
    ///
    /// 为什么要有这个：一批功能页（MTK / 应用管理 / 可视刷写 / 小米 EDL / 命令行）
    /// 的布局是「裸行直接进 QVBoxLayout」—— 按钮、输入框、读数全部浮在壁纸上，
    /// 一条 `#card` 都没有，用户看到的就是「这页没实现液态玻璃」。
    /// 逐页包卡片改动大、还容易和已有布局打架；这里给整页铺一张与卡片同材质的
    /// 玻璃板，控件落在这张板子上，观感立刻统一。
    ///
    /// ⚠️ 只给**没有卡片**的平铺页开；已经有 `#card` 的页面开了会玻璃叠玻璃。
    void setGlassSheet(bool on);
    bool hasGlassSheet() const { return m_glassSheet; }

    /// 便捷日志（内部已切 UI 线程）。
    void logInfo(const QString& message);
    void logSuccess(const QString& message);
    void logWarning(const QString& message);
    void logError(const QString& message);
    void logDetail(const QString& message);

    /// 便捷通知（内部已切 UI 线程）。
    void success(const QString& message, int autoHideMs = 4000);
    void error(const QString& message, int autoHideMs = 5000);
    void hideNotify();

    /// 执行一段「读 / 写本页可视元素」的动作；非 UI 线程会排队。
    void onUiThread(std::function<void()> action);

private:
    bool isOnUiThread() const;

    QString m_title;
    NotificationBar* m_notifyBar = nullptr;
    LogView* m_logView = nullptr;
    bool m_glassSheet = false;
};

} // namespace kc::ui
