#include "ui/widgets/DynamicIsland.h"

#include "ui/theme/GlassSettings.h"   // 液态玻璃参数（glass 档共用设置页那一套）
#include "ui/theme/Motion.h"
#include "ui/widgets/AppIcons.h"
#include "ui/widgets/IosSearchField.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QStringList>
#include <QTime>
#include <QTimer>

namespace kc::ui {

namespace {

// ── 折叠态 ──────────────────────────────────────────────────────────────────
constexpr int kCollapsedH = 30;
constexpr int kCollapsedMinW = 118;
constexpr int kCollapsedMaxW = 220;
/// hover 模式下缩成的「小短横」：视觉 34×5，控件给 44×10 —— 5px 高的热区根本点不到。
constexpr int kDashW = 44;
constexpr int kDashH = 10;
constexpr qreal kDashBarW = 34;
constexpr qreal kDashBarH = 5;
constexpr int kTopMargin = 9;   // 顶栏 48px：岛高 30，上下各 9 正好垂直居中

// ── 展开态：四段式的纵向节奏（单位 px）──────────────────────────────────────
constexpr qreal kPad = 16.0;       // 左右内边距
constexpr qreal kHeaderH = 44.0;   // ① 标题行
constexpr qreal kSearchY = 48.0;   // ② 搜索行
constexpr qreal kSearchH = 34.0;
constexpr qreal kActionY = 90.0;   // ③ 快捷动作条
constexpr qreal kActionH = 34.0;
constexpr qreal kCardsTop = 134.0; // ④ 页面卡片区
constexpr qreal kCardH = 58.0;
constexpr qreal kGap = 8.0;
constexpr qreal kReadoutH = 30.0;  // ⑤ 设备读数条
constexpr qreal kBottomPad = 16.0;
constexpr int kMaxRows = 6;        // 页面再多也不让面板无限长

// ── 配色 ────────────────────────────────────────────────────────────────────
const QColor kTextPrimary(255, 255, 255, 236);
const QColor kTextSecondary(255, 255, 255, 132);
const QColor kTextTertiary(255, 255, 255, 96);
const QColor kAccent(0x0A, 0x84, 0xFF);

/// 快捷动作条的四项。id 与 MainShell::onIslandAction 一一对应。
struct QuickAction {
    const char* id;
    const char* label;
    AppIcon icon;
};
const QuickAction kActions[] = {
    {"screenshot", "截图", AppIcon::Monitor},
    {"console", "主屏", AppIcon::Home},
    {"control", "控制中心", AppIcon::Grid},
    {"settings", "设置", AppIcon::Gear},
};
constexpr int kActionCount = int(sizeof(kActions) / sizeof(kActions[0]));

} // namespace

DynamicIsland::DynamicIsland(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("dynamicIsland"));
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setMouseTracking(true);   // 卡片 / 快捷动作都要做悬停高亮，不能等按下
    setToolTip(QStringLiteral("灵动岛 · 点击展开，左右拖动切页"));
    resize(kCollapsedMinW, kCollapsedH);

    // ── 搜索框：真子控件 ────────────────────────────────────────────────────
    // 它不在 paintEvent 里画（自绘控件里嵌输入框是必要的：自己处理输入法、选区、
    // 光标闪烁的成本远高于放一个 QLineEdit）。2026-10-24 换成 iOS 搜索框：
    // 胶囊底 + 放大镜 + 自绘清除按钮，岛体永远是深的 → setForceDark(true)。
    auto* search = new IosSearchField(this);
    search->setForceDark(true);
    search->setPlaceholderText(QStringLiteral("搜索页面…"));
    m_search = search;
    m_search->hide();
    m_search->installEventFilter(this);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& text) {
        emit searchChanged(text);
    });

    // ── 折叠态时钟 ──────────────────────────────────────────────────────────
    m_clock = new QTimer(this);
    m_clock->setInterval(1000);
    connect(m_clock, &QTimer::timeout, this, [this] {
        m_cachedCollapsedW = 0;   // 时间文字宽度会变（09:59 → 10:00）
        update();
    });

    // ── 临时提示（flashMessage）──────────────────────────────────────────────
    m_flashTimer = new QTimer(this);
    m_flashTimer->setSingleShot(true);
    connect(m_flashTimer, &QTimer::timeout, this, [this] {
        if (m_flash.isEmpty()) return;
        m_flash.clear();
        m_cachedCollapsedW = 0;
        applyGeometry();
        update();
    });
}

// ── 页面表 ──────────────────────────────────────────────────────────────────

void DynamicIsland::setPages(const QVector<IslandPage>& pages)
{
    m_pages = pages;
    if (m_hoverCard >= m_pages.size()) m_hoverCard = -1;
    if (m_expansion > 0.0) applyGeometry();   // 行数变了，面板高度跟着变
    update();
}

void DynamicIsland::setCurrentKey(const QString& key)
{
    if (m_currentKey == key) return;
    m_currentKey = key;
    update();
}

void DynamicIsland::setCurrentTitle(const QString& title)
{
    if (m_currentTitle == title) return;
    m_currentTitle = title;
    m_cachedCollapsedW = 0;   // 折叠态显示页面名时宽度要跟着变
    if (m_expansion < 0.5) applyGeometry();
    update();
}

void DynamicIsland::setBackdrop(const QImage& backdrop)
{
    if (m_backdrop.cacheKey() == backdrop.cacheKey() && m_backdrop.size() == backdrop.size()) return;
    m_backdrop = backdrop;
    update();
}

void DynamicIsland::setOptions(const IslandOptions& options)
{
    m_opt = options;
    // 折叠态宽度是内容驱动的：换了折叠档位 / 圆角不影响宽度，但主题和档位可能一起变，
    // 一律作废缓存重新量一次（量一次是微秒级，没必要为它做差量判断）。
    m_cachedCollapsedW = 0;
    updateClockTimer();
    applyGeometry();
    update();
}

void DynamicIsland::updateClockTimer()
{
    const bool needClock = (m_opt.collapsed == QLatin1String("clock"));
    if (needClock && !m_clock->isActive()) m_clock->start();
    else if (!needClock && m_clock->isActive()) m_clock->stop();
}

QString DynamicIsland::searchText() const
{
    return m_search ? m_search->text() : QString();
}

void DynamicIsland::clearSearch()
{
    if (m_search && !m_search->text().isEmpty()) m_search->clear();
}

void DynamicIsland::flashMessage(const QString& text, int ms)
{
    m_flash = text;
    m_cachedCollapsedW = 0;   // 提示文案的长度和常态那句不一样
    m_flashTimer->start(qMax(400, ms));
    applyGeometry();
    update();
}

// ── 几何 ────────────────────────────────────────────────────────────────────

int DynamicIsland::columnCount() const
{
    if (m_opt.columns <= 4) return 4;
    if (m_opt.columns >= 8) return 8;
    return 6;
}

int DynamicIsland::rowCount() const
{
    const int cols = columnCount();
    const int n = qMax(1, m_pages.size());
    return qBound(1, (n + cols - 1) / cols, kMaxRows);
}

QSize DynamicIsland::expandedSize() const
{
    const int cols = columnCount();
    const int w = (cols <= 4) ? 440 : (cols >= 8 ? 720 : 560);
    const int rows = rowCount();
    const qreal cardsH = rows * kCardH + (rows - 1) * kGap;
    const int h = qRound(kCardsTop + cardsH + 12.0 + kReadoutH + kBottomPad);
    return QSize(w, h);
}

QRectF DynamicIsland::searchRect() const
{
    return QRectF(kPad, kSearchY, width() - 2 * kPad, kSearchH);
}

QVector<QRectF> DynamicIsland::actionRects() const
{
    const qreal each = (width() - 2 * kPad - kGap * (kActionCount - 1)) / kActionCount;
    QVector<QRectF> out;
    out.reserve(kActionCount);
    for (int i = 0; i < kActionCount; ++i) {
        out.append(QRectF(kPad + i * (each + kGap), kActionY, each, kActionH));
    }
    return out;
}

QVector<QRectF> DynamicIsland::cardRects() const
{
    const int cols = columnCount();
    const int rows = rowCount();
    const qreal each = (width() - 2 * kPad - kGap * (cols - 1)) / cols;
    QVector<QRectF> out;
    out.reserve(cols * rows);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            out.append(QRectF(kPad + c * (each + kGap),
                              kCardsTop + r * (kCardH + kGap), each, kCardH));
        }
    }
    return out;
}

QRectF DynamicIsland::readoutRect() const
{
    const int rows = rowCount();
    const qreal cardsH = rows * kCardH + (rows - 1) * kGap;
    return QRectF(kPad, kCardsTop + cardsH + 12.0, width() - 2 * kPad, kReadoutH);
}

int DynamicIsland::cardAt(const QPoint& pos) const
{
    // 折叠态（或正在展开的中间态）里卡片并不存在，别误判命中。
    if (m_expansion < 0.5) return -1;
    const QVector<QRectF> rects = cardRects();
    const int count = qMin(rects.size(), m_pages.size());
    for (int i = 0; i < count; ++i) {
        if (rects.at(i).contains(QPointF(pos))) return i;
    }
    return -1;
}

int DynamicIsland::actionAt(const QPoint& pos) const
{
    if (m_expansion < 0.5) return -1;
    const QVector<QRectF> rects = actionRects();
    for (int i = 0; i < rects.size(); ++i) {
        if (rects.at(i).contains(QPointF(pos))) return i;
    }
    return -1;
}

qreal DynamicIsland::cardReveal(int index) const
{
    // 动效精简：不做错峰，整块跟岛一起出现。
    if (m_opt.liteMotion) return qBound(0.0, (m_expansion - 0.5) * 2.0, 1.0);

    // 卡片不是跟岛一起「整块出现」的：岛拉到 55% 之后才开始，行优先逐张淡入。
    // 🔴 启动间隔必须按**卡片总数**算，不能写死每张 22ms：卡片多时最后一张的
    //    启动点会超过 1.0，岛都拉满了它还是半透明 —— 截图里最后一排明显发灰
    //    （2026-10-18 实测）。这里把「55% → 100%」这段等分成 n 份。
    const int n = qMax(1, m_pages.size());
    constexpr qreal kStart = 0.55;
    constexpr qreal kSpan = 0.08;                        // 单张卡的淡入时长
    const qreal usable = qMax(0.0, 1.0 - kStart - kSpan);
    const qreal step = (n > 1) ? usable / (n - 1) : 0.0;
    const qreal base = kStart + index * step;
    return qBound(0.0, (m_expansion - base) / kSpan, 1.0);
}

bool DynamicIsland::collapsedAsDash() const
{
    return m_opt.visibility == QLatin1String("hover") && !m_hover && !m_expanded
           && m_expansion < 0.01;
}

void DynamicIsland::setState(const IslandState& state)
{
    // 折叠态宽度是**内容驱动**的：文字一变就得重新量、重新居中。
    // 但不能无条件重算 —— 外壳每秒都在喂状态，每次都 resize+move 会让胶囊抽。
    const QString before = collapsedText();
    m_state = state;
    if (m_opt.collapsed != QLatin1String("clock") && collapsedText() != before) {
        m_cachedCollapsedW = 0;
        if (m_expansion < 0.01) applyGeometry();
    }
    update();
}

QSize DynamicIsland::currentSize() const
{
    const qreal t = m_expansion;
    int cw = kCollapsedMinW;
    int ch = kCollapsedH;

    if (collapsedAsDash()) {
        cw = kDashW;
        ch = kDashH;
    } else {
        if (m_cachedCollapsedW <= 0) {
            QFont f = font();
            f.setPixelSize(11);
            f.setWeight(QFont::DemiBold);
            // clock 档用固定宽度：时间每跳一分钟宽度就变会让胶囊抽一下。
            const QString text = (m_opt.collapsed == QLatin1String("clock"))
                                     ? QStringLiteral("00:00")
                                     : collapsedText();
            const int textW = QFontMetrics(f).horizontalAdvance(text);
            m_cachedCollapsedW = qBound(kCollapsedMinW, 20 + 9 + textW + 16, kCollapsedMaxW);
        }
        cw = m_cachedCollapsedW;
    }

    const QSize es = expandedSize();
    return QSize(qRound(cw + (es.width() - cw) * t),
                 qRound(ch + (es.height() - ch) * t));
}

void DynamicIsland::applyGeometry()
{
    const QSize sz = currentSize();
    if (size() != sz) resize(sz);
    reposition();
    layoutSearch();
}

void DynamicIsland::reposition()
{
    QWidget* host = parentWidget();
    if (!host) return;
    const int x = (host->width() - width()) / 2;
    // 折叠态的三种高度（30 胶囊 / 10 短横 / 展开后的面板）都要**竖直居中在顶栏里**：
    // 一律按 kTopMargin 贴顶的话，hover 档那根 10px 短横会贴着顶栏上沿，看着像没对齐。
    const int y = kTopMargin + qMax(0, (kCollapsedH - qMin(height(), kCollapsedH)) / 2);
    move(qMax(0, x), y);
}

void DynamicIsland::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    layoutSearch();
}

void DynamicIsland::layoutSearch()
{
    if (!m_search) return;
    // 搜索框只在展开到 60% 之后才出现：更早出现会浮在还没拉开的胶囊外面。
    const qreal t = m_expansion;
    if (t < 0.6) {
        if (m_search->isVisible()) m_search->hide();
        return;
    }
    const QRectF r = searchRect();
    m_search->setGeometry(r.toRect());
    if (!m_search->isVisible()) m_search->show();
    m_search->raise();
}

void DynamicIsland::setExpanded(bool expanded)
{
    if (m_expanded == expanded) return;
    m_expanded = expanded;
    if (!expanded) clearSearch();

    const qreal from = m_expansion;
    const qreal to = expanded ? 1.0 : 0.0;
    if (!Motion::animationsEnabled()) {
        setExpansion(to);
        return;
    }
    if (m_anim) {
        m_anim->stop();
        m_anim = nullptr;
    }
    // appleEase：无过冲 —— 岛是"拉长"而不是"弹开"，过冲会显得廉价。
    const int duration = m_opt.liteMotion ? 180 : 340;
    m_anim = Motion::tween(this, "expansion", from, to, duration, Motion::appleEase());
    Motion::start(m_anim);
}

void DynamicIsland::setExpansion(qreal value)
{
    m_expansion = qBound(0.0, value, 1.0);
    applyGeometry();
    update();
    emit expansionChanged(m_expansion);
}

// ── 文字 / 配色 ─────────────────────────────────────────────────────────────

QString DynamicIsland::collapsedText() const
{
    // 临时提示优先于一切档位（否则「已复制截图」会被设备名顶掉）。
    if (!m_flash.isEmpty()) return m_flash;

    const QString mode = m_opt.collapsed;
    if (mode == QLatin1String("clock")) {
        return QTime::currentTime().toString(QStringLiteral("HH:mm"));
    }
    if (mode == QLatin1String("device")) {
        const QString d = m_state.device.trimmed();
        return (d.isEmpty() || d == QLatin1String("-")) ? QStringLiteral("无设备") : d;
    }
    if (mode == QLatin1String("page")) {
        return m_currentTitle.trimmed().isEmpty() ? QStringLiteral("主屏") : m_currentTitle;
    }

    // auto：进度 > 连接 > 设备 > 就绪
    if (m_state.percent >= 0) {
        return QStringLiteral("%1 %2%")
            .arg(m_state.task.isEmpty() ? QStringLiteral("处理中") : m_state.task)
            .arg(m_state.percent);
    }
    if (m_state.linkState == QLatin1String("ok")) {
        const QString d = m_state.device.trimmed();
        return (d.isEmpty() || d == QLatin1String("-")) ? QStringLiteral("已连接") : d;
    }
    if (m_state.linkState == QLatin1String("failed")) return QStringLiteral("连接异常");
    const QString d = m_state.device.trimmed();
    if (!d.isEmpty() && d != QLatin1String("-")) return d;
    return QStringLiteral("就绪");
}

QColor DynamicIsland::statusColor() const
{
    if (m_state.linkState == QLatin1String("failed")) return QColor(0xFF, 0x45, 0x3A);
    if (m_state.percent >= 0) return kAccent;
    if (m_state.linkState == QLatin1String("ok")) return QColor(0x32, 0xD7, 0x4B);
    return QColor(0x8E, 0x8E, 0x93);
}

QColor DynamicIsland::bodyColor() const
{
    // 三档配色。无论哪一档，展开态都要求足够实心 —— 岛体透出底下页面时，
    // 卡片之间的空槽里能看到主屏图标的鬼影（2026-10-18 截图实测）。
    if (m_opt.theme == QLatin1String("accent")) return QColor(0x0A, 0x2E, 0x52);
    if (m_opt.theme == QLatin1String("glass")) return QColor(0x16, 0x1A, 0x22);
    return QColor(0x0B, 0x0C, 0x10);
}

QColor DynamicIsland::bodyStrokeColor() const
{
    const int base = m_opt.theme == QLatin1String("glass") ? 40 : 26;
    const int hover = m_opt.theme == QLatin1String("glass") ? 62 : 44;
    return QColor(255, 255, 255, m_hover ? hover : qRound(base + 18 * m_expansion));
}

// ── 绘制 ────────────────────────────────────────────────────────────────────

void DynamicIsland::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal t = m_expansion;
    const qreal radius = (kCollapsedH / 2.0) + (qMax(8, m_opt.radius) - kCollapsedH / 2.0) * t;

    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    QColor body = bodyColor();
    // 折叠态给一点通透（hover 时更亮），展开态按 t 把 alpha 插到 255。
    const int bodyAlpha = m_hover ? 253 : 243;
    const int solid = (m_opt.theme == QLatin1String("glass")) ? 244 : 255;
    body.setAlpha(qRound(bodyAlpha + (solid - bodyAlpha) * t));

    // ── 岛体材质 ────────────────────────────────────────────────────────────
    // glass 档且外壳已经喂了背板 → 走**真液态玻璃**（磨砂 + 边缘折射 + 顶沿高光）。
    // 否则退回「深色实心 + 一道高光」—— 那是 2026-10-18 的旧观感，只作为
    // 背板还没就绪（启动瞬间 / 视频壁纸首帧前）时的兜底，不能什么都不画。
    const bool realGlass = (m_opt.theme == QLatin1String("glass")) && !m_backdrop.isNull();
    if (realGlass) {
        glass::Style st = currentGlassStyle();
        st.press = 0.0;                    // 岛体不响应按压果冻
        // 🔴 tint 不能低于 0.86：岛展开时底下是主屏图标网格，玻璃一透就能
        //    看到卡片之间空槽里的图标（0.60 / 0.68 / 0.80 逐档实测，前两档
        //    能清楚看见图标，0.80 仍能看出轮廓）。
        //    岛是「浮在内容之上」的控件，和 Dock（浮在壁纸上）性质不同 ——
        //    iOS 的岛展开态本来就接近实心，玻璃感靠**边缘折射 + 顶沿高光**撑，
        //    不靠透。0.86 留 14% 透出壁纸色调，够有材质又不脏。
        st.tint = qMax(st.tint, 0.86);
        st.edgeBand = 0.16;                // 岛体是大圆角块，按标准带宽即可
        st.lightPos = QPointF(0.5, 0.0);   // 光从正上方来（岛贴在窗口顶沿）
        glass::paint(p, QRectF(rect()), radius, m_backdrop, /*dark=*/true, st);
    } else {
        p.fillPath(path, body);

        // glass 档补一道自上而下的高光，否则和 ink 档在深色背景上几乎分不出来。
        if (m_opt.theme == QLatin1String("glass")) {
            QLinearGradient gloss(0, 0, 0, height());
            gloss.setColorAt(0.0, QColor(255, 255, 255, 26));
            gloss.setColorAt(0.45, QColor(255, 255, 255, 6));
            gloss.setColorAt(1.0, QColor(255, 255, 255, 0));
            p.fillPath(path, gloss);
        }

        // 描边随展开加亮：深色主题下背景压暗后整窗都很黑，面板只靠底色已经分不出边界。
        p.setPen(QPen(bodyStrokeColor(), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

    p.setClipPath(path);

    if (t < 0.5) {
        p.setOpacity(1.0 - t * 2.0);
        if (collapsedAsDash()) {
            // hover 模式下的「有内容」提示：一条居中的小短横。
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 150));
            const QRectF bar((width() - kDashBarW) / 2.0, (height() - kDashBarH) / 2.0,
                             kDashBarW, kDashBarH);
            p.drawRoundedRect(bar, kDashBarH / 2.0, kDashBarH / 2.0);
        } else {
            paintCollapsed(p);
        }
    } else {
        p.setOpacity((t - 0.5) * 2.0);
        paintHeader(p);
        paintActions(p);
        paintCards(p);
        paintReadout(p);
    }
    p.setOpacity(1.0);
}

void DynamicIsland::paintCollapsed(QPainter& p) const
{
    const qreal cy = height() / 2.0;
    const qreal dotX = 20.0;

    p.setPen(Qt::NoPen);
    p.setBrush(statusColor());
    p.drawEllipse(QPointF(dotX, cy), 3.5, 3.5);

    QFont f = font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(kTextPrimary);
    const QRectF textRect(dotX + 9.0, 0, width() - dotX - 20.0, height());
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(f).elidedText(collapsedText(), Qt::ElideRight,
                                          qMax(12, int(textRect.width()))));

    // 有进度时在胶囊底部铺一根 2.5px 进度线（折叠态也能一眼看到进展）。
    if (m_state.percent >= 0) {
        const qreal m = 14.0;
        const QRectF track(m, height() - 6.0, width() - 2 * m, 2.5);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 42));
        p.drawRoundedRect(track, 1.25, 1.25);
        QRectF fill = track;
        fill.setWidth(track.width() * qBound(0, m_state.percent, 100) / 100.0);
        p.setBrush(kAccent);
        p.drawRoundedRect(fill, 1.25, 1.25);
    }

    // 拖动切页时的方向提示：位移的那一侧浮出一枚箭头。
    if (m_dragActive && qAbs(m_dragDx) > 6.0) {
        const bool right = m_dragDx > 0;
        const qreal a = qBound(0.0, (qAbs(m_dragDx) - 6.0) / 24.0, 1.0);
        const qreal cx = right ? (width() - 14.0) : 14.0;
        p.setPen(QPen(QColor(255, 255, 255, int(210 * a)), 1.7, Qt::SolidLine,
                      Qt::RoundCap, Qt::RoundJoin));
        if (right) {
            p.drawLine(QPointF(cx - 3, cy - 4), QPointF(cx + 1, cy));
            p.drawLine(QPointF(cx + 1, cy), QPointF(cx - 3, cy + 4));
        } else {
            p.drawLine(QPointF(cx + 3, cy - 4), QPointF(cx - 1, cy));
            p.drawLine(QPointF(cx - 1, cy), QPointF(cx + 3, cy + 4));
        }
    }
}

void DynamicIsland::paintHeader(QPainter& p) const
{
    const qreal pad = kPad;
    const qreal w = width();

    QFont hf = font();
    hf.setPixelSize(12);
    hf.setWeight(QFont::Bold);
    p.setFont(hf);
    p.setPen(kTextPrimary);
    p.drawText(QRectF(pad, 12, w - 2 * pad, 18),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("灵动岛"));

    QFont sf = font();
    sf.setPixelSize(10);
    p.setFont(sf);
    const QRectF statusRect(pad, 12, w - 2 * pad - 26, 18);
    p.setPen(kTextSecondary);
    p.drawText(statusRect, Qt::AlignRight | Qt::AlignVCenter,
               QFontMetrics(sf).elidedText(collapsedText(), Qt::ElideRight,
                                           qMax(12, int(statusRect.width()) - 12)));
    p.setPen(Qt::NoPen);
    p.setBrush(statusColor());
    const qreal dotX = statusRect.right() - 7.0;
    p.drawEllipse(QPointF(dotX, statusRect.center().y()), 3.0, 3.0);

    const qreal chevX = w - pad - 5.0;
    const qreal chevY = 21.0;
    p.setPen(QPen(QColor(255, 255, 255, 150), 1.6, Qt::SolidLine,
                  Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(chevX - 4, chevY + 2), QPointF(chevX, chevY - 2));
    p.drawLine(QPointF(chevX, chevY - 2), QPointF(chevX + 4, chevY + 2));
}

void DynamicIsland::paintActions(QPainter& p) const
{
    const QVector<QRectF> rects = actionRects();
    for (int i = 0; i < rects.size(); ++i) {
        const QRectF r = rects.at(i);
        const bool hot = (i == m_hoverAction);
        p.setPen(Qt::NoPen);
        p.setBrush(hot ? QColor(255, 255, 255, 30) : QColor(255, 255, 255, 14));
        p.drawRoundedRect(r, 10.0, 10.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 255, 255, hot ? 54 : 22), 1.0));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 10.0, 10.0);

        // 图标 + 文字横排居中：图标 15，间距 6。
        QFont f = font();
        f.setPixelSize(11);
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        const QFontMetrics fm(f);
        const QString label = QString::fromUtf8(kActions[i].label);
        const qreal textW = fm.horizontalAdvance(label);
        const qreal iconBox = 15.0;
        const qreal totalW = iconBox + 6.0 + textW;
        const qreal x0 = r.center().x() - totalW / 2.0;

        const QRectF iconRect(x0, r.center().y() - iconBox / 2.0, iconBox, iconBox);
        drawAppIcon(p, iconRect, kActions[i].icon,
                    QColor(255, 255, 255, hot ? 240 : 196));

        p.setPen(hot ? QColor(255, 255, 255, 238) : QColor(255, 255, 255, 198));
        p.drawText(QRectF(x0 + iconBox + 6.0, r.top(), textW + 2, r.height()),
                   Qt::AlignVCenter | Qt::AlignLeft, label);
    }
}

void DynamicIsland::paintCards(QPainter& p) const
{
    const QVector<QRectF> rects = cardRects();
    const int count = qMin(rects.size(), m_pages.size());
    for (int i = 0; i < count; ++i) {
        const qreal reveal = cardReveal(i);
        if (reveal <= 0.0) continue;

        const QRectF r = rects.at(i);
        const IslandPage& page = m_pages.at(i);
        const bool hot = (i == m_hoverCard);
        const bool current = !m_currentKey.isEmpty() && page.key == m_currentKey;

        p.save();
        p.setOpacity(p.opacity() * reveal);

        // 卡面：极淡白底，悬停提亮；当前页用强调色淡底 + 描边。
        QColor face = current ? QColor(10, 132, 255, hot ? 76 : 56)
                              : QColor(255, 255, 255, hot ? 34 : 16);
        p.setPen(Qt::NoPen);
        p.setBrush(face);
        p.drawRoundedRect(r, 14.0, 14.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(current ? QColor(10, 132, 255, 190)
                              : QColor(255, 255, 255, hot ? 56 : 22),
                      1.0));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 14.0, 14.0);

        // 图标：卡片上部居中，24px。
        const qreal iconBox = 24.0;
        const QRectF iconRect(r.center().x() - iconBox / 2, r.top() + 7.0,
                              iconBox, iconBox);
        drawAppIcon(p, iconRect, appIconForKey(page.key),
                    QColor(255, 255, 255, current ? 250 : 214));

        // 名字：卡片底部居中。
        QFont lf = font();
        lf.setPixelSize(10);
        lf.setWeight(current ? QFont::DemiBold : QFont::Normal);
        p.setFont(lf);
        p.setPen(current ? QColor(255, 255, 255, 240) : QColor(255, 255, 255, 178));
        const QRectF labelRect(r.left() + 3, r.bottom() - 21.0, r.width() - 6, 18);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignVCenter,
                   QFontMetrics(lf).elidedText(page.title, Qt::ElideRight,
                                               qMax(12, int(labelRect.width()))));
        p.restore();
    }
}

void DynamicIsland::paintReadout(QPainter& p) const
{
    const QRectF r = readoutRect();

    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    const QFontMetrics fm(f);

    // 一条读完：设备 · 模式 · 任务 · 进度。全是 "-" 时给一句人话。
    QStringList parts;
    auto push = [&parts](const QString& v) {
        const QString t = v.trimmed();
        if (!t.isEmpty() && t != QLatin1String("-")) parts << t;
    };
    push(m_state.device);
    push(m_state.mode);
    if (m_state.percent >= 0) parts << QStringLiteral("%1%").arg(m_state.percent);
    else push(m_state.task);

    const QString text = parts.isEmpty() ? QStringLiteral("暂无设备读数")
                                         : parts.join(QStringLiteral("  ·  "));
    p.setPen(kTextTertiary);
    p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter,
               fm.elidedText(text, Qt::ElideRight, qMax(12, int(r.width()))));
}

// ── 交互 ────────────────────────────────────────────────────────────────────

void DynamicIsland::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressPos = event->pos();
        m_dragArmed = true;
        m_dragActive = false;
        m_dragDx = 0.0;
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void DynamicIsland::mouseMoveEvent(QMouseEvent* event)
{
    // 折叠态：横向拖过 8px 就进入「切页跟手」。
    if (m_dragArmed && !m_expanded) {
        const qreal dx = event->pos().x() - m_pressPos.x();
        if (!m_dragActive && qAbs(dx) > 8.0) m_dragActive = true;
        if (m_dragActive) {
            m_dragDx = dx;
            update();
            event->accept();
            return;
        }
    }

    const int card = cardAt(event->pos());
    const int action = actionAt(event->pos());
    if (card != m_hoverCard || action != m_hoverAction) {
        m_hoverCard = card;
        m_hoverAction = action;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void DynamicIsland::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        const bool wasDragging = m_dragActive;
        const qreal dx = m_dragDx;
        m_dragArmed = false;
        m_dragActive = false;
        m_dragDx = 0.0;

        if (wasDragging) {
            // 松手位移过 30px 才切页，否则回弹（什么都不做）。
            if (qAbs(dx) > 30.0) emit pageStepRequested(dx > 0 ? -1 : 1);
            update();
            event->accept();
            return;
        }

        // 展开态：先看快捷动作，再看页面卡片，最后才当成「收起」。
        const int action = actionAt(event->pos());
        if (action >= 0 && action < kActionCount) {
            emit actionTriggered(QString::fromLatin1(kActions[action].id));
            event->accept();
            return;
        }
        // 点在页面卡片上 → 只发信号，**不**自己收起（收不收由外壳决定）；
        // 点别处才是展开 / 收起。
        const int card = cardAt(event->pos());
        if (card >= 0 && card < m_pages.size()) {
            emit pageRequested(m_pages.at(card).key);
            event->accept();
            return;
        }
        setExpanded(!m_expanded);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

bool DynamicIsland::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_search) {
        if (event->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(event);
            if (ke->key() == Qt::Key_Escape) {
                // Esc：有内容先清空，空了才收起面板（两级退出，符合直觉）。
                if (!m_search->text().isEmpty()) {
                    m_search->clear();
                } else {
                    setExpanded(false);
                }
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                // 回车 = 进第一张卡片，不用再抬鼠标去点。
                if (!m_pages.isEmpty()) emit pageRequested(m_pages.first().key);
                return true;
            }
        }
        // 搜索框里按下鼠标不该被岛的拖动逻辑接走。
        if (event->type() == QEvent::MouseButtonPress) {
            m_dragArmed = false;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DynamicIsland::enterEvent(QEnterEvent* event)
{
    m_hover = true;
    if (collapsedAsDash() || m_opt.visibility == QLatin1String("hover")) {
        applyGeometry();   // hover 档：从 34×5 的小短横长回完整胶囊
    }
    update();
    QWidget::enterEvent(event);
}

void DynamicIsland::leaveEvent(QEvent* event)
{
    m_hover = false;
    m_hoverCard = -1;
    m_hoverAction = -1;
    if (m_opt.visibility == QLatin1String("hover") && !m_expanded) applyGeometry();
    update();
    QWidget::leaveEvent(event);
}

} // namespace kc::ui
