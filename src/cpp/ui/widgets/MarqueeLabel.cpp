#include "ui/widgets/MarqueeLabel.h"

#include "ui/theme/Motion.h"

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
/// 滚动步进：每 30ms 走 1px ≈ 33px/s，中文小字读得过来。
constexpr int kTickMs = 30;

} // namespace

MarqueeLabel::MarqueeLabel(QWidget* parent) : QLabel(parent)
{
    setObjectName(QStringLiteral("marqueeLabel"));
    // 文本自己画，QLabel 的自动绘制关掉（否则会与 paintEvent 叠字）。
    setTextInteractionFlags(Qt::NoTextInteraction);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, [this] {
        const int cycle = m_textWidth + kGap;
        if (cycle <= 0) return;
        m_offset = (m_offset + 1) % cycle;
        update();
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

void MarqueeLabel::applyTimerState()
{
    // 三个条件同时满足才滚：有文本、文本比可视区宽、动画开关开着且未暂停。
    const bool tooLong = m_textWidth > qMax(0, width() - 2 * kPad);
    const bool shouldRun = !m_text.isEmpty() && tooLong && !m_paused &&
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
