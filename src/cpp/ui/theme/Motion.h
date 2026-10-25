#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Motion —「发布会风格」动效参数与动画工厂
//
//  从 tests/src/KunClient.App/Services/Motion.cs 移植（297 行）。
//  全局唯一缓动来源，避免各控件各写一套曲线。
//
//  三个特征（与 C# 版逐条对应）：
//    1. 临界阻尼曲线：先快后极缓，落点无生硬停帧；
//    2. 位移/缩放带极轻微过冲后落定（"落定感"），不是线性刹停；
//    3. 以淡入淡出 + 微缩放表达层次，位移量刻意做小（≤16px），不做夸张横移。
//
//  C# 版靠 async/await + Task.WhenAny(completed, delay) 给动画兜底；Qt 没有这层，
//  这里改用「令牌重入保护」达到同一目的：同一元素上快速连续触发 显示/隐藏 时，
//  旧的那次动画在自己结束时发现"已被顶替"就直接让位，绝不把新动画正在播的状态
//  强行写成终态（否则会闪一下）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QObject>
#include <QPointer>
#include <QString>

class QPropertyAnimation;
class QWidget;

namespace kc::ui {

/// 缓动曲线族（对应 C# 的 KeySpline 常量）。
/// 用 QEasingCurve 的贝塞尔近似：Qt 原生 CubicBezier 支持任意控制点。
class Motion {
public:
    /// 动画时长之外再给的兜底增量（C# TimeoutFudgeMs）。
    static constexpr int TimeoutFudgeMs = 800;

    /// Apple 转场曲线 cubic-bezier(0.32, 0.72, 0, 1) —— 标准"丝滑"，用于位移。
    static QEasingCurve appleEase();

    /// Silk 曲线 cubic-bezier(0.16, 1, 0.3, 1) —— 先快后极缓，用于淡入。
    static QEasingCurve silkEase();

    /// 退场曲线 cubic-bezier(0.4, 0, 0.6, 1) —— 慢起快收，用于淡出/退场。
    static QEasingCurve exitEase();

    /// ColorOS 弹簧落定曲线 cubic-bezier(0.34, 1.56, 0.64, 1) —— 轻微过冲后
    /// 回落收住（导航指示器 / 开关 / 磁贴按压回弹）。落点感 = 不像线性刹停。
    static QEasingCurve springEase();

    /// 落定曲线 cubic-bezier(0.2, 0.9, 0.3, 1) —— 过冲后的回落段。
    static QEasingCurve settleEase();

    // ── 出现 / 消失原语 ────────────────────────────────────────────────────
    //  项目里所有「展开 → 收起」「显示 → 隐藏」过去都是一行 setVisible，
    //  也就是硬切。下面两个原语把这类切换统一成「淡 + 微位移」，
    //  位移量刻意做小（默认 10px）：够表达方向，又不会把界面推得晃。

    /// 平滑显现：淡入 + 从 slideFrom 滑到 0。
    /// 默认从**上方** 10px 滑下（负值）—— 展开类元素从"被压进去的方向"长出来最自然。
    static void reveal(QWidget* w, int durationMs = 240, double slideFrom = -10.0, int delayMs = 0);

    /// 平滑隐藏：淡出 + 滑向 slideTo；动画结束后按 collapseAfter 决定是否真的隐藏。
    static void conceal(QWidget* w, int durationMs = 160, double slideTo = -10.0,
                        int delayMs = 0, bool collapseAfter = true);

    /// 横向平滑显现：淡入 + 从 dx 平移过来（页面切换用，dx 正=从右来，负=从左来）。
    /// 与 reveal 同一族曲线，区别只是位移方向：翻页要有方向感，展开要"从被压进去的方向长出来"。
    static void revealX(QWidget* w, int dx, int durationMs = 260);

    // ── 纯淡入 / 淡出（不动位置）──────────────────────────────────────────
    //  🔴 布局管辖的控件**不能**用 reveal/conceal：它们内部会 `w->move()`，
    //     而布局下一次重排（resize / 增删子项 / polish）会把位置推回去 ——
    //     结果是「淡」生效、「位移」被无声吃掉，中途还会闪一下错位。
    //     这类控件（顶栏页名、网格瓦片、表头）只能走下面两个纯透明度原语。
    //
    //  delayMs 期间元素保持「起点态」（0 或 1），不会先亮一下再动。
    //  曲线固定：淡入 silkEase（先快后极缓），淡出 exitEase（慢起快收），
    //  与 reveal / conceal 同一族 —— 全应用只有一套"淡"的手感。
    static void fadeIn(QWidget* w, int durationMs = 200, int delayMs = 0);
    /// fadeOut 结束后按 hideAfter 决定是否真的 hide（默认隐藏）。
    static void fadeOut(QWidget* w, int durationMs = 140, bool hideAfter = true);

    // ── 全局动效开关 ──────────────────────────────────────────────────────
    //  读 QSettings 的 appearance/animations（默认 true）。关掉之后 reveal /
    //  conceal / revealX / AnimatedDialog 全部退化成直改（保留旧行为），
    //  给「觉得动画晃眼」的用户和自动化截图探针各留一条活路。
    static bool animationsEnabled();
    static void setAnimationsEnabled(bool on);

    // ── 通用补间助手 ──────────────────────────────────────────────────────

    /// 对某个 QObject 上的 qreal 属性跑一段单段补间。
    /// finished 时自动把属性落到终值并清场 —— 留着动画值会让后续直接赋值被静默覆盖。
    ///
    /// 🔴 **返回的指针只在本次调用栈内可信**：动画是以 DeleteWhenStopped 起播的，
    ///    跑完（或被打断 stop()）就 deleteLater 自删。想在成员里长期持有它，
    ///    必须用 `QPointer<QPropertyAnimation>` —— 用裸指针存下来，下一次
    ///    `ptr->stop()` 就是 use-after-free（2026-10-17 在 PageTransition 与
    ///    DynamicIsland 上各踩了一次，症状都是「第一次正常、第二次闪退」）。
    ///    只用一次（当场 connect / 起播）的场景用裸指针是安全的。
    static QPropertyAnimation* tween(QObject* target, const char* property, double from, double to,
                                     int ms, const QEasingCurve& curve = QEasingCurve());

    /// 带轻微过冲的「落定」补间：先冲到略过目标的位置，再回落收住。
    /// ⚠️ **只可用于缩放/位移** —— 透明度绝不能过冲（>1 或 <0 会表现异常/被钳位）。
    /// ⚠️ 返回值同样是**自删**动画，长期持有请用 QPointer（见 tween 的说明）。
    static QPropertyAnimation* settle(QObject* target, const char* property, double from, double to,
                                      int ms, double overshoot = 0.02);

    /// 起播一段动画并挂好收尾（+tokenMs 兜底）。动画所有权随 parent 转移。
    static void start(QAbstractAnimation* anim);

    /// 对窗口容器做一次 reveal/conceal —— 供 NotificationBar 等复用（保持同一族曲线）。
    static void revealWidget(QWidget* w, int durationMs = 240);
    static void concealWidget(QWidget* w, int durationMs = 160, bool collapseAfter = true);

    // ── 令牌重入保护 ──────────────────────────────────────────────────────
    //  每个 widget 一个递增令牌：同一 widget 上快速连续触发 显示/隐藏 时，
    //  旧的那次动画在自己结束时必须发现自己"已被顶替"，直接让位。
    static quint64 nextToken(const QWidget* w);
    static bool isCurrentToken(const QWidget* w, quint64 token);

    /// 摘掉某属性上的动画并把值落到 finalValue（<=NaN 表示不落值）。绝不对无效指针动手。
    static void clearAnimation(QObject* target, const char* property, double finalValue);

private:
    Motion() = delete;
};

} // namespace kc::ui
