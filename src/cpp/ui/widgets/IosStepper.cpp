#include "ui/widgets/IosStepper.h"

#include "ui/theme/ThemeManager.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace kc::ui {

namespace {
constexpr int kW = 94;
constexpr int kH = 32;
constexpr qreal kRadius = 8;
} // namespace

IosStepper::IosStepper(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kW, kH);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

QSize IosStepper::sizeHint() const
{
    return QSize(kW, kH);
}

void IosStepper::setRange(int lo, int hi)
{
    m_min = lo;
    m_max = std::max(lo, hi);
    setValue(m_value);
}

void IosStepper::setSingleStep(int step)
{
    m_step = std::max(1, step);
}

void IosStepper::setValue(int value)
{
    const int v = std::clamp(value, m_min, m_max);
    if (v == m_value) {
        update();
        return;
    }
    m_value = v;
    update();
    emit valueChanged(m_value);
}

QRectF IosStepper::halfRect(bool plus) const
{
    const qreal w = width() / 2.0;
    return QRectF(plus ? w : 0.0, 0.0, w, height());
}

void IosStepper::nudge(int delta)
{
    setValue(m_value + delta * m_step);
}

void IosStepper::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    p.setPen(QPen(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 22), 1.0));
    p.setBrush(dark ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 210));
    p.drawRoundedRect(box, kRadius, kRadius);

    // 中间分隔线
    p.setPen(QPen(dark ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 18), 1.0));
    p.drawLine(QPointF(width() / 2.0, 6), QPointF(width() / 2.0, height() - 6));

    const bool canMinus = m_value > m_min;
    const bool canPlus = m_value < m_max;
    const QColor on = dark ? QColor(255, 255, 255, 232) : QColor(16, 20, 26, 232);
    const QColor off = dark ? QColor(255, 255, 255, 70) : QColor(16, 20, 26, 60);

    const qreal cy = height() / 2.0;
    const qreal arm = 5.0;

    // −
    p.setPen(QPen(canMinus ? on : off, 1.8, Qt::SolidLine, Qt::RoundCap));
    const qreal mx = width() * 0.25;
    p.drawLine(QPointF(mx - arm, cy), QPointF(mx + arm, cy));

    // ＋
    p.setPen(QPen(canPlus ? on : off, 1.8, Qt::SolidLine, Qt::RoundCap));
    const qreal px = width() * 0.75;
    p.drawLine(QPointF(px - arm, cy), QPointF(px + arm, cy));
    p.drawLine(QPointF(px, cy - arm), QPointF(px, cy + arm));

    // 按下反馈
    if (m_pressed != 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(dark ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 14));
        p.drawRoundedRect(halfRect(m_pressed > 0), kRadius, kRadius);
    }
}

void IosStepper::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = event->pos().x() >= width() / 2 ? 1 : -1;
        update();
    }
    QWidget::mousePressEvent(event);
}

void IosStepper::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_pressed != 0) {
        const bool inside = rect().contains(event->pos());
        const int side = m_pressed;
        m_pressed = 0;
        if (inside) nudge(side);
        update();
    }
    QWidget::mouseReleaseEvent(event);
}

void IosStepper::leaveEvent(QEvent* event)
{
    m_pressed = 0;
    update();
    QWidget::leaveEvent(event);
}

} // namespace kc::ui
