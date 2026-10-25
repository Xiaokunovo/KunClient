#include "ui/dialogs/GlassDialog.h"

#include "ui/MainShell.h"
#include "ui/theme/ThemeManager.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

namespace kc::ui {

namespace {

QColor titleColor(bool dark)
{
    return dark ? QColor(255, 255, 255, 240) : QColor(16, 20, 26, 240);
}

/// 🔴 Qt QSS 的颜色坑：`#aarrggbb`（QColor::name(HexArgb)）不认，会静默回退黑；
///    `rgba(r,g,b,255)` 也不可靠。不透明色一律输出 `#rrggbb`，半透明才用 rgba()。
QString cssColor(const QColor& c)
{
    if (c.alpha() >= 255) return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

} // namespace

GlassDialog::GlassDialog(QWidget* parent)
    : AnimatedDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setModal(true);

    m_style.tint = 0.88;      // 弹窗要放正文，必须够实
    m_style.frost = 26.0;
    m_style.edgeBand = 0.18;
    m_style.lightPos = QPointF(0.5, 0.0);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 18, 22, 18);
    root->setSpacing(12);

    m_title = new QLabel(this);
    m_title->setWordWrap(true);
    m_title->hide();
    root->addWidget(m_title);

    m_body = new QWidget(this);
    m_body->setAttribute(Qt::WA_TranslucentBackground, true);
    m_bodyLayout = new QVBoxLayout(m_body);
    m_bodyLayout->setContentsMargins(0, 0, 0, 0);
    m_bodyLayout->setSpacing(8);
    root->addWidget(m_body);

    auto* buttonHost = new QWidget(this);
    buttonHost->setAttribute(Qt::WA_TranslucentBackground, true);
    m_buttonRow = new QHBoxLayout(buttonHost);
    m_buttonRow->setContentsMargins(0, 0, 0, 0);
    m_buttonRow->setSpacing(10);
    m_buttonRow->addStretch(1);
    root->addWidget(buttonHost);
}

GlassDialog::~GlassDialog() = default;

void GlassDialog::setTitle(const QString& text)
{
    m_title->setText(text);
    m_title->setVisible(!text.isEmpty());
    m_title->setStyleSheet(QStringLiteral(
        "background: transparent; color: %1; font-size: 15px; font-weight: 700;")
        .arg(cssColor(titleColor(ThemeManager::instance().isDark()))));
}

void GlassDialog::setRadius(qreal radius)
{
    m_radius = radius;
    refreshBackdrop();
    update();
}

QPushButton* GlassDialog::addButton(const QString& text, ButtonKind kind,
                                    std::function<void()> onClick)
{
    auto* btn = new QPushButton(text, this);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setMinimumHeight(36);
    btn->setMinimumWidth(96);

    // 🔴 按钮外观走全局 QSS（objectName），不用控件级 setStyleSheet：
    //    在这种 frameless 透明窗口里，控件级 `background` 会被别处规则盖掉，
    //    实测主按钮永远是黑块（color 却正常）。
    if (kind == ButtonKind::Danger) {
        btn->setObjectName(QStringLiteral("glassBtnDanger"));
    } else if (kind == ButtonKind::Primary) {
        btn->setObjectName(QStringLiteral("glassBtnPrimary"));
    } else {
        btn->setObjectName(QStringLiteral("glassBtnGhost"));
    }

    m_buttonRow->addWidget(btn);
    if (onClick) {
        QObject::connect(btn, &QPushButton::clicked, this, [onClick] { onClick(); });
    }
    return btn;
}

void GlassDialog::refreshBackdrop()
{
    MainShell* shell = MainShell::instance();
    if (!shell || size().isEmpty()) return;

    QImage raw = shell->backdropForWidget(this);
    if (raw.isNull()) return;
    if (raw.size() != size()) {
        raw = raw.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    m_backdrop = glass::process(raw, m_radius, m_style);
    update();
}

void GlassDialog::paintEvent(QPaintEvent*)
{
    // 🔴 不调 QDialog::paintEvent：QSS 的 `QDialog { background: #14181F; }`
    //    会先糊一层不透明底，玻璃就白做了。
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    glass::paint(p, QRectF(rect()), m_radius, m_backdrop,
                 ThemeManager::instance().isDark(), m_style);
}

void GlassDialog::showEvent(QShowEvent* event)
{
    AnimatedDialog::showEvent(event);   // 入场淡入 + 上浮（复用父类，含复用复位）
    refreshBackdrop();
}

void GlassDialog::resizeEvent(QResizeEvent* event)
{
    AnimatedDialog::resizeEvent(event);
    refreshBackdrop();
}

} // namespace kc::ui
