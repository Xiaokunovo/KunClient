#include "ui/widgets/IosProgressRing.h"

#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QFont>
#include <QPainter>
#include <QVariantAnimation>

#include <algorithm>

namespace kc::ui {

IosProgressRing::IosProgressRing(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(m_diameter, m_diameter);
    setFocusPolicy(Qt::NoFocus);
}

void IosProgressRing::setDiameter(int px)
{
    m_diameter = std::max(16, px);
    setFixedSize(m_diameter, m_diameter);
    updateGeometry();
    update();
}

void IosProgressRing::setLineWidth(qreal px)
{
    m_lineWidth = std::max(1.0, px);
    update();
}

void IosProgressRing::setColor(const QColor& color)
{
    m_color = color;
    update();
}

void IosProgressRing::setTextVisible(bool on)
{
    m_textVisible = on;
    update();
}

void IosProgressRing::setCenterText(const QString& text)
{
    m_centerText = text;
    update();
}

QSize IosProgressRing::sizeHint() const
{
    return QSize(m_diameter, m_diameter);
}

void IosProgressRing::setValue(qreal v)
{
    m_value = std::clamp(v, 0.0, 1.0);
    if (!Motion::animationsEnabled()) {
        m_shown = m_value;
        update();
        return;
    }
    if (!m_anim) {
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(220);
        m_anim->setEasingCurve(Motion::silkEase());
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant& val) {
            m_shown = val.toReal();
            update();
        });
    }
    m_anim->stop();
    m_anim->setStartValue(m_shown);
    m_anim->setEndValue(m_value);
    m_anim->start();
}

void IosProgressRing::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    // 🔴 ThemeManager::accent() 在「用户没自定义强调色」时返回**无效色**，
    //    直接喂给 QPen 会画成黑色。这里兜底到品牌蓝。
    QColor accent = m_color.isValid() ? m_color : ThemeManager::instance().accent();
    if (!accent.isValid()) accent = QColor(0x0A, 0x84, 0xFF);
    const QColor track = dark ? QColor(255, 255, 255, 34) : QColor(0, 0, 0, 26);

    const qreal pen = m_lineWidth;
    const QRectF box = QRectF(rect()).adjusted(pen / 2.0 + 0.5, pen / 2.0 + 0.5,
                                               -pen / 2.0 - 0.5, -pen / 2.0 - 0.5);

    // 轨道
    p.setPen(QPen(track, pen, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(box, 0, 360 * 16);

    // 进度弧：从 12 点开始顺时针
    if (m_shown > 0.0005) {
        p.setPen(QPen(accent, pen, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(box, 90 * 16, int(-m_shown * 360.0 * 16));
    }

    if (!m_textVisible) return;

    QFont f = font();
    f.setPixelSize(std::max(9, int(m_diameter * 0.26)));
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(dark ? QColor(255, 255, 255, 235) : QColor(16, 20, 26, 235));
    const QString text = m_centerText.isEmpty()
        ? QStringLiteral("%1%").arg(int(std::lround(m_shown * 100)))
        : m_centerText;
    p.drawText(rect(), Qt::AlignCenter, text);
}

} // namespace kc::ui
