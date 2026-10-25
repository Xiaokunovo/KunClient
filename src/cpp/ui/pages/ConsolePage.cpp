#include "ui/pages/ConsolePage.h"

#include "ui/theme/GlassSettings.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/AppIcons.h"
#include "ui/widgets/GlassSurface.h"

#include <QCursor>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace kc::ui {

namespace {

// ── 主屏尺寸 ────────────────────────────────────────────────────────────────
constexpr int kColumns = 5;          // 5 列；2026-10-19 起 16 个入口 = 5 × 3 + 1（尾行居中）
constexpr int kTileW = 96;           // 图标瓦片（含下方应用名）
constexpr int kIconBox = 62;         // 图标方块边长（iOS 圆角方块）
constexpr int kDockBox = 50;         // Dock 里的小号图标
constexpr int kDockTile = 70;        // Dock 瓦片边长（比图标大一圈，给放大留余量）
constexpr int kDockPad = 8;

// 悬停放大：光标正下方最大 1.38×，影响半径约 1.6 个图标间距。
// 这两个数是「像不像 Dock」的关键 —— 半径太小只有自己变大（普通 hover），
// 太大整个网格一起呼吸（晕）。165px 落在 104px 的列间距上，恰好让左右邻居
// 放大到约 1.2×、隔一个基本回到 1.0×，就是 Dock 那个钟形。
constexpr qreal kMaxMagnify = 1.38;
constexpr qreal kMagnifyRadius = 165.0;

// 🔴 顶部留白 = 放大后超出方块的那一整段。Qt 把子控件绘制裁剪在自己的矩形里，
//    没有这段余量，放大时图标方块的上沿会被切平。
//    放大以方块**下沿**为锚点（Dock 就是从底座往上长），所以超出的部分全在顶部：
//    ceil((1.38-1)*62) = 24，一个像素都不能省。
constexpr int kIconHeadroom = 24;
constexpr int kLabelH = 20;
constexpr int kTileH = kIconHeadroom + kIconBox + 6 + kLabelH;   // 112

/// 探针：KC_CONSOLE_CURSOR="x,y"（页面坐标）时用固定光标位置代替真实光标，
/// 好让无头截图能取证「悬停放大」。与 KC_TRANSITION_FREEZE 同款永久保留设计。
bool probeCursor(QPoint* out)
{
    static const QByteArray raw = qgetenv("KC_CONSOLE_CURSOR");
    if (raw.isEmpty()) return false;
    const QList<QByteArray> parts = raw.split(',');
    if (parts.size() != 2) return false;
    bool okX = false;
    bool okY = false;
    const int x = parts.at(0).trimmed().toInt(&okX);
    const int y = parts.at(1).trimmed().toInt(&okY);
    if (!okX || !okY) return false;
    *out = QPoint(x, y);
    return true;
}

/// macOS Dock 的放大曲线：余弦半周期，中心 1、影响半径处 0。
/// 用 cos 而不是线性，是为了**边缘导数为零** —— 线性衰减在半径边界会「啪」地
/// 断掉，光标慢慢移开时能看见图标突然一跳。
qreal magnifyFactorFor(qreal distance)
{
    if (distance >= kMagnifyRadius) return 1.0;
    return 1.0 + (kMaxMagnify - 1.0) * std::cos(distance / kMagnifyRadius * M_PI / 2.0);
}

/// 主屏图标**不再按 key 上色**（2026-10-18 用户口径「抛弃现在的图标颜色」）。
/// 旧版一 app 一色（iOS 系统色板），问题有两个：
///   · 15 个高饱和色块挤在壁纸上，主屏看着像一张贴纸墙，用户自己挑的壁纸全被盖住；
///   · 深色主题下深色瓦片（终端 / 设置）会糊进背景，看起来像少了两个图标。
/// 现在统一成**半透明玻璃方块**：材质跟卡片一致，颜色交给壁纸，图标用前景色画。
/// 识别度靠字形（appIconForKey）而不是靠色相 —— 这也是 iOS 26 之后的取向。

// ─────────────────────────────────────────────────────────────────────────────
//  AppTile —— 单个应用图标（自绘 QWidget，非 QPushButton）
//
//  🔴 不用 QPushButton：QSS 里 QPushButton 的基础规则带 min-height / padding，
//     QStyleSheetStyle 会接管尺寸约束，setFixedSize 会被无声忽略（2026-10-16
//     在主页快捷瓦片上实测踩过：设了 58px 仍渲染 20px）。自绘控件不碰 QSS 尺寸。
//  图标方块 = 上下渐变填充 + 顶部玻璃高光 + 1px 浅描边，就是 iOS 图标那层质感。
//
//  放大倍率**每帧按光标位置现算**，不缓存：光标一直在动，缓存只会带来失步。
//  重绘由 ConsolePage::refreshTiles() 统一驱动（瓦片自己发不了「别的瓦片也该重画」）。
// ─────────────────────────────────────────────────────────────────────────────
class AppTile : public QWidget {
public:
    AppTile(ConsolePage* page, const QString& key, const QString& title, bool dock)
        : QWidget(page), m_page(page), m_key(key), m_title(title), m_dock(dock) {
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);   // 悬停放大要跟着光标走，不能等按下
        setToolTip(title);
        setFixedSize(dock ? kDockTile : kTileW, dock ? kDockTile : kTileH);
    }

    /// 点按回调（不用信号：本类定义在 .cpp 内，避免 AUTOMOC 的 .moc 包含仪式）。
    std::function<void()> onActivated;
    /// 光标在本瓦片上移动 → 请宿主重算全部瓦片的放大倍率。
    std::function<void()> onPointerMoved;

    /// 本瓦片对应的页面 key（ConsolePage::tileRect 按它反查位置）。
    const QString& key() const { return m_key; }

    /// 宿主推过来的「本页坐标系下的光标位置」。
    void setPageCursor(const QPoint& p)
    {
        if (m_pageCursor == p && m_pageCursorValid) return;
        m_pageCursor = p;
        m_pageCursorValid = true;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const int box = m_dock ? kDockBox : kIconBox;
        const int bx = (width() - box) / 2;
        const int by = m_dock ? (height() - box) / 2 : kIconHeadroom;
        const QRectF boxRect(bx, by, box, box);
        const qreal radius = box * 0.235;   // iOS squircle 近似（连续圆角）

        // ── macOS Dock 式悬停放大 ──────────────────────────────────────
        // 倍率取决于「光标离这个图标方块中心多远」，不是「鼠标在不在我身上」——
        // 所以邻居会跟着一起放大，这才是 Dock 的手感。
        const QPointF cursorPage = m_pageCursorValid ? QPointF(m_pageCursor)
                                                     : QPointF(mapFromGlobal(QCursor::pos()));
        const QPointF cursorLocal = QPointF(mapFrom(m_page, cursorPage.toPoint()));
        const qreal distance = std::hypot(cursorLocal.x() - boxRect.center().x(),
                                          cursorLocal.y() - boxRect.center().y());
        // 按下时缩到 0.92（按压反馈）—— 覆盖放大倍率，不然按住放大中的图标没反应。
        const qreal scale = m_pressed ? 0.92 : magnifyFactorFor(distance);

        p.save();
        if (!qFuzzyCompare(scale, 1.0)) {
            // 网格瓦片以方块**下沿**为锚点放大（Dock 是从底座往上长的，图标不会
            // 往下压到应用名上）；Dock 瓦片以中心为锚点，它四周本来就留了一圈余量。
            const QPointF anchor = m_dock ? boxRect.center()
                                          : QPointF(boxRect.center().x(), boxRect.bottom());
            p.translate(anchor);
            p.scale(scale, scale);
            p.translate(-anchor);
        }

        const bool dark = ThemeManager::instance().isDark();

        // ── 玻璃方块 ──────────────────────────────────────────────────────
        // 竖向渐变 + 顶边高光 + 1px 描边，与卡片同一套材质：
        //   浅色主题 = 白玻璃（顶 0.77 → 底 0.43），字形用深墨；
        //   深色主题 = 淡白玻璃（顶 0.17 → 底 0.05），字形用近白。
        // 🔴 深色主题**不能**用深灰填充：壁纸本身可能是深色照片，深灰方块会
        //    整块消失，只剩一圈描边。
        QLinearGradient fill(boxRect.topLeft(), boxRect.bottomLeft());
        if (dark) {
            fill.setColorAt(0.0, QColor(255, 255, 255, 46));
            fill.setColorAt(0.55, QColor(255, 255, 255, 24));
            fill.setColorAt(1.0, QColor(255, 255, 255, 13));
        } else {
            fill.setColorAt(0.0, QColor(255, 255, 255, 200));
            fill.setColorAt(0.55, QColor(255, 255, 255, 156));
            fill.setColorAt(1.0, QColor(255, 255, 255, 116));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(boxRect, radius, radius);

        // 顶部玻璃高光：上 55% 一层白到透明，图标立刻有「玻璃片」的厚度。
        QPainterPath clip;
        clip.addRoundedRect(boxRect, radius, radius);
        p.setClipPath(clip);
        QLinearGradient gloss(boxRect.topLeft(),
                              QPointF(boxRect.left(), boxRect.top() + box * 0.55));
        gloss.setColorAt(0.0, QColor(255, 255, 255, dark ? 54 : 128));
        gloss.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.fillRect(boxRect, gloss);
        p.setClipping(false);

        // 描边：浅色主题用极淡墨（白玻璃在白壁纸上会没有边），深色主题用淡白。
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(dark ? QColor(255, 255, 255, 54) : QColor(15, 23, 42, 30), 1.0));
        p.drawRoundedRect(boxRect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

        // 字形占方块的比例。0.64 是照着 iOS 主屏量的：图标库字形自带约 12% 内边距，
        // 再乘 0.64 之后实际落笔约 55% —— 与 iOS 图标的视觉重量接近。
        const qreal gs = box * 0.64;
        drawAppIcon(p, QRectF(boxRect.center().x() - gs / 2,
                              boxRect.center().y() - gs / 2, gs, gs),
                    appIconForKey(m_key),
                    dark ? QColor(255, 255, 255, 232) : QColor(0x1B, 0x22, 0x2C, 236));
        p.restore();

        if (m_dock) return;

        // 应用名：主题前景色 + 反向光晕。壁纸是用户自己选的，纯色字在照片上
        // 会读不清，加一层同族描边式光晕是最省的解法（iOS 也是这么干的）。
        // 应用名**不参与放大** —— 字号跟着变会糊，Dock 本身也不放大标签。
        QFont f = font();
        f.setPixelSize(12);
        p.setFont(f);
        const QRectF lr(0, kIconHeadroom + kIconBox + 6, width(), kLabelH);
        const QString label = QFontMetrics(f).elidedText(m_title, Qt::ElideRight,
                                                         qMax(12, int(lr.width())));
        const QColor halo = dark ? QColor(0, 0, 0, 150) : QColor(255, 255, 255, 190);
        p.setPen(halo);
        p.drawText(lr.translated(0, 1), Qt::AlignHCenter | Qt::AlignTop, label);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(lr, Qt::AlignHCenter | Qt::AlignTop, label);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            m_pressed = true;
            update();
        }
        QWidget::mousePressEvent(e);
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        const bool inside = rect().contains(e->pos());
        if (m_pressed) {
            m_pressed = false;
            update();
        }
        if (e->button() == Qt::LeftButton && inside && onActivated) onActivated();
        QWidget::mouseReleaseEvent(e);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (onPointerMoved) onPointerMoved();
        e->accept();   // 别再往上传：宿主那一次 refreshTiles 已经把全部瓦片覆盖了
    }

    void leaveEvent(QEvent* e) override {
        m_pressed = false;
        if (onPointerMoved) onPointerMoved();   // 光标离开 → 重算（含把邻居收回 1.0）
        QWidget::leaveEvent(e);
    }

private:
    ConsolePage* m_page = nullptr;
    QString m_key;
    QString m_title;
    bool m_dock = false;
    bool m_pressed = false;
    QPoint m_pageCursor;
    bool m_pageCursorValid = false;
};

} // namespace

ConsolePage::ConsolePage(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("consolePage"));
    // 光标在图标之间的空隙里移动时也要重算 —— 只靠瓦片的 move 事件会漏。
    setMouseTracking(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 4, 24, 14);
    root->setSpacing(0);

    m_gridHost = new QWidget(this);
    m_gridHost->setObjectName(QStringLiteral("consoleGrid"));
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(0, 0, 0, 0);
    m_grid->setHorizontalSpacing(8);
    m_grid->setVerticalSpacing(12);

    // Dock：**真液态玻璃**（iOS 26 里 Dock 就是这套材质的教科书案例）。
    //
    // 之前这里是 QFrame + QSS 的 rgba 半透明 —— 那只是「一层带颜色的膜」，
    // 透过它看到的壁纸是清晰的，所以永远不像玻璃。现在换成 GlassSurface：
    // 背板由外壳从壁纸帧裁出来喂进来（见 MainShell::updateDockBackdrop），
    // 走真磨砂 + 边缘折射 + 色散，圆角 28 与旧 QSS 完全一致。
    //
    // 🔴 objectName 保持 consoleDock：QSS 里 `QFrame#consoleDock` 那条规则
    //    因为类型不匹配自动失效（不会再叠一层半透明底），但抓图探针 / 调试
    //    仍然按这个名字找得到它。
    auto* dock = new GlassSurface(this);
    dock->setObjectName(QStringLiteral("consoleDock"));
    dock->setRadius(28);
    dock->setInteractive(false);   // Dock 不响应按压果冻（图标自己才是可点的）
    dock->setFallbackBackdrop(true);
    dock->setStyle(currentGlassStyle());
    m_dockGlass = dock;
    m_dockLayout = new QVBoxLayout(dock);
    m_dockLayout->setContentsMargins(kDockPad + 2, kDockPad, kDockPad + 2, kDockPad);
    m_dockLayout->setSpacing(0);
    m_dockHost = dock;

    root->addStretch(1);
    root->addWidget(m_gridHost, 0, Qt::AlignHCenter);
    root->addStretch(1);
    root->addWidget(m_dockHost, 0, Qt::AlignHCenter);
}

void ConsolePage::setEntries(const QVector<Entry>& entries, const QStringList& dockKeys)
{
    m_entries = entries;
    m_dockKeys = dockKeys;
    rebuildGrid();
    rebuildDock();
    refreshTiles();   // 探针模式下没有鼠标事件，这里补一次初始倍率
}

void ConsolePage::mouseMoveEvent(QMouseEvent* e)
{
    refreshTiles();
    QWidget::mouseMoveEvent(e);
}

void ConsolePage::leaveEvent(QEvent* e)
{
    refreshTiles();   // 光标离开页面 → 各瓦片按「离得很远」自己回到 1.0
    QWidget::leaveEvent(e);
}

QRect ConsolePage::tileRect(const QString& key) const
{
    if (key.isEmpty()) return {};
    for (QWidget* w : m_tiles) {
        // m_tiles 里只存 AppTile（rebuildGrid / rebuildDock 建的），静态转型安全。
        // 🔴 不能用 qobject_cast：AppTile 没有 Q_OBJECT（本类刻意不用信号，
        //    定义在 .cpp 里省掉 .moc 包含），qobject_cast 编不过。
        auto* tile = static_cast<AppTile*>(w);
        if (!tile || tile->key() != key) continue;
        if (!tile->isVisible()) return {};
        // 瓦片的父级是 m_gridHost / m_dockHost，中间隔了一层 —— 必须 mapTo 到本页，
        // 不能直接用 geometry()（那是在宿主坐标系里的）。
        return QRect(tile->mapTo(this, QPoint(0, 0)), tile->size());
    }
    return {};
}

void ConsolePage::refreshTiles()
{
    QPoint probe;
    const bool hasProbe = probeCursor(&probe);
    const QPoint cursor = hasProbe ? probe : mapFromGlobal(QCursor::pos());

    bool anyMagnified = false;
    for (QWidget* w : m_tiles) {
        auto* tile = static_cast<AppTile*>(w);
        tile->setPageCursor(cursor);
        // 只在光标确实落在某个瓦片上时才算「放大中」：外壳据此决定主屏快照
        // 能不能进缓存（存下放大态的画面，下次回来转场会闪一下）。
        if (!hasProbe && tile->underMouse()) anyMagnified = true;
        tile->update();
    }
    m_magnifying = anyMagnified;
}

void ConsolePage::rebuildGrid()
{
    if (!m_grid) return;
    m_tiles.clear();
    while (QLayoutItem* item = m_grid->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    // 瓦片工厂：整行 / 尾行两条路径共用，避免复制那段信号接线。
    const auto makeTile = [this](const Entry& entry) {
        auto* tile = new AppTile(this, entry.key, entry.title, /*dock=*/false);
        const QString key = entry.key;
        tile->onActivated = [this, key] { emit pageRequested(key); };
        tile->onPointerMoved = [this] { refreshTiles(); };
        m_tiles.append(tile);
        return tile;
    };

    // 整行按 5 列铺开。
    const int fullRows = m_entries.size() / kColumns;
    int index = 0;
    for (int row = 0; row < fullRows; ++row) {
        for (int col = 0; col < kColumns; ++col) {
            m_grid->addWidget(makeTile(m_entries.at(index++)), row, col);
        }
    }

    // 尾行不满：整行包进一个横向容器并**居中**。
    // 🔴 不能像以前那样「补几个空 QWidget 占列」：空控件没有内容，列宽仍由
    //    上面几行决定，尾行那唯一一个瓦片就贴在网格最左边 —— 2026-10-19 加了
    //    第 16 个入口（小米字库牌子）之后，「关于」单独一行偏在左侧，看着像
    //    布局漏了四格。居中后同样的形态读作「最后一排多一个」。
    const int tail = m_entries.size() % kColumns;
    if (tail > 0) {
        auto* holder = new QWidget(m_gridHost);
        auto* rowLayout = new QHBoxLayout(holder);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(8);   // 与 m_grid 的 horizontalSpacing 一致
        rowLayout->addStretch(1);
        for (int i = 0; i < tail; ++i) {
            rowLayout->addWidget(makeTile(m_entries.at(index++)));
        }
        rowLayout->addStretch(1);
        m_grid->addWidget(holder, fullRows, 0, 1, kColumns);
    }
}

void ConsolePage::rebuildDock()
{
    if (!m_dockLayout) return;
    while (QLayoutItem* item = m_dockLayout->takeAt(0)) {
        if (QWidget* w = item->widget()) w->deleteLater();
        delete item;
    }

    auto* row = new QWidget(m_dockHost);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    // 间距 0：瓦片本身已比图标大一圈（kDockTile vs kDockBox），那圈余量就是
    // 放大用的空间；再加间距会让放大后的图标之间出现空洞。
    layout->setSpacing(0);

    for (const Entry& e : m_entries) {
        if (!m_dockKeys.contains(e.key)) continue;
        auto* tile = new AppTile(this, e.key, e.title, /*dock=*/true);
        const QString key = e.key;
        tile->onActivated = [this, key] { emit pageRequested(key); };
        tile->onPointerMoved = [this] { refreshTiles(); };
        m_tiles.append(tile);
        layout->addWidget(tile);
    }
    m_dockLayout->addWidget(row);
}

void ConsolePage::setDockBackdrop(const QImage& backdrop)
{
    if (!m_dockGlass) return;
    m_dockGlass->setRawBackdrop(backdrop);
}

void ConsolePage::refreshDockGlass()
{
    if (!m_dockGlass) return;
    m_dockGlass->setStyle(currentGlassStyle());
}

} // namespace kc::ui
