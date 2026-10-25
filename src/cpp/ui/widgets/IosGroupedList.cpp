#include "ui/widgets/IosGroupedList.h"

#include "ui/theme/ThemeManager.h"
#include "ui/widgets/IosSwitch.h"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace kc::ui {

namespace {

constexpr int kRowH = 48;
constexpr int kRowWithDetailH = 62;
constexpr int kPadX = 16;
constexpr int kRadius = 12;

QColor primary(bool dark)
{
    return dark ? QColor(255, 255, 255, 236) : QColor(16, 20, 26, 236);
}

QColor secondary(bool dark)
{
    return dark ? QColor(255, 255, 255, 130) : QColor(16, 20, 26, 130);
}

/// 圆角分组底板（自绘，不依赖 QSS）。
class GroupBox : public QWidget {
public:
    explicit GroupBox(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const bool dark = ThemeManager::instance().isDark();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 16), 1.0));
        p.setBrush(dark ? QColor(255, 255, 255, 14) : QColor(255, 255, 255, 210));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
    }
};

} // namespace

// ─── Row ────────────────────────────────────────────────────────────────────
IosGroupedList::Row::Row(const QString& title, const QString& subtitle,
                         bool chevron, QWidget* parent)
    : QWidget(parent)
    , m_chevron(chevron)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setCursor(onActivated ? Qt::PointingHandCursor : Qt::ArrowCursor);
    setMouseTracking(true);

    const bool dark = ThemeManager::instance().isDark();

    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(kPadX, 8, kPadX, 8);
    lay->setSpacing(10);

    auto* textCol = new QVBoxLayout;
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(2);

    m_title = new QLabel(title, this);
    m_title->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 13px;")
                               .arg(primary(dark).name()));
    textCol->addWidget(m_title);

    if (!subtitle.isEmpty()) {
        m_detail = new QLabel(subtitle, this);
        m_detail->setStyleSheet(
            QStringLiteral("background: transparent; color: %1; font-size: 11.5px;")
                .arg(secondary(dark).name()));
        m_detail->setWordWrap(true);
        textCol->addWidget(m_detail);
    }

    lay->addLayout(textCol, 1);

    m_value = new QLabel(this);
    m_value->setStyleSheet(
        QStringLiteral("background: transparent; color: %1; font-size: 12.5px;")
            .arg(secondary(dark).name()));
    m_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_value->hide();
    lay->addWidget(m_value, 0);

    setMinimumHeight(m_detail ? kRowWithDetailH : kRowH);
}

void IosGroupedList::Row::setValue(const QString& value)
{
    m_value->setText(value);
    m_value->setVisible(!value.isEmpty());
}

void IosGroupedList::Row::setDetail(const QString& subtitle)
{
    if (!m_detail) {
        const bool dark = ThemeManager::instance().isDark();
        m_detail = new QLabel(this);
        m_detail->setStyleSheet(
            QStringLiteral("background: transparent; color: %1; font-size: 11.5px;")
                .arg(secondary(dark).name()));
        m_detail->setWordWrap(true);
        if (auto* v = qobject_cast<QVBoxLayout*>(layout()->itemAt(0)->layout())) {
            v->addWidget(m_detail);
        }
        setMinimumHeight(kRowWithDetailH);
    }
    m_detail->setText(subtitle);
    m_detail->setVisible(!subtitle.isEmpty());
}

void IosGroupedList::Row::setRowEnabled(bool on)
{
    setEnabled(on);
    const bool dark = ThemeManager::instance().isDark();
    m_title->setStyleSheet(
        QStringLiteral("background: transparent; color: %1; font-size: 13px;")
            .arg(on ? primary(dark).name() : secondary(dark).name()));
    update();
}

void IosGroupedList::Row::setFirst(bool first)
{
    m_first = first;
    update();
}

void IosGroupedList::Row::setControl(QWidget* control)
{
    if (!control) return;
    if (auto* lay = qobject_cast<QHBoxLayout*>(layout())) {
        lay->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
    // 行里有控件时不再显示手型（点控件本身，不是点整行）。
    setClickable(false);
    updateGeometry();
}

void IosGroupedList::Row::relayout()
{
    updateGeometry();
    update();
}

void IosGroupedList::Row::paintEvent(QPaintEvent*)
{
    const bool dark = ThemeManager::instance().isDark();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (m_hover && isEnabled()) {
        p.setPen(Qt::NoPen);
        p.setBrush(dark ? QColor(255, 255, 255, 12) : QColor(0, 0, 0, 8));
        p.drawRoundedRect(rect(), kRadius, kRadius);
    }

    // 分隔线：16px 内缩，首行不画
    if (!m_first) {
        p.setPen(QPen(dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 16), 1.0));
        p.drawLine(QPointF(kPadX, 0.5), QPointF(width() - kPadX, 0.5));
    }

    // 箭头
    if (m_chevron) {
        const QColor c = dark ? QColor(255, 255, 255, 90) : QColor(16, 20, 26, 90);
        p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal cx = width() - kPadX / 2.0 - 2.0;
        const qreal cy = height() / 2.0;
        p.drawLine(QPointF(cx - 2.5, cy - 5), QPointF(cx + 2.5, cy));
        p.drawLine(QPointF(cx + 2.5, cy), QPointF(cx - 2.5, cy + 5));
    }
}

void IosGroupedList::Row::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())
        && isEnabled() && onActivated) {
        onActivated();
    }
    QWidget::mouseReleaseEvent(event);
}

void IosGroupedList::Row::enterEvent(QEnterEvent* event)
{
    m_hover = true;
    update();
    QWidget::enterEvent(event);
}

void IosGroupedList::Row::leaveEvent(QEvent* event)
{
    m_hover = false;
    update();
    QWidget::leaveEvent(event);
}

// ─── IosGroupedList ─────────────────────────────────────────────────────────
IosGroupedList::IosGroupedList(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(6);

    const bool dark = ThemeManager::instance().isDark();

    m_header = new QLabel(this);
    m_header->setStyleSheet(
        QStringLiteral("background: transparent; color: %1; font-size: 11.5px; padding-left: 4px;")
            .arg(secondary(dark).name()));
    m_header->hide();
    outer->addWidget(m_header);

    m_group = new GroupBox(this);
    m_groupLayout = new QVBoxLayout(m_group);
    m_groupLayout->setContentsMargins(0, 0, 0, 0);
    m_groupLayout->setSpacing(0);
    outer->addWidget(m_group);

    m_footer = new QLabel(this);
    m_footer->setWordWrap(true);
    m_footer->setStyleSheet(
        QStringLiteral("background: transparent; color: %1; font-size: 11px; padding-left: 4px;")
            .arg(secondary(dark).name()));
    m_footer->hide();
    outer->addWidget(m_footer);
}

void IosGroupedList::setHeader(const QString& text)
{
    m_header->setText(text);
    m_header->setVisible(!text.isEmpty());
}

void IosGroupedList::setFooter(const QString& text)
{
    m_footer->setText(text);
    m_footer->setVisible(!text.isEmpty());
}

IosGroupedList::Row* IosGroupedList::addRow(const QString& title, const QString& subtitle,
                                            const QString& value, bool chevron)
{
    auto* row = new Row(title, subtitle, chevron, m_group);
    row->setValue(value);
    row->setFirst(m_rows.isEmpty());
    m_groupLayout->addWidget(row);
    m_rows.append(row);
    return row;
}

IosGroupedList::Row* IosGroupedList::addCustomRow(const QString& title, QWidget* control,
                                                  const QString& subtitle)
{
    auto* row = new Row(title, subtitle, false, m_group);
    row->setFirst(m_rows.isEmpty());
    row->setControl(control);
    m_groupLayout->addWidget(row);
    m_rows.append(row);
    return row;
}

IosSwitch* IosGroupedList::addToggleRow(const QString& title, bool on, Row** rowOut)
{
    auto* row = new Row(title, QString(), false, m_group);
    row->setFirst(m_rows.isEmpty());

    auto* sw = new IosSwitch(row);
    sw->setCheckedSilently(on);
    // Row 的布局是 [文字列][值]；开关插到最右。
    if (auto* lay = qobject_cast<QHBoxLayout*>(row->layout())) {
        lay->addWidget(sw, 0, Qt::AlignRight | Qt::AlignVCenter);
    }

    m_groupLayout->addWidget(row);
    m_rows.append(row);
    if (rowOut) *rowOut = row;
    return sw;
}

void IosGroupedList::addSeparator()
{
    auto* line = new QWidget(m_group);
    line->setFixedHeight(1);
    line->setAttribute(Qt::WA_TranslucentBackground, true);
    m_groupLayout->addWidget(line);
    m_rows.append(line);
}

void IosGroupedList::clearRows()
{
    for (QWidget* w : m_rows) {
        m_groupLayout->removeWidget(w);
        w->deleteLater();
    }
    m_rows.clear();
}

} // namespace kc::ui
