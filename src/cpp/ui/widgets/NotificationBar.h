#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  NotificationBar — 内联通知条（替代 MessageBox）
//
//  移植自 tests/src/KunClient.App/Services/Notify.cs（119 行）。
//  · 出现 / 消失都走 Motion 的「淡入 + 上滑」，不再硬切 Visibility；
//  · 自动隐藏计时器**按容器各挂各的** —— 过去全应用共用一个 static 计时器，
//    A 页提示条还在倒计时、B 页又弹一条时 A 的计时器被顶掉，A 的提示条从此
//    永远停在屏幕上（页面按页签缓存，切回来它还在）。这里每个实例自带 QTimer。
//  · 底色/描边/文字色一律取主题令牌，绝不写死（写死 = 深色主题下的浅色补丁）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>

class QLabel;
class QTimer;
class QVBoxLayout;

namespace kc::ui {

/// 通知级别：信息 / 成功 / 警告 / 错误。
enum class NotifyLevel { Info, Success, Warning, Error };

class NotificationBar : public QWidget {
    Q_OBJECT

public:
    explicit NotificationBar(QWidget* parent = nullptr, NotifyLevel level = NotifyLevel::Info);
    ~NotificationBar() override;

    /// 便捷工厂：在 parent 上创建一条通知条并淡入上滑显示。
    /// text 为空或 parent 为空时安全返回 nullptr。
    static NotificationBar* show(QWidget* parent, const QString& text, NotifyLevel level,
                                 int ms = 4000);

    /// 在当前容器内显示一条通知（已有实例则复用，不重播动画）。
    void notify(const QString& text, NotifyLevel level, int ms = 4000);

    void setLevel(NotifyLevel level);
    NotifyLevel level() const { return m_level; }

    /// 出现时长（与页面转场同一族曲线，手感一致）。
    static constexpr int RevealMs = 240;
    /// 消失时长。退场必须比入场快 —— 和页面转场同一个原则。
    static constexpr int ConcealMs = 160;

protected:
    void changeEvent(QEvent* event) override;

private:
    void applyTheme();
    void concealAndHide();
    void buildUi();

    QLabel* m_icon = nullptr;
    QLabel* m_text = nullptr;
    QTimer* m_timer = nullptr;
    NotifyLevel m_level = NotifyLevel::Info;
    int m_autoHideMs = 4000;
    // 是否已在完全展示状态（决定是否重播入场动画，见基类注释）。
    bool m_shown = false;
    // 重入守卫：applyTheme() 里的 setStyleSheet() 会同步投递 StyleChange，
    // 若 changeEvent 再回灌 applyTheme 就是无限递归（栈溢出）。见 .cpp 注释。
    bool m_applyingTheme = false;
};

} // namespace kc::ui
