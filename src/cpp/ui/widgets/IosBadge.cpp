#include "ui/widgets/IosBadge.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>

#include <algorithm>

namespace kc::ui {

namespace {
constexpr int kH = 18;
constexpr int kMinW = 18;
} // namespace

IosBadge::IosBadge(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setFixedSize(kMinW, kH);
    setFocusPolicy(Qt::NoFocus);
    hide();
}

QString IosBadge::label() const
{
    return m_count > 99 ? QStringLiteral("99+") : QString::number(m_count);
}

void IosBadge::setCount(int count)
{
    m_count = std::max(0, count);
    if (m_count == 0) {
        hide();
        return;
    }
    QFont f = font();
    f.setPixelSize(11);
    f.setWeight(QFont::Bold);
    const int textW = QFontMetrics(f).horizontalAdvance(label());
    setFixedSize(std::max(kMinW, textW + 10), kH);
    show();
    raise();
    update();
}

void IosBadge::setBadgeColor(const QColor& color)
{
    m_color = color;
    update();
}

QSize IosBadge::sizeHint() const
{
    return QSize(std::max(kMinW, width()), kH);
}

void IosBadge::paintEvent(QPaintEvent*)
{
    if (m_count <= 0) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(m_color);
    p.drawRoundedRect(QRectF(rect()), height() / 2.0, height() / 2.0);

    QFont f = font();
    f.setPixelSize(11);
    f.setWeight(QFont::Bold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    p.drawText(rect(), Qt::AlignCenter, label());
}

} // namespace kc::ui
