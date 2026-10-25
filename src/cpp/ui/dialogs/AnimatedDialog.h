#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AnimatedDialog —— 所有模态弹窗的统一动画底座（kc::ui）。
//
//  为什么需要它：弹窗过去都是 `setVisible(true)` / `accept()` 硬切，出现和消失
//  都没有任何过渡，和「发布会风格」的其余界面完全两个世代。这里把入场/退场
//  收敔到一处，全部 18 个对话框只改一个基类就都有了。
//
//  两个动作（与 Motion 同一族曲线，不另起炉灶）：
//    · 入场（showEvent）：windowOpacity 0→1 + 从下方 12px 浮上来（silkEase，200ms）；
//    · 退场（done）：windowOpacity 1→0 + 下沉 8px（exitEase，150ms），**动画播完
//      才真正调用 QDialog::done()**，所以关闭不再是"啪"地消失。
//
//  🔴 三个坑（都踩过，别再改回去）：
//   1. 动画用 **windowOpacity** 而不是 QGraphicsOpacityEffect —— 本项目的弹窗多是
//      无边框 + WA_TranslucentBackground，给顶层 widget 挂 graphics effect 会和
//      QSS 的背景绘制打架（表现为整窗发黑/花屏）；windowOpacity 走合成器，没这事。
//   2. 退场必须**延迟**调用 QDialog::done()：done() 会退出 exec() 的嵌套事件循环，
//      在循环里同步播动画等于动画一帧都不走。所以先记下目标返回值，动画结束再补调用。
//   3. 重入保护：done() 可能在动画未结束时被二次触发（例如关闭定时器 + 用户点按钮），
//      用 m_closing 挡掉，否则会出现"退场动画播一半被 QDialog::done 打断闪一下"。
//
//  全局开关：Motion::animationsEnabled()（QSettings appearance/animations）为假时
//  两个动画都退化成直改，行为与改造前完全一致。
// ─────────────────────────────────────────────────────────────────────────────

#include <QDialog>

namespace kc::ui {

class AnimatedDialog : public QDialog {
    Q_OBJECT

public:
    explicit AnimatedDialog(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;
    /// 拦截关闭：先播退场动画，动画结束才真正交给 QDialog::done。
    void done(int r) override;

    /// 入场是否带上浮位移（默认 true）。
    /// 登录框这类「居中模态大窗」历史上只要淡入不要位移（用户 2026-10-11 的意见：
    /// 模态顶层窗口飘一下显得不稳），构造时 setRiseEnabled(false) 即可。
    void setRiseEnabled(bool on) { m_riseEnabled = on; }

private:
    void playIn();
    void playOut(int r);

    bool m_inPlayed = false;   ///< 入场动画是否已播（重复 show 不重播）
    bool m_closing = false;    ///< 是否正在播退场动画（重入保护）
    bool m_riseEnabled = true; ///< 入场是否带上浮（登录框关掉）
};

} // namespace kc::ui
