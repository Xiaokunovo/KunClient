#include "ui/widgets/IosPageControl.h"

#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace kc::ui {

namespace {
constexpr int kDot = 7;
constexpr int kDotActive = 18;
constexpr int kGap = 8;
constexpr int kH = 12;
} // namespace

IosPageControl::IosPageControl(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedHeight(kH);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void IosPageControl::setCount(int count)
{
    m_count = std::max(0, count);
    if (m_index >= m_count) m_index = std::max(0, m_count - 1);
    m_indicator = m_index;
    updateGeometry();
    update();
}

void IosPageControl::setCurrentIndex(int index)
{
    if (m_count <= 0) return;
    index = std::clamp(index, 0, m_count - 1);
    if (index == m_index) return;
    m_index = index;
    animateIndicator(index);
    emit currentIndexChanged(index);
    update();
}

QSize IosPageControl::sizeHint() const
{
    if (m_count <= 0) return QSize(0, kH);
    const int w = kDotActive + (m_count - 1) * kDot + m_count * kGap;
    return QSize(w, kH);
}

void IosPageControl::animateIndicator(qreal target)
{
    if (!Motion::animationsEnabled()) {
        m_indicator = target;
        update();
        return;
    }
    if (!m_anim) {
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(240);
        m_anim->setEasingCurve(Motion::silkEase());
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_indicator = v.toReal();
            update();
        });
    }
    m_anim->stop();
    m_anim->setStartValue(m_indicator);
    m_anim->setEndValue(target);
    m_anim->start();
}

void IosPageControl::paintEvent(QPaintEvent*)
{
    if (m_count <= 0) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    const QColor idle = dark ? QColor(255, 255, 255, 80) : QColor(16, 20, 26, 70);
    const QColor active = dark ? QColor(255, 255, 255, 230) : QColor(16, 20, 26, 220);

    // 当前点的胶囊位置随 m_indicator 连续移动；其它点是固定圆。
    const qreal cy = height() / 2.0;
    qreal x = 0.0;
    for (int i = 0; i < m_count; ++i) {
        // 每个点的「名义」起点按活动点宽度计算，滑动时用插值宽度。
        const qreal grow = std::max(0.0, 1.0 - std::abs(m_indicator - i));
        const qreal w = kDot + (kDotActive - kDot) * grow;
        const QRectF r(x, cy - kDot / 2.0, w, kDot);
        p.setPen(Qt::NoPen);
        p.setBrush(i == m_index || grow > 0.5 ? active : idle);
        p.drawRoundedRect(r, kDot / 2.0, kDot / 2.0);
        x += w + kGap;
    }
}

void IosPageControl::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_count > 0) {
        // 按「点 + 间距」的步长反算落在哪个点上。
        const qreal step = kDot + kGap;
        const int idx = std::clamp(int(event->pos().x() / step), 0, m_count - 1);
        setCurrentIndex(idx);
    }
    QWidget::mousePressEvent(event);
}

} // namespace kc::ui
