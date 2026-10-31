#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MarqueeLabel — 单行跑马灯文本（外壳状态栏底部的「签名 / 哲理」带）。
//
//  2026-10-16 用户要求：把内嵌 EDL 窗底部那行提示文案并进外壳状态栏后，改成一条
//  跑马灯 —— 用户设了「个性签名」就滚签名，没设就轮播一组人生哲理文案。
//
//  实现要点：
//   · **继承 QLabel**（而不是裸 QWidget）：QSS 的 `color` / `font-*` 对 QLabel 会
//     落到 QPalette::WindowText 与字体上，paintEvent 里直接读 palette 即可 ——
//     裸 QWidget 拿不到 QSS 的 color，只能自己在代码里写死颜色（本项目红线之一）。
//   · 只有 x 偏移在动，文本用 QPainter 画两遍（首尾相接）实现无缝循环；不用
//     QTimer + move()，那是 QLabel 跑马灯的老做法，滚动时整控件重排、性能差。
//   · 文本比可用宽度短时**不滚**（静态左对齐）—— 短签名滚起来像故障。
//   · 尾部留一段空白再接回开头，读起来不赶；步进 2px / 50ms ≈ 40px/s。
//   · 动画总开关关掉时（Motion::animationsEnabled() 为假）退化为静态显示 —— 与
//     全项目其余动效同一口径。
//
//  🔴 2026-10-30 卡顿修复（用户口径「动画会有卡顿」）：
//     滚动期间每 tick 一次 update()，默认会让 Qt 把**整条父链**标脏 →
//     MainShell::paintEvent 整窗重绘壁纸 → 再连带所有玻璃子控件重新磨砂 + 折射。
//     --fps-probe 实测：主屏常驻帧率从 6/s 飙到 900~1500/s，肉眼就是持续卡顿。
//     修法是把本控件做成**不透明子控件**（WA_OpaquePaintEvent），paintEvent 里
//     自己把背景铺满，Qt 就不必再让父控件兜底 —— 滚动只剩这一条 22px 高的区域在刷。
// ─────────────────────────────────────────────────────────────────────────────

#include <QLabel>
#include <QString>

class QTimer;

namespace kc::ui {

class MarqueeLabel : public QLabel {
    Q_OBJECT

public:
    explicit MarqueeLabel(QWidget* parent = nullptr);

    /// 设置要滚动的文本（会重置滚动位置）。空文本时整个控件自动隐藏。
    void setText(const QString& text);
    QString text() const { return m_text; }

    /// 暂停 / 恢复滚动（鼠标悬停时自动暂停，让人读全）。
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void measure();
    void applyTimerState();
    /// 铺底用的实色画刷（见 .cpp 里为什么不能用 palette().window()）。
    QBrush backgroundBrush() const;

    QString m_text;
    QTimer* m_timer = nullptr;
    int m_offset = 0;        ///< 当前滚动偏移（像素，正数向左推进）
    int m_textWidth = 0;     ///< 文本像素宽（预量）
    bool m_paused = false;
};

} // namespace kc::ui
