#include "ui/widgets/IosSpinner.h"

#include "ui/theme/ThemeManager.h"

#include <QPainter>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace kc::ui {

namespace {
constexpr int kSpokes = 12;
constexpr int kTickMs = 16;
constexpr qreal kDegPerTick = 360.0 / kSpokes / 6.0;   // 6 tick 走一格 ≈ 1.15s 一圈
} // namespace

IosSpinner::IosSpinner(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(m_diameter, m_diameter);
    setFocusPolicy(Qt::NoFocus);

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &IosSpinner::tick);
}

void IosSpinner::setDiameter(int px)
{
    m_diameter = std::max(10, px);
    setFixedSize(m_diameter, m_diameter);
    updateGeometry();
    update();
}

void IosSpinner::setLineWidth(qreal px)
{
    m_lineWidth = std::max(1.0, px);
    update();
}

void IosSpinner::setColor(const QColor& color)
{
    m_color = color;
    update();
}

QSize IosSpinner::sizeHint() const
{
    return QSize(m_diameter, m_diameter);
}

void IosSpinner::start()
{
    if (!m_timer->isActive()) m_timer->start();
    update();
}

void IosSpinner::stop()
{
    m_timer->stop();
}

bool IosSpinner::isRunning() const
{
    return m_timer->isActive();
}

void IosSpinner::tick()
{
    m_angle = std::fmod(m_angle + kDegPerTick, 360.0);
    update();
}

void IosSpinner::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // 只有真正可见时才转：隐藏的页（比如没激活的 tab）不该白烧 CPU。
    if (!m_timer->isActive()) m_timer->start();
}

void IosSpinner::hideEvent(QHideEvent* event)
{
    m_timer->stop();
    QWidget::hideEvent(event);
}

void IosSpinner::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    const QColor base = m_color.isValid()
        ? m_color
        : (dark ? QColor(255, 255, 255) : QColor(16, 20, 26));

    const QPointF c(width() / 2.0, height() / 2.0);
    const qreal r = (std::min(width(), height()) - m_lineWidth) / 2.0 - 1.0;

    p.translate(c);
    p.rotate(m_angle);

    for (int i = 0; i < kSpokes; ++i) {
        // 越靠后的辐条越淡 —— 形成「拖尾」，与 iOS 一致。
        const qreal t = qreal(i + 1) / kSpokes;
        QColor col = base;
        col.setAlphaF(0.10 + 0.90 * t);
        p.setPen(QPen(col, m_lineWidth, Qt::SolidLine, Qt::RoundCap));
        const qreal a = qDegreesToRadians(i * (360.0 / kSpokes));
        const QPointF from(std::sin(a) * (r - 3.0), -std::cos(a) * (r - 3.0));
        const QPointF to(std::sin(a) * r, -std::cos(a) * r);
        p.drawLine(from, to);
    }
}

} // namespace kc::ui
