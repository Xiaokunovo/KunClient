#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  WelcomeDialog —— 首次启动的欢迎 + 引导（2026-10-17）
//
//  用户要求：「当用户注册第一次进入客户端增加一个欢迎界面和引导界面，
//  注意仅仅是第一次打开，不是每个设备打开都要一次」。
//
//  所以判据是 **本机 QSettings 的一次性标记**（onboarding/completed），
//  不是每次启动、也不是每台设备：
//    · 第一次打开 → 登录成功后弹一次，看完（或点「跳过」）就写标记；
//    · 之后每次启动 → 直接进主屏，不再打扰。
//  想重看：设置页「关于」里的「再看一次引导」，或者删掉该键。
//
//  四页：欢迎 → 导航 → 刷机 → 开始。翻页用 QStackedWidget，页脚是
//  跳过 / 圆点指示 / 下一步。窗口无边框 + 自绘圆角玻璃，进出场动画继承
//  AnimatedDialog（windowOpacity，别改成 QGraphicsOpacityEffect）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/dialogs/AnimatedDialog.h"

#include <functional>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace kc::ui {

class WelcomeDialog : public AnimatedDialog {
    Q_OBJECT

public:
    explicit WelcomeDialog(QWidget* parent = nullptr);

    /// 本机是否已经看过引导（QSettings onboarding/completed）。
    static bool isCompleted();
    /// 记下「已看过」。
    static void markCompleted();
    /// 清掉标记（调试 / 命令行用；设置页的「再看一次」**不**走它 ——
    /// 重看只是重看，中途关掉不该把「已看过」抹掉）。
    static void resetCompleted();

    /// 直接落到第 index 页（截图探针 / 调试用；不写任何标记）。
    void previewPage(int index);

signals:
    /// 引导结束（看完或跳过）。外壳据此把焦点还给主窗口。
    void finished();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void buildUi();
    QWidget* buildPage(int index);
    void goTo(int index);
    void updateFooter();

    QStackedWidget* m_stack = nullptr;
    QPushButton* m_skip = nullptr;
    QPushButton* m_next = nullptr;
    QLabel* m_stepLabel = nullptr;
    // StepDots 是 .cpp 里的匿名类，头文件里不能有它的成员类型 —— 用一个
    // 回调把「同步圆点」的能力带出去，免得 goTo() 与圆点各改一份状态。
    std::function<void(int)> m_setDots;
    int m_index = 0;
    int m_pageCount = 4;
};

} // namespace kc::ui
