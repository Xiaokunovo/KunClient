#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ConsolePage —— iOS 启动器式控制台主屏（2026-10-17）
//
//  外壳的**根视图**：取代原来的「左导航轨 + 顶栏页签条」两级导航。结构对齐
//  iOS 主屏：
//    · 中部 = 应用图标网格（圆角方块图标 + 下方应用名），5 列 × 3 行；
//    · 底部 = Dock（半透明圆角长条，钉 5 个最常用入口，不显示应用名）；
//    · 背景全透 —— 直接露出外壳画在窗口上的色晕 / 壁纸，图标浮在壁纸上。
//
//  交互：点图标 → 发 pageRequested(key)，外壳切到该页（页面顶栏有「‹ 控制台」
//  返回）。主屏自身不含任何业务逻辑，纯导航入口。
//
//  悬停放大（2026-10-17 第二代，对齐 macOS Dock）：光标下的图标放到最大，
//  **左右邻居按余弦钟形衰减**跟着放大 —— 这是 Dock 的招牌手感；只让光标下的
//  那一个变大是普通「按钮 hover」，不是 Dock。见 magnifyFactorFor()。
//
//  🔴 图标块是普通 QWidget 而非 QPushButton —— 项目里 QSS 的 QPushButton 规则
//     带 min-height / padding 时 QStyleSheetStyle 会接管尺寸约束，setFixedSize
//     会被**无声忽略**（2026-10-16 在快捷瓦片上踩过）。自绘控件不碰 QSS 尺寸，
//     最省心。
//
//  🔴 瓦片尺寸必须给放大留余量：Qt 会把子控件的绘制**裁剪到自己的矩形**，
//     放大后的图标方块上沿会被切平。所以图标方块不贴顶，上方留 kIconHeadroom。
// ─────────────────────────────────────────────────────────────────────────────

#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QGridLayout;
class QImage;
class QVBoxLayout;

namespace kc::ui {

class GlassSurface;
class ConsoleWidgets;

class ConsolePage : public QWidget {
    Q_OBJECT

public:
    struct Entry {
        QString key;
        QString title;
        QString group;
    };

    explicit ConsolePage(QWidget* parent = nullptr);

    /// 重建主屏：entries 进图标网格，dockKeys 是钉在 Dock 上的 key（顺序即显示顺序）。
    void setEntries(const QVector<Entry>& entries, const QStringList& dockKeys);

    /// 当前主屏图标网格里的 key（按显示顺序）。隐藏页不会出现在这里 ——
    /// 所以这是「这一页上不上主屏」唯一可靠的问法（页面本身的 isHidden()
    /// 对 QStackedWidget 里的非当前页恒为 true，拿它判断会误报）。
    QStringList entryKeys() const;

    /// 当前是否有瓦片处于放大态。外壳用它决定「能不能把主屏快照存进缓存」——
    /// 放大中的画面存下来，下次回来转场会闪一下放大的图标。
    bool isMagnifying() const { return m_magnifying; }

    /// 某个 key 的瓦片在**本页坐标系**里的矩形（含 Dock 上的）。
    /// 外壳拿它当 Mode::Zoom 转场的生长起点；找不到返回空矩形。
    QRect tileRect(const QString& key) const;

    /// Dock 本体（外壳算它在大窗里的位置，用来裁壁纸当玻璃背板）。
    QWidget* dockWidget() const { return m_dockHost; }

    /// 主屏小组件条（2026-10-29）。外壳用它裁壁纸当玻璃背板 / 刷新数据 /
    /// 控制秒级时钟。可能是空指针（设置里关掉小组件时不创建）。
    ConsoleWidgets* widgetsBar() const { return m_widgets; }

    /// 开关主屏小组件条（设置页「iOS 玩法」里那一个开关）。
    /// 🔴 必须能当场生效：设置页的文案是「改完立刻生效」，而小组件是构造时按
    ///    AppSettings 决定建不建的 —— 没有这个入口，关掉开关得重启才看得见变化。
    /// 懒建：第一次打开才 new，关掉只是 hide（反复开关不重建，避免反复裁背板）。
    void setWidgetsVisible(bool visible);

    /// 喂 Dock 的液态玻璃背板：**与 Dock 等尺寸**的「它背后那块真实画面」。
    /// 由外壳从壁纸帧裁出来（见 MainShell::updateDockBackdrop）——
    /// ConsolePage 自己拿不到壁纸，硬抓窗口会把 Dock 自己抓进去。
    /// 传空图 = 退回 GlassSurface 内置的环境光兜底。
    void setDockBackdrop(const QImage& backdrop);

    /// 重读 AppSettings 里的液态玻璃参数并套到 Dock 上（设置页保存后由外壳调用）。
    void refreshDockGlass();

signals:
    void pageRequested(const QString& key);

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void rebuildGrid();
    void rebuildDock();
    /// 让所有瓦片按当前光标位置重算放大倍率。
    void refreshTiles();

    QVector<Entry> m_entries;
    QStringList m_dockKeys;

    QWidget* m_gridHost = nullptr;
    QGridLayout* m_grid = nullptr;
    QWidget* m_dockHost = nullptr;
    /// 主屏小组件条（三块玻璃卡）。nullptr = 设置里关掉了。
    ConsoleWidgets* m_widgets = nullptr;
    /// 小组件条 + 它下面那 14px 间距的容器（一起显隐，见 setWidgetsVisible）。
    QWidget* m_widgetsHost = nullptr;
    /// Dock 的液态玻璃面板（= m_dockHost 本体）。单独存一份是为了调 setRawBackdrop。
    GlassSurface* m_dockGlass = nullptr;
    QVBoxLayout* m_dockLayout = nullptr;
    /// 所有瓦片（网格 + Dock）。存下来是为了鼠标一动就能直接遍历 update()，
    /// 不用每次 findChildren 递归整棵树（那是 qApp 级过滤器上踩过的坑）。
    QVector<QWidget*> m_tiles;
    bool m_magnifying = false;
};

} // namespace kc::ui
