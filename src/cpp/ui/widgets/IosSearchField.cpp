#include "ui/widgets/IosSearchField.h"

#include "ui/theme/ThemeManager.h"

#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>

namespace kc::ui {

namespace {
constexpr int kIconLeft = 30;    // 左侧留给放大镜
constexpr int kIconRight = 28;   // 右侧留给清除按钮
constexpr int kIconBox = 14;
} // namespace

IosSearchField::IosSearchField(QWidget* parent)
    : QLineEdit(parent)
{
    setFrame(false);
    setClearButtonEnabled(false);   // 自己画，避免原生清除按钮与自绘重叠
    setMinimumHeight(28);
    setTextMargins(kIconLeft, 0, kIconRight, 0);
    setAttribute(Qt::WA_MacShowFocusRect, false);
    // 只清掉底/边框；文字颜色仍走主题 QSS，避免和全局规则打架。
    setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: 0; padding: 0; }"));
    connect(this, &QLineEdit::textChanged, this, [this] { update(); });
    applyPalette();
}

void IosSearchField::applyPalette()
{
    const bool dark = m_forceDark || ThemeManager::instance().isDark();
    QPalette pal = palette();
    pal.setColor(QPalette::Text, dark ? QColor(235, 239, 245) : QColor(18, 22, 28));
    pal.setColor(QPalette::PlaceholderText,
                 dark ? QColor(255, 255, 255, 118) : QColor(18, 22, 28, 118));
    pal.setColor(QPalette::Highlight, QColor(10, 132, 255, 180));
    pal.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    setPalette(pal);
}

void IosSearchField::setForceDark(bool on)
{
    m_forceDark = on;
    applyPalette();
    update();
}

QSize IosSearchField::sizeHint() const
{
    return QSize(220, 32);
}

void IosSearchField::resizeEvent(QResizeEvent* event)
{
    QLineEdit::resizeEvent(event);
    update();
}

bool IosSearchField::clearHit(const QPoint& pos) const
{
    if (text().isEmpty()) return false;
    return pos.x() >= width() - kIconRight && pos.x() <= width();
}

void IosSearchField::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && clearHit(event->pos())) {
        clear();
        return;
    }
    QLineEdit::mousePressEvent(event);
}

void IosSearchField::paintEvent(QPaintEvent* event)
{
    const bool dark = m_forceDark || ThemeManager::instance().isDark();
    const qreal radius = height() / 2.0;

    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(dark ? QColor(255, 255, 255, 26) : QColor(0, 0, 0, 20), 1.0));
        p.setBrush(dark ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 10));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }

    // 文字 / 光标
    QLineEdit::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor fg = dark ? QColor(255, 255, 255, 140) : QColor(16, 20, 26, 140);
    p.setPen(QPen(fg, 1.6, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);

    // 放大镜：圆 + 斜柄
    const QRectF box(11.0, (height() - kIconBox) / 2.0, kIconBox - 4.0, kIconBox - 4.0);
    p.drawEllipse(box);
    p.drawLine(QPointF(box.right() - 0.5, box.bottom() - 0.5),
               QPointF(box.right() + 3.5, box.bottom() + 3.5));

    // 清除：灰底圆 + 白叉
    if (!text().isEmpty()) {
        const QPointF c(width() - kIconRight / 2.0 - 2.0, height() / 2.0);
        const qreal r = 7.0;
        p.setPen(Qt::NoPen);
        p.setBrush(dark ? QColor(255, 255, 255, 60) : QColor(0, 0, 0, 55));
        p.drawEllipse(c, r, r);
        p.setPen(QPen(dark ? QColor(20, 24, 30) : QColor(255, 255, 255), 1.5,
                      Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(-2.6, -2.6), c + QPointF(2.6, 2.6));
        p.drawLine(c + QPointF(2.6, -2.6), c + QPointF(-2.6, 2.6));
    }
}

} // namespace kc::ui
