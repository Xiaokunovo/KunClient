#include "ui/widgets/GlassSurface.h"

#include "ui/theme/ThemeManager.h"

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace kc::ui {
namespace {

/// 没有真实背板时自造一张「材质测试卡」。
///
/// 🔴 这张图的目的不是「好看」，是**让三个参数都看得见**：
///   · 等距细网格 —— 磨砂一拉就糊成一片（磨砂强度有没有生效）；
///   · 高对比色块 + 圆环 —— 边缘折射会把它们「吸」进去、边界会弯
///     （折射强度有没有生效）；纯渐变折射出来还是渐变，等于测不出来；
///   · 白/黑交界处 —— 色散在这里会浮出彩虹边（色散有没有生效）。
///
/// 真实接入时应当由调用方喂真背板；这个只是「用户没设壁纸 / 用的是视频壁纸」
/// 时的兜底，不能让预览变成一块纯色。
QImage makeFallbackBackdrop(const QSize& size, bool dark) {
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    if (img.isNull()) return img;
    img.fill(dark ? QColor(0x10, 0x14, 0x1C) : QColor(0xEC, 0xF1, 0xF8));

    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    const qreal w = r.width();
    const qreal h = r.height();

    // ① 底色：两团大色斑，给玻璃一点「有环境光」的底色。
    QRadialGradient cool(QPointF(w * 0.12, h * 0.06), std::max(w, h) * 0.72);
    cool.setColorAt(0.0, dark ? QColor(0x2F, 0x6F, 0xD0, 235) : QColor(0x6F, 0xAE, 0xF0, 220));
    cool.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(r, cool);

    QRadialGradient warm(QPointF(w * 0.90, h * 1.04), std::max(w, h) * 0.66);
    warm.setColorAt(0.0, dark ? QColor(0xD0, 0x6A, 0x2F, 200) : QColor(0xF0, 0xA8, 0x6F, 190));
    warm.setColorAt(1.0, QColor(0, 0, 0, 0));
    p.fillRect(r, warm);

    // ② 等距细网格：磨砂强度的「尺子」。
    //    🔴 间距必须**大于磨砂半径**才测得出来：网格比磨砂还密的话，一进磨砂就
    //    糊成一片灰，用户看到的是「什么都没有」而不是「磨砂生效了」。
    //    这里按面板高度取 1/6，磨砂拉满（48px）刚好糊掉，拉到 0 则根根分明。
    const qreal gridStep = std::max(12.0, h / 6.0);
    p.setPen(QPen(dark ? QColor(255, 255, 255, 76) : QColor(255, 255, 255, 215), 1.2));
    for (qreal x = 0; x < w; x += gridStep) p.drawLine(QPointF(x, 0), QPointF(x, h));
    for (qreal y = 0; y < h; y += gridStep) p.drawLine(QPointF(0, y), QPointF(w, y));

    // ③ 高对比色块：折射会把这些方块的边缘「吸」向玻璃外沿。
    //    尺寸同样要压过磨砂半径，否则糊成彩色光斑就看不出「被挤压」了。
    //    🔴 位置必须**压边**：折射只影响边缘带内的像素，色块全堆在中间的话
    //    无论怎么调都看不到形变（平行于边缘的线条位移后仍是直线，也测不出来）。
    const QColor chips[] = {
        QColor(0xFF, 0x45, 0x3A),   // 红
        QColor(0x32, 0xD7, 0x4B),   // 绿
        QColor(0x0A, 0x84, 0xFF),   // 蓝
        QColor(0xFF, 0xD6, 0x0A),   // 黄
    };
    const qreal chip = std::max(24.0, h * 0.52);
    for (int i = 0; i < 4; ++i) {
        // 首尾两块有一半在画外 —— 折射带正好从它们身上切过去，形变最明显。
        const qreal cx = (i == 0) ? 0.0 : (i == 3 ? w : w * 0.34 * i);
        const qreal cy = h * (i % 2 == 0 ? 0.30 : 0.72);
        p.setPen(Qt::NoPen);
        p.setBrush(chips[i]);
        p.drawRoundedRect(QRectF(cx - chip / 2, cy - chip / 2, chip, chip), chip * 0.28, chip * 0.28);
    }

    // ④ 白圆环：折射会让圆环在贴边处明显「胀」出去，色散会把它染成彩边。
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(255, 255, 255, dark ? 210 : 240), std::max(2.0, h * 0.026)));
    p.drawEllipse(QPointF(w * 0.50, h * 0.50), h * 0.36, h * 0.36);

    // ⑤ 一条黑白硬边：色散在硬边上最容易看出来（一侧偏红、一侧偏青）。
    p.setPen(Qt::NoPen);
    p.setBrush(dark ? QColor(0, 0, 0, 150) : QColor(255, 255, 255, 190));
    p.drawRect(QRectF(0, h * 0.86, w, h * 0.14));

    p.end();
    return img;
}

} // namespace

GlassSurface::GlassSurface(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);

    m_pressAnim = new QVariantAnimation(this);
    connect(m_pressAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_style.press = v.toReal();
        update();
    });

    m_lightAnim = new QVariantAnimation(this);
    m_lightAnim->setDuration(140);
    m_lightAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_lightAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_style.lightPos = v.toPointF();
        update();   // 高光是矢量绘制，不重算背板
    });

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](bool) { m_dirty = true; update(); });
}

GlassSurface::~GlassSurface() = default;

void GlassSurface::setStyle(const glass::Style& style)
{
    // 只有「会改变背板像素」的参数才触发重算。tint / highlight / lightPos / press
    // 是纯绘制层参数，逐帧变都不该重算 —— 那会把拖滑条拖成幻灯片。
    const bool needRebuild =
        !qFuzzyCompare(m_style.frost + 1.0, style.frost + 1.0)
        || !qFuzzyCompare(m_style.refraction + 1.0, style.refraction + 1.0)
        || !qFuzzyCompare(m_style.dispersion + 1.0, style.dispersion + 1.0)
        || !qFuzzyCompare(m_style.edgeBand + 1.0, style.edgeBand + 1.0);

    m_style = style;
    if (needRebuild) m_dirty = true;
    update();
}

void GlassSurface::setRadius(qreal radius)
{
    const qreal r = std::max(0.0, radius);
    if (qFuzzyCompare(m_radius + 1.0, r + 1.0)) return;
    m_radius = r;
    m_dirty = true;
    update();
}

void GlassSurface::setBackdrop(const QImage& processed)
{
    m_processed = processed;
    m_raw = QImage();
    m_dirty = false;
    update();
}

void GlassSurface::setRawBackdrop(const QImage& raw)
{
    m_raw = raw;
    m_dirty = true;
    update();
}

void GlassSurface::grabBackdropFrom(QWidget* source)
{
    if (!source) return;
    QWidget* top = source->window();
    if (!top) return;

    // 只抓 source 所在的那一块：全窗 grab 是同步全渲染，能省一点是一点。
    // 坐标换算走 mapTo(top, {0,0})，兼容 source 嵌在多层布局里的情况。
    const QPoint origin = source->mapTo(top, QPoint(0, 0));
    const QPixmap shot = top->grab(QRect(origin, source->size()));
    if (shot.isNull()) return;
    m_raw = shot.toImage();
    m_dirty = true;
    update();
}

void GlassSurface::setInteractive(bool on)
{
    m_interactive = on;
    setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void GlassSurface::setFallbackBackdrop(bool on)
{
    m_fallback = on;
    m_dirty = true;
    update();
}

void GlassSurface::rebuild()
{
    m_dirty = false;
    if (size().isEmpty()) {
        m_processed = QImage();
        return;
    }

    if (m_raw.isNull() && m_fallback) {
        m_raw = makeFallbackBackdrop(size(), ThemeManager::instance().isDark());
    }
    if (m_raw.isNull()) {
        m_processed = QImage();
        return;
    }

    QImage src = m_raw;
    if (src.size() != size()) {
        src = src.scaled(size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    m_processed = glass::process(src, m_radius, m_style);
}

void GlassSurface::paintEvent(QPaintEvent*)
{
    if (m_dirty) rebuild();

    QPainter p(this);
    glass::paint(p, QRectF(rect()), m_radius, m_processed,
                 ThemeManager::instance().isDark(), m_style);
}

void GlassSurface::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    m_dirty = true;
}

void GlassSurface::animatePress(qreal target)
{
    m_pressAnim->stop();
    m_pressAnim->setStartValue(m_style.press);
    m_pressAnim->setEndValue(target);
    if (target > 0.5) {
        // 下压：快、略慢出 —— 玻璃是「被按下去」而不是「弹上来」。
        m_pressAnim->setDuration(120);
        m_pressAnim->setEasingCurve(QEasingCurve::OutCubic);
    } else {
        // 回弹：长一点、带过冲。iOS 的果冻感全在这条 OutBack 上。
        m_pressAnim->setDuration(460);
        m_pressAnim->setEasingCurve(QEasingCurve::OutBack);
    }
    m_pressAnim->start();
}

void GlassSurface::retargetLight(const QPointF& pos)
{
    const qreal w = std::max(1, width());
    const qreal h = std::max(1, height());
    const QPointF target(std::clamp(pos.x() / w, 0.0, 1.0),
                         std::clamp(pos.y() / h, 0.0, 1.0));
    if (QLineF(target, m_lightTarget).length() < 0.004) return;
    m_lightTarget = target;

    m_lightAnim->stop();
    m_lightAnim->setStartValue(m_style.lightPos);
    m_lightAnim->setEndValue(target);
    m_lightAnim->start();
}

void GlassSurface::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressed = true;
    m_pressPos = event->pos();
    if (m_interactive) animatePress(1.0);
    retargetLight(event->position());
    event->accept();
}

void GlassSurface::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasPressed = m_pressed;
    m_pressed = false;
    if (m_interactive) animatePress(0.0);

    if (wasPressed && rect().contains(event->pos())) emit clicked();
    event->accept();
}

void GlassSurface::mouseMoveEvent(QMouseEvent* event)
{
    retargetLight(event->position());
    QWidget::mouseMoveEvent(event);
}

void GlassSurface::leaveEvent(QEvent* event)
{
    // 指针离开：光源滑回默认的左上角（iOS 上「没有指针照着」就是默认光位）。
    m_lightTarget = QPointF(0.22, 0.10);
    m_lightAnim->stop();
    m_lightAnim->setStartValue(m_style.lightPos);
    m_lightAnim->setEndValue(m_lightTarget);
    m_lightAnim->start();
    QWidget::leaveEvent(event);
}

} // namespace kc::ui
