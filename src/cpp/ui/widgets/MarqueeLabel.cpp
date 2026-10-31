#include "ui/widgets/MarqueeLabel.h"

#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QTimer>

namespace kc::ui {
namespace {

/// 左内边距：与状态栏其它分段对齐（12px）。
constexpr int kPad = 12;
/// 文本滚完一轮后接回开头前留的空白，避免首尾粘在一起。
constexpr int kGap = 56;
/// 滚动步进：每 50ms 走 2px ≈ 40px/s。
///
/// 🔴 原来是「30ms 走 1px」。改成「50ms 走 2px」是 2026-10-30 的卡顿修复：
///    · 速度反而更快一点（33 → 40px/s），读起来不更慢；
///    · 但**刷新次数从 33/s 降到 20/s**，少了 40% 的重绘。
///    中文小字在 40px/s 下依然读得过来，而重绘次数直接决定卡不卡。
constexpr int kTickMs = 50;
/// 每次滚动的像素步长（与 kTickMs 一起决定速度）。
constexpr int kStepPx = 2;

} // namespace

MarqueeLabel::MarqueeLabel(QWidget* parent) : QLabel(parent)
{
    setObjectName(QStringLiteral("marqueeLabel"));
    // 文本自己画，QLabel 的自动绘制关掉（否则会与 paintEvent 叠字）。
    setTextInteractionFlags(Qt::NoTextInteraction);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    // 🔴 让本控件的重绘**不外溢到父链**（2026-10-30 卡顿修复的核心）。
    //    MarqueeLabel 铺在状态栏的圆角胶囊里，滚动时每 50ms 一次 update()。
    //    默认行为下 Qt 会把父控件也标脏 → MainShell::paintEvent 整窗重绘壁纸
    //    → 再连带所有玻璃子控件（每块重新磨砂 + 折射）。
    //    --fps-probe 实测：滚动期间主屏常驻帧率 6/s → 900~1500/s。
    //    WA_OpaquePaintEvent 告诉 Qt「这块我自己画满，不用父控件兜底」，
    //    paintEvent 里随之把底铺成父级胶囊的底色（见 paintEvent 第一段）。
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    // WA_StaticContents：内容只在「自己这块」变化，Qt 不会因为本控件重绘
    // 而把父控件的其它区域一起标脏 —— 这是把 32/s 的父链重绘真正掐掉的关键。
    setAttribute(Qt::WA_StaticContents, true);

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, [this] {
        const int cycle = m_textWidth + kGap;
        if (cycle <= 0) return;
        m_offset = (m_offset + kStepPx) % cycle;
        // 🔴 必须**只刷自己这一条**，不能裸调 update()。
        //    裸 update() 会把整条父链标脏：状态栏 → MainShell → 整窗壁纸重绘，
        //    再连带所有玻璃子控件（每块都要重新磨砂 + 折射）一起重画。
        //    实测（--fps-probe）：文案一进入滚动，主屏常驻帧率从 6/s 飙到
        //    900~1500/s，用户看到的就是「动画一卡一卡」。
        //    滚动只影响本控件内部，update(rect()) 就够了。
        update(rect());
    });
}

void MarqueeLabel::setText(const QString& text)
{
    m_text = text;
    m_offset = 0;
    measure();
    setVisible(!m_text.isEmpty());
    applyTimerState();
    update();
}

void MarqueeLabel::setPaused(bool paused)
{
    m_paused = paused;
    applyTimerState();
}

void MarqueeLabel::measure()
{
    m_textWidth = fontMetrics().horizontalAdvance(m_text);
}

QBrush MarqueeLabel::backgroundBrush() const
{
    // 铺底用的实色：与 QSS 里 #marqueeBox 的填充**同色**，但去掉 alpha。
    //
    // 🔴 为什么不用 palette().window() 直接取：父级胶囊的底色来自 QSS 的
    //    `background: rgba(255,255,255,.92)`，那是**半透明**的 —— 本控件不透明
    //    地刷一层半透明色，会先把自己的底擦成黑的再叠上去，屏上就是一块灰斑。
    //    所以这里显式给一对「深 / 浅」实色，值与两份 QSS 保持一致。
    //    QSS 改色时这里要同步（两个值都写了出处，grep 一下就能找到）。
    const bool dark = ThemeManager::instance().isDark();
    // light.qss: rgba(255,255,255,.92) 压在浅色壁纸上 ≈ #F4F7FB
    // dark.qss : rgba(23,27,33,.94)    压在深色壁纸上 ≈ #171B21
    return QBrush(dark ? QColor(0x17, 0x1B, 0x21) : QColor(0xF4, 0xF7, 0xFB));
}

void MarqueeLabel::applyTimerState()
{
    // 三个条件同时满足才滚：有文本、文本比可视区宽、动画开关开着且未暂停。
    const bool tooLong = m_textWidth > qMax(0, width() - 2 * kPad);
    // 🔴 探针夹具：KC_FORCE_MARQUEE=1 时无视「是否超宽」强制滚动。
    //    为什么要它：正常窗口下状态栏文案通常不超宽，滚动根本不触发，
    //    于是「滚动导致的整窗重绘」这个 bug 在自动化里**永远测不到**。
    //    只在探针里设这个环境变量，正常运行完全不受影响。
    static const bool kForce = qEnvironmentVariableIsSet("KC_FORCE_MARQUEE");
    const bool shouldRun = !m_text.isEmpty() && (tooLong || kForce) && !m_paused &&
                           Motion::animationsEnabled();
    if (shouldRun) {
        if (!m_timer->isActive()) m_timer->start();
    } else {
        m_timer->stop();
        if (m_offset != 0) { m_offset = 0; update(); }
    }
}

void MarqueeLabel::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // 🔴 第一件事：把底铺满。本控件是 WA_OpaquePaintEvent —— Qt 不会再让父控件
    //    兜底，不自己铺就会残留上一帧的字影（滚动时看着像拖影）。
    //    底色取父级胶囊的填充色（QSS 的 #marqueeBox），拿不到就用窗口底。
    painter.fillRect(rect(), backgroundBrush());

    // QSS 的 `color` 对 QLabel 落到 WindowText —— 这里读它，绝不在代码里写死颜色。
    painter.setPen(palette().color(QPalette::WindowText));
    painter.setFont(font());

    // 🔴 父级胶囊（#marqueeBox）是 22px 高的圆头条，本控件铺满它整宽 ——
    //    不裁剪的话滚动中的文字会直接压在圆头上、并越过右端的弧线跑到外面去，
    //    屏上看着像「胶囊被一刀切平」（2026-10-17 实测）。
    //    这里按胶囊的半径把绘制区收成圆角矩形，文字跟着弧线一起被收住。
    QPainterPath clip;
    const qreal rad = qMax(0.0, (height() - 2.0) / 2.0);
    clip.addRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), rad, rad);
    painter.setClipPath(clip);

    const int baseline = (height() + fontMetrics().ascent() - fontMetrics().descent()) / 2;
    const int cycle = m_textWidth + kGap;
    const bool scrolling = m_timer->isActive() && cycle > 0;
    const int x = kPad - (scrolling ? m_offset : 0);

    painter.drawText(x, baseline, m_text);
    if (scrolling) {
        // 再画一份接在尾部 —— 无缝循环的关键。
        painter.drawText(x + cycle, baseline, m_text);
    }
}

void MarqueeLabel::resizeEvent(QResizeEvent* event)
{
    QLabel::resizeEvent(event);
    applyTimerState();
}

void MarqueeLabel::enterEvent(QEnterEvent* event)
{
    QLabel::enterEvent(event);
    setPaused(true);   // 悬停停下让人读全
}

void MarqueeLabel::leaveEvent(QEvent* event)
{
    QLabel::leaveEvent(event);
    setPaused(false);
}

} // namespace kc::ui
