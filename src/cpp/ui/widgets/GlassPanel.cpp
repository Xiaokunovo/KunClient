#include "ui/widgets/GlassPanel.h"

#include "ui/MainShell.h"
#include "ui/theme/ThemeManager.h"

#include <QMoveEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>

namespace kc::ui {

GlassPanel::GlassPanel(QWidget* parent)
    : QWidget(parent)
{
    // 顶层无边框窗口：圆角与阴影全部自己画，窗口本身透明。
    setWindowFlags(Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    // 默认观感偏「实」：菜单 / Toast 上的文字必须有足够对比度，
    // 0.72 是实测在深色壁纸上白字清晰、又还能看出背后有壁纸的档位。
    m_style.tint = 0.72;
    m_style.frost = 26.0;
    m_style.edgeBand = 0.18;
    m_style.refraction = 0.55;
    m_style.dispersion = 0.35;

    m_content = new QWidget(this);
    m_content->setAttribute(Qt::WA_TranslucentBackground, true);
    m_content->setAttribute(Qt::WA_NoSystemBackground, true);
    m_contentLayout = new QVBoxLayout(m_content);
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(0);
    m_content->setGeometry(rect());
}

GlassPanel::~GlassPanel() = default;

void GlassPanel::setRadius(qreal radius)
{
    if (qFuzzyCompare(m_radius, radius)) return;
    m_radius = radius;
    m_dirty = true;
    update();
}

void GlassPanel::setStyle(const glass::Style& style)
{
    m_style = style;
    m_dirty = true;
    update();
}

void GlassPanel::setRawBackdrop(const QImage& raw)
{
    m_raw = raw;
    m_dirty = true;
    update();
}

void GlassPanel::setBackdrop(const QImage& processed)
{
    m_processed = processed;
    m_raw = QImage();
    m_dirty = false;
    update();
}

void GlassPanel::setHighlight(bool on)
{
    if (m_style.highlight == (on ? 1.0 : 0.0)) return;
    m_style.highlight = on ? 1.0 : 0.0;
    m_dirty = true;
    update();
}

void GlassPanel::refreshBackdrop()
{
    if (size().isEmpty()) return;
    MainShell* shell = MainShell::instance();
    if (!shell) return;
    const QImage raw = shell->backdropForWidget(this);
    if (raw.isNull()) return;
    m_raw = raw;
    m_dirty = true;
    update();
}

void GlassPanel::rebuild()
{
    m_dirty = false;
    if (m_raw.isNull() || size().isEmpty()) {
        m_processed = QImage();
        return;
    }
    QImage src = m_raw;
    if (src.size() != size()) {
        src = src.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    m_processed = glass::process(src, m_radius, m_style);
}

void GlassPanel::paintEvent(QPaintEvent*)
{
    if (m_dirty) rebuild();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    glass::paint(p, QRectF(rect()), m_radius, m_processed, dark, m_style);
}

void GlassPanel::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_content) m_content->setGeometry(rect());
    // 背板尺寸必须与控件一致：尺寸一变就得重取（进程内一次 blit，开销可接受）。
    refreshBackdrop();
}

void GlassPanel::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    // 换了位置 = 背后那块壁纸换了。
    refreshBackdrop();
}

void GlassPanel::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refreshBackdrop();
}

} // namespace kc::ui
