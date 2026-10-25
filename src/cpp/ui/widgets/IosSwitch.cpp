#include "ui/widgets/IosSwitch.h"

#include "ui/theme/ThemeManager.h"

#include <QEnterEvent>
#include <QPainter>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QVariantAnimation>

#include <algorithm>

namespace kc::ui {

namespace {

constexpr int kSwitchW = 46;
constexpr int kSwitchH = 28;

QColor mixColor(const QColor& a, const QColor& b, qreal t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor(int(a.red() + (b.red() - a.red()) * t),
                  int(a.green() + (b.green() - a.green()) * t),
                  int(a.blue() + (b.blue() - a.blue()) * t),
                  int(a.alpha() + (b.alpha() - a.alpha()) * t));
}

} // namespace

IosSwitch::IosSwitch(QWidget* parent)
    : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kSwitchW, kSwitchH);
    setFocusPolicy(Qt::NoFocus);

    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        animateTo(on ? 1.0 : 0.0);
    });
}

QSize IosSwitch::sizeHint() const
{
    return QSize(kSwitchW, kSwitchH);
}

void IosSwitch::setOnColor(const QColor& color)
{
    m_onColor = color;
    update();
}

void IosSwitch::setCheckedSilently(bool on)
{
    if (m_anim) m_anim->stop();
    QSignalBlocker block(this);
    setChecked(on);
    m_pos = on ? 1.0 : 0.0;
    update();
}

void IosSwitch::animateTo(qreal target)
{
    if (!m_anim) {
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(230);
        // OutBack：滑到终点时轻微过冲再回来 —— iOS 开关的手感来源。
        m_anim->setEasingCurve(QEasingCurve(QEasingCurve::OutBack));
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            m_pos = v.toReal();
            update();
        });
    }
    m_anim->stop();
    m_anim->setStartValue(m_pos);
    m_anim->setEndValue(target);
    m_anim->start();
}

void IosSwitch::showEvent(QShowEvent* event)
{
    QAbstractButton::showEvent(event);
    // 首次出现时直接落到当前状态，不要从 0 滑过来（读配置不该有动画）。
    if (!m_anim || m_anim->state() != QAbstractAnimation::Running) {
        m_pos = isChecked() ? 1.0 : 0.0;
        update();
    }
}

void IosSwitch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    const qreal h = height();
    const qreal w = width();
    const qreal pos = std::clamp(m_pos, 0.0, 1.0);

    QColor off = dark ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 38);
    if (m_hover) off = dark ? QColor(255, 255, 255, 62) : QColor(0, 0, 0, 50);
    const QColor track = mixColor(off, m_onColor, pos);

    p.setPen(Qt::NoPen);
    p.setBrush(track);
    p.drawRoundedRect(QRectF(0, 0, w, h), h / 2.0, h / 2.0);

    // 滑块：直径 = 高度 - 4，左右各留 2px。
    const qreal d = h - 4.0;
    const qreal x = 2.0 + pos * (w - d - 4.0);

    // 轻微投影，让白滑块浮起来（不用 QGraphicsDropShadowEffect：太重）。
    p.setBrush(QColor(0, 0, 0, dark ? 70 : 40));
    p.drawEllipse(QRectF(x, 3.5, d, d));

    p.setBrush(QColor(255, 255, 255));
    p.drawEllipse(QRectF(x, 2.0, d, d));
}

void IosSwitch::enterEvent(QEnterEvent* event)
{
    m_hover = true;
    update();
    QAbstractButton::enterEvent(event);
}

void IosSwitch::leaveEvent(QEvent* event)
{
    m_hover = false;
    update();
    QAbstractButton::leaveEvent(event);
}

} // namespace kc::ui
