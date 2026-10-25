#include "ui/widgets/SegmentedControl.h"

#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QEnterEvent>
#include <QFont>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>

#include <algorithm>

namespace kc::ui {

namespace {

constexpr int kHeight = 32;
constexpr int kSegmentPadX = 18;   // 每段文字左右留白
constexpr int kMinWidth = 220;

} // namespace

SegmentedControl::SegmentedControl(QWidget* parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFixedHeight(kHeight);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

void SegmentedControl::addItem(const QString& text, const QVariant& data)
{
    m_items.append(Item{text, data});
    if (m_items.size() == 1) {
        m_index = 0;
        m_indicator = 0.0;
    }
    updateGeometry();
    update();
}

QSize SegmentedControl::sizeHint() const
{
    if (m_items.isEmpty()) return QSize(kMinWidth, kHeight);

    QFont f = font();
    f.setPixelSize(13);
    const QFontMetrics fm(f);
    int w = 8;   // 轨道左右内边距
    for (const Item& it : m_items) w += fm.horizontalAdvance(it.text) + kSegmentPadX * 2;
    return QSize(std::max(kMinWidth, w), kHeight);
}

QSize SegmentedControl::minimumSizeHint() const
{
    return QSize(kMinWidth, kHeight);
}

QVariant SegmentedControl::currentData() const
{
    if (m_index < 0 || m_index >= m_items.size()) return QVariant();
    return m_items.at(m_index).data;
}

int SegmentedControl::findData(const QVariant& data) const
{
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).data == data) return i;
    }
    return -1;
}

void SegmentedControl::setCurrentData(const QVariant& data)
{
    const int idx = findData(data);
    if (idx >= 0) setCurrentIndex(idx);
}

void SegmentedControl::setCurrentIndex(int index)
{
    if (m_items.isEmpty()) return;
    index = std::clamp(index, 0, int(m_items.size()) - 1);
    if (index == m_index && qFuzzyCompare(m_indicator + 1.0, index + 1.0)) return;

    m_index = index;
    animateIndicator(index);
    emit currentIndexChanged(index);
    update();
}

QRectF SegmentedControl::innerRect() const
{
    return QRectF(rect()).adjusted(2, 2, -2, -2);
}

qreal SegmentedControl::segmentWidth() const
{
    if (m_items.isEmpty()) return 0.0;
    return innerRect().width() / m_items.size();
}

int SegmentedControl::segmentAt(const QPoint& pos) const
{
    if (m_items.isEmpty()) return -1;
    const QRectF inner = innerRect();
    if (pos.x() < inner.left() || pos.x() > inner.right()) return -1;
    const qreal sw = segmentWidth();
    if (sw <= 0.0) return -1;
    const int idx = int((pos.x() - inner.left()) / sw);
    return std::clamp(idx, 0, int(m_items.size()) - 1);
}

void SegmentedControl::animateIndicator(qreal target)
{
    if (!Motion::animationsEnabled()) {
        m_indicator = target;
        update();
        return;
    }
    if (!m_anim) {
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(220);
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

void SegmentedControl::paintEvent(QPaintEvent*)
{
    if (m_items.isEmpty()) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    const QRectF inner = innerRect();
    const qreal sw = segmentWidth();
    const qreal radius = inner.height() / 2.0;

    // 轨道
    p.setPen(Qt::NoPen);
    p.setBrush(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 16));
    p.drawRoundedRect(inner, radius, radius);

    // 滑动药丸：选中段背景
    const qreal pillX = inner.left() + m_indicator * sw + 2.0;
    const QRectF pill(pillX, inner.top() + 2.0, sw - 4.0, inner.height() - 4.0);
    p.setBrush(dark ? QColor(255, 255, 255, 40) : QColor(255, 255, 255, 235));
    if (!dark) {
        // 浅色主题给药丸一点投影，才看得出「浮起来」
        p.setBrush(QColor(0, 0, 0, 18));
        p.drawRoundedRect(pill.adjusted(0, 1.5, 0, 1.5), radius - 2, radius - 2);
        p.setBrush(QColor(255, 255, 255));
    }
    p.drawRoundedRect(pill, radius - 2, radius - 2);

    // 文字
    QFont f = font();
    f.setPixelSize(13);
    const QFontMetrics fm(f);
    for (int i = 0; i < m_items.size(); ++i) {
        const bool selected = (i == m_index);
        QFont fi = f;
        fi.setWeight(selected ? QFont::DemiBold : QFont::Normal);
        p.setFont(fi);

        QColor color;
        if (dark) color = selected ? QColor(255, 255, 255, 240) : QColor(255, 255, 255, 130);
        else      color = selected ? QColor(16, 20, 26, 240)  : QColor(16, 20, 26, 130);
        if (i == m_hover && !selected) color = dark ? QColor(255, 255, 255, 190)
                                                    : QColor(16, 20, 26, 190);
        p.setPen(color);

        const QRectF segRect(inner.left() + i * sw, inner.top(), sw, inner.height());
        p.drawText(segRect, Qt::AlignCenter,
                   fm.elidedText(m_items.at(i).text, Qt::ElideRight, int(sw) - 8));
    }
}

void SegmentedControl::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const int idx = segmentAt(event->pos());
        if (idx >= 0) setCurrentIndex(idx);
    }
    QWidget::mousePressEvent(event);
}

void SegmentedControl::enterEvent(QEnterEvent* event)
{
    QWidget::enterEvent(event);
}

void SegmentedControl::mouseMoveEvent(QMouseEvent* event)
{
    const int idx = segmentAt(event->pos());
    if (idx != m_hover) {
        m_hover = idx;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void SegmentedControl::leaveEvent(QEvent* event)
{
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
    QWidget::leaveEvent(event);
}

} // namespace kc::ui
