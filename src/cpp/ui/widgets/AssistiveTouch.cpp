#include "ui/widgets/AssistiveTouch.h"

#include "ui/theme/ThemeManager.h"
#include "ui/widgets/AppIcons.h"
#include "ui/widgets/GlassMenu.h"

#include <QEnterEvent>
#include <QIcon>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QVector>

#include <algorithm>

namespace kc::ui {

// ─────────────────────────────────────────────────────────────────────────────
//  实现分节：
//    ① 常量与菜单表 —— 菜单条目「id / 文案 / 图标」只写一份，menuActionIds()
//       与 openMenu() 都从这张表取，顺序天然一致（外壳按 id 分派动作，两边
//       顺序错一位就是「点主屏跳到了控制中心」这类难查的错位）。
//    ② 构造 + 位置恢复 —— 位置存**比例**，窗口尺寸变了仍贴原来那条边。
//    ③ 自绘 —— 圆底 / 描边 / 九点，全部 QPainter 矢量，零字体字形。
//    ④ 拖动 —— 全局坐标 → 父坐标必须走 host->mapFromGlobal()（见注释里的坑）。
//    ⑤ 菜单 —— 堆上建 + execAt（嵌套事件循环），收起后 deleteLater。
// ─────────────────────────────────────────────────────────────────────────────

namespace {

constexpr int kBallSize = 48;      ///< 直径（也是控件尺寸，圆正好铺满）
constexpr int kSnapMargin = 8;     ///< 吸附后离父控件左/右边缘的距离
constexpr int kDragMargin = 8;     ///< 拖动时不许越过的父控件内边距
constexpr int kDragSlop = 4;       ///< 位移超过这么多像素才算「拖动」，否则算点击
constexpr int kDotGrid = 3;        ///< 九点图标的行列数
constexpr qreal kDotGap = 6.0;     ///< 相邻点的中心距
constexpr qreal kDotRadius = 1.6;  ///< 单点半径
constexpr int kMenuIconSize = 16;

/// 位置键：值是 "xRatio,yRatio"（相对父控件可用宽/高的比例，0~1）。
constexpr char kPosKey[] = "ui/assistiveTouchPos";

/// 菜单项的一次性释放：GlassMenu::dismiss() 只 hide()、不 close()，所以
/// WA_DeleteOnClose 永远不会触发 —— 堆上那个菜单必须自己 deleteLater。
/// 用属性做去重：选中条目时 lambda 已经排过一次队，execAt 返回后这里不能再排
/// 第二次（同一个对象排两个 DeferredDelete 事件是未定义行为）。
void disposeMenuOnce(GlassMenu* menu)
{
    if (!menu) return;
    if (menu->property("kcDisposeQueued").toBool()) return;
    menu->setProperty("kcDisposeQueued", true);
    menu->deleteLater();
}

struct MenuEntry {
    QString id;
    QString label;
    AppIcon icon;
};

/// 菜单条目表（顺序即显示顺序）。id 与外壳的分派 switch 一一对应。
const QVector<MenuEntry>& menuEntries()
{
    static const QVector<MenuEntry> table = {
        {QStringLiteral("console"),   QStringLiteral("主屏"),     AppIcon::Home},
        {QStringLiteral("control"),   QStringLiteral("控制中心"), AppIcon::Grid},
        {QStringLiteral("shortcuts"), QStringLiteral("快捷指令"), AppIcon::Bolt},
        {QStringLiteral("history"),   QStringLiteral("刷机记录"), AppIcon::Folder},
        {QStringLiteral("standby"),   QStringLiteral("待机模式"), AppIcon::Monitor},
        {QStringLiteral("focus"),     QStringLiteral("专注模式"), AppIcon::Gear},
    };
    return table;
}

} // namespace

// ── ① 构造 / 位置 ────────────────────────────────────────────────────────────

AssistiveTouch::AssistiveTouch(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("assistiveTouch"));
    setFixedSize(kBallSize, kBallSize);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setToolTip(QStringLiteral("辅助触控"));

    // 位置以**比例**存：用户把窗口拉大 / 最大化之后，球还贴在原来那条边、
    // 同样的相对高度上。存绝对像素的话，1080p 换成 4K 会跑到屏幕外，
    // 缩小窗口又会跑到父控件外面（然后被 clamp 到左上角，用户以为丢了）。
    const QString saved = QSettings().value(QLatin1String(kPosKey)).toString();
    const QStringList parts = saved.split(QLatin1Char(','));
    if (parts.size() == 2) {
        bool okX = false;
        bool okY = false;
        const qreal x = parts.at(0).trimmed().toDouble(&okX);
        const qreal y = parts.at(1).trimmed().toDouble(&okY);
        if (okX) m_xRatio = std::clamp(x, 0.0, 1.0);
        if (okY) m_yRatio = std::clamp(y, 0.0, 1.0);
    }
    repositionToParent();

    // 球的配色全在 paintEvent 里现取，换主题只需重绘一次。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](bool) { update(); });
}

void AssistiveTouch::repositionToParent()
{
    QWidget* host = parentWidget();
    if (!host) return;

    // 可用区间 = 父控件尺寸 - 球自身尺寸；为 0 时（父控件比球还小）退化成 1，
    // 避免除零 —— 此时 x/y 都会被 clamp 到 0。
    const int availX = std::max(1, host->width() - width());
    const int availY = std::max(1, host->height() - height());

    int x = qRound(m_xRatio * availX);
    int y = qRound(m_yRatio * availY);
    x = std::clamp(x, 0, std::max(0, host->width() - width()));
    y = std::clamp(y, 0, std::max(0, host->height() - height()));
    move(x, y);
}

void AssistiveTouch::snapToEdge()
{
    QWidget* host = parentWidget();
    if (!host) return;

    const int availX = std::max(1, host->width() - width());
    const int availY = std::max(1, host->height() - height());

    // 按**中心**判边：用户松手时球多半是斜着甩过去的，按中心判比按左上角
    // 更符合直觉（球有一半越过中线就归到那一侧）。
    const bool toLeft = (x() + width() / 2) < host->width() / 2;
    const int rightX = std::max(kSnapMargin, host->width() - width() - kSnapMargin);
    const int snappedX = toLeft ? kSnapMargin : rightX;

    const int maxY = std::max(kSnapMargin, host->height() - height() - kSnapMargin);
    const int snappedY = std::clamp(y(), kSnapMargin, maxY);

    // 比例写盘前必须夹到 [0,1]：父控件比球还小的时候 availX/availY 会退化成 1，
    // 直接除出来的比例能到 8，存进 ini 后下次读出来再夹一次就对不上位置了。
    m_xRatio = std::clamp(qreal(snappedX) / qreal(availX), 0.0, 1.0);
    m_yRatio = std::clamp(qreal(snappedY) / qreal(availY), 0.0, 1.0);
    move(snappedX, snappedY);

    // 固定 4 位小数：够精确（1080p 下 < 1px），又不会让 ini 里出现一长串。
    QSettings().setValue(QLatin1String(kPosKey),
                         QStringLiteral("%1,%2")
                             .arg(m_xRatio, 0, 'f', 4)
                             .arg(m_yRatio, 0, 'f', 4));
}

// ── ③ 自绘 ───────────────────────────────────────────────────────────────────

void AssistiveTouch::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();

    // 底色：深色主题用「被提亮的深灰」，浅色主题用近白 —— 两种都要能压住
    // 背后的壁纸，否则球在亮壁纸上会糊成一团、在暗壁纸上又会消失。
    const QColor fill = dark ? QColor(28, 32, 40, 215) : QColor(250, 252, 255, 225);
    // 1px 描边就是「玻璃边」：静止时很淡，hover 时提亮，给出可点的暗示。
    const QColor stroke = dark ? QColor(255, 255, 255, m_hovered ? 120 : 60)
                               : QColor(0, 0, 0, m_hovered ? 120 : 40);

    // hover 放大 1.04：底半径取 22.5 而不是 24 —— 48×48 的盒子里，半径 24 的圆
    // 加上 0.5px 描边已经贴边，再放大 4% 会在上下左右四个切点上被裁平。
    // 22.5 × 1.04 = 23.4，加描边仍在盒内，放大是「整颗球长了一点」而不是缺角。
    const qreal scale = m_hovered ? 1.04 : 1.0;
    const qreal radius = 22.5 * scale;
    const QPointF center(width() / 2.0, height() / 2.0);

    p.setPen(QPen(stroke, 1.0));
    p.setBrush(fill);
    p.drawEllipse(center, radius, radius);

    // 九点图标：3×3 实心圆点，间距 6px。
    // 🔴 绝不用「⠿」「⋮⋮」「≡」这类字体字形 —— 小字号下 Windows 会回退到
    //    一个只有零星点阵的字体，渲染出来是一堆小圆点（项目红线，踩过）。
    QColor dot = ThemeManager::instance().color(ColorToken::Text);
    dot.setAlpha(dark ? 235 : 220);
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    const qreal half = (kDotGrid - 1) / 2.0;
    for (int row = 0; row < kDotGrid; ++row) {
        for (int col = 0; col < kDotGrid; ++col) {
            const QPointF at = center + QPointF((col - half) * kDotGap, (row - half) * kDotGap);
            p.drawEllipse(at, kDotRadius, kDotRadius);
        }
    }
}

void AssistiveTouch::enterEvent(QEnterEvent* event)
{
    m_hovered = true;
    update();
    QWidget::enterEvent(event);
}

void AssistiveTouch::leaveEvent(QEvent* event)
{
    m_hovered = false;
    update();
    QWidget::leaveEvent(event);
}

// ── ④ 拖动 ───────────────────────────────────────────────────────────────────

void AssistiveTouch::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressGlobal = event->globalPosition().toPoint();
    m_dragOffset = event->pos();   // 按下点相对球左上角，拖动时保持这个偏移
    m_dragging = false;
    event->accept();
}

void AssistiveTouch::mouseMoveEvent(QMouseEvent* event)
{
    if (!(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint global = event->globalPosition().toPoint();
    if (!m_dragging) {
        // 4px 死区：手一抖就判定成拖动的话，点一下菜单会变成「拖了一下、没点开」。
        if ((global - m_pressGlobal).manhattanLength() <= kDragSlop) return;
        m_dragging = true;
    }

    QWidget* host = parentWidget();
    if (!host) return;

    // 🔴 全局坐标 → 父坐标必须走 host->mapFromGlobal()。
    //    QWidget::mapToParent() 收的是**本控件局部**坐标（内部再叠一次自身 pos()），
    //    喂全局坐标进去会多算一份 pos()，表现为「球比鼠标快一倍、越拖越偏」。
    QPoint target = host->mapFromGlobal(global - m_dragOffset);
    const int maxX = std::max(kDragMargin, host->width() - width() - kDragMargin);
    const int maxY = std::max(kDragMargin, host->height() - height() - kDragMargin);
    target.setX(std::clamp(target.x(), kDragMargin, maxX));
    target.setY(std::clamp(target.y(), kDragMargin, maxY));
    move(target);
    event->accept();
}

void AssistiveTouch::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_dragging) {
        m_dragging = false;
        snapToEdge();          // 内部顺带写 QSettings
        emit dragged();
        event->accept();
        return;
    }

    event->accept();
    // 🔴 openMenu() 里跑嵌套事件循环（execAt），返回前外壳可能已经因为菜单里的
    //    动作把球藏起来/挪走。所以它必须是本函数的**最后一句**，返回后不再碰成员。
    openMenu();
}

// ── ⑤ 菜单 ───────────────────────────────────────────────────────────────────

void AssistiveTouch::openMenu()
{
    // 堆上建：execAt() 内部是 QEventLoop，栈对象活不到它返回。
    // 🔴 释放**不走** WA_DeleteOnClose：GlassMenu::dismiss() 只 hide()、不 close()，
    //    那个属性永远不会触发；而设成 true 之后一旦有人 close()（Alt+F4 / 退出），
    //    对象会在嵌套事件循环里被删掉，之后我们再 deleteLater 就是 use-after-free。
    //    统一走 disposeMenuOnce() 显式排队，路径唯一、可去重。
    auto* menu = new kc::ui::GlassMenu(window());
    menu->setAttribute(Qt::WA_DeleteOnClose, false);

    const QColor iconColor = ThemeManager::instance().color(ColorToken::Text);
    for (const MenuEntry& entry : menuEntries()) {
        const QString id = entry.id;   // 按值捕获：菜单表是静态的，但 id 要独立一份
        menu->addItem(QIcon(appIconPixmap(entry.icon, iconColor, kMenuIconSize)), entry.label,
                      [this, id, menu]() {
                          emit actionTriggered(id);
                          // 先收菜单再释放：外壳的动作可能开模态弹窗，菜单得先消失。
                          // 🔴 GlassMenuRow::mouseReleaseEvent 在调本回调**之前**已经
                          //    dismiss 过一次（菜单此刻已不可见）。这里必须判 isVisible，
                          //    否则 dismiss 会走「不可见 → emit hidden()」那条分支，
                          //    把 hidden 发两遍（外壳若在 hidden 里做清理会重复执行）。
                          if (menu->isVisible()) menu->dismiss(false);
                          disposeMenuOnce(menu);
                      });
    }

    // 菜单挂在球的正下方（GlassMenu 内部会在贴屏幕底时自动翻到上方）。
    QPointer<GlassMenu> guard(menu);
    menu->execAt(QRect(mapToGlobal(QPoint(0, 0)), size()));
    // 走到这里说明菜单已收起：可能是选中了某项（lambda 里已排队释放），
    // 也可能是点外面 / Esc 关的（hideEvent 发 hidden → execAt 的循环退出）。
    // 后者没人删，必须在这里补一刀；disposeMenuOnce 自带去重。
    if (guard) disposeMenuOnce(guard);
}

QStringList AssistiveTouch::menuActionIds()
{
    QStringList ids;
    ids.reserve(menuEntries().size());
    for (const MenuEntry& entry : menuEntries()) ids.append(entry.id);
    return ids;
}

} // namespace kc::ui
