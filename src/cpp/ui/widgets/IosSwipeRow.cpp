#include "ui/widgets/IosSwipeRow.h"

#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QResizeEvent>

#include <algorithm>

namespace kc::ui {

namespace {
constexpr qreal kRadius = 12;
constexpr int kDragSlop = 6;   // 超过这个位移才判定为拖拽，避免误吞点击
} // namespace

IosSwipeRow::IosSwipeRow(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(48);
}

QSize IosSwipeRow::sizeHint() const
{
    const int h = m_content ? m_content->sizeHint().height() : 48;
    return QSize(m_content ? m_content->sizeHint().width() : 240, std::max(48, h));
}

void IosSwipeRow::setContent(QWidget* content)
{
    if (m_content == content) return;
    if (m_content) {
        m_content->setParent(nullptr);
        m_content->deleteLater();
    }
    m_content = content;
    if (m_content) {
        m_content->setParent(this);
        setMinimumHeight(m_content->minimumHeight() > 0 ? m_content->minimumHeight() : 48);
        m_content->setGeometry(-int(m_offset), 0, width(), height());
        m_content->show();
    }
    updateGeometry();
    update();
}

void IosSwipeRow::setActionText(const QString& text)
{
    m_text = text;
    update();
}

void IosSwipeRow::setActionWidth(int px)
{
    m_actionW = std::max(56, px);
    update();
}

void IosSwipeRow::setActionColor(const QColor& color)
{
    m_color = color;
    update();
}

QRectF IosSwipeRow::actionRect() const
{
    // 动作块随内容左移而逐渐露出：露出多少画多少。
    const qreal reveal = std::clamp(m_offset, 0.0, qreal(m_actionW));
    return QRectF(width() - reveal, 0.0, reveal, height());
}

void IosSwipeRow::setOffset(qreal v)
{
    m_offset = std::clamp(v, 0.0, qreal(m_actionW));
    if (m_content) m_content->move(-int(m_offset), 0);
    update();
}

void IosSwipeRow::animateTo(qreal target)
{
    if (!Motion::animationsEnabled()) {
        setOffset(target);
        return;
    }
    if (!m_anim) {
        m_anim = new QPropertyAnimation(this, "offset", this);
        m_anim->setDuration(240);
        m_anim->setEasingCurve(Motion::silkEase());
    }
    m_anim->stop();
    m_anim->setStartValue(m_offset);
    m_anim->setEndValue(target);
    m_anim->start();
}

void IosSwipeRow::open()
{
    animateTo(m_actionW);
}

void IosSwipeRow::close()
{
    animateTo(0.0);
}

void IosSwipeRow::paintEvent(QPaintEvent*)
{
    if (m_offset <= 0.5) return;

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = actionRect();
    if (r.width() < 1.0) return;

    // 只给整行的右端留圆角：左边界是直切，跟着内容滑动。
    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, width(), height()), kRadius, kRadius);
    p.setClipPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(m_color);
    p.drawRect(r);

    if (r.width() > 40) {
        QFont f = font();
        f.setPixelSize(13);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(QColor(255, 255, 255, int(255 * std::clamp((r.width() - 40) / 30.0, 0.0, 1.0))));
        p.drawText(r, Qt::AlignCenter, m_text);
    }
}

void IosSwipeRow::resizeEvent(QResizeEvent* event)
{
    if (m_content) m_content->setGeometry(-int(m_offset), 0, width(), height());
    QWidget::resizeEvent(event);
}

void IosSwipeRow::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // 已打开时：点动作块 = 执行；点别处 = 收起。
    if (m_offset > 0.5) {
        if (actionRect().contains(event->pos())) {
            animateTo(0.0);
            emit actionTriggered();
            if (onAction) onAction();
            return;
        }
        animateTo(0.0);
        return;
    }

    m_pressPos = event->pos();
    m_pressOffset = m_offset;
    m_dragging = false;
    event->accept();
}

void IosSwipeRow::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int dx = event->pos().x() - m_pressPos.x();
    if (!m_dragging && std::abs(dx) > kDragSlop) m_dragging = true;
    if (!m_dragging) return;

    // 只允许向左拉；向右拉出边界时阻尼（除以 3）。
    qreal next = m_pressOffset - dx;
    if (next < 0) next /= 3.0;
    setOffset(next);
    event->accept();
}

void IosSwipeRow::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (m_dragging) {
        // 过半吸附到打开，否则弹回。
        animateTo(m_offset > m_actionW * 0.5 ? m_actionW : 0.0);
        m_dragging = false;
    }
    event->accept();
}

} // namespace kc::ui
