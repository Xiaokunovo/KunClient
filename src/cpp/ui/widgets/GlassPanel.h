#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  GlassPanel —— 「浮在界面之上」的液态玻璃容器基类
//
//  Dock / 顶栏胶囊 / 灵动岛都是**页面里的控件**，背板由 MainShell 主动喂。
//  菜单 / Toast / 底部抽屉 / 玻璃弹窗是**顶层窗口**（frameless），没有父布局
//  帮它们算背板，于是统一走这个基类：
//
//    ① 自己向 MainShell 要「我这块矩形背后的壁纸」（backdropForWidget），
//       不 grab 窗口 —— 否则快照里含玻璃自己，磨砂出来是一块灰糊；
//    ② 缓存 process() 的成品（磨砂 + 折射），只在尺寸/参数变时重算；
//    ③ 提供一块**透明内容层** content()，子控件往上摆，玻璃从底下透出来。
//
//  🔴 只用于**顶层**控件（Qt::FramelessWindowHint 的窗口）。页面内控件请继续
//     用 GlassSurface（它由布局摆几何，背板由外壳喂）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/widgets/LiquidGlass.h"

#include <QImage>
#include <QWidget>

class QVBoxLayout;

namespace kc::ui {

class GlassPanel : public QWidget {
    Q_OBJECT

public:
    explicit GlassPanel(QWidget* parent = nullptr);
    ~GlassPanel() override;

    /// 子控件往这上面摆（本身就是透明的，玻璃从底下透出来）。
    QWidget* content() const { return m_content; }
    QVBoxLayout* contentLayout() const { return m_contentLayout; }

    void setRadius(qreal radius);
    qreal radius() const { return m_radius; }

    void setStyle(const glass::Style& style);
    const glass::Style& style() const { return m_style; }

    /// 向 MainShell 重新取背板（show / resize / 位置变化后调用）。
    void refreshBackdrop();
    /// 直接喂**原始**背板（未 process，尺寸须与控件一致）。
    void setRawBackdrop(const QImage& raw);
    /// 直接喂**成品**背板（已 process）。
    void setBackdrop(const QImage& processed);

    /// 顶沿高光开关（默认开）。
    void setHighlight(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void rebuild();

    QWidget* m_content = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;

    QImage m_raw;         ///< 原始背板（壁纸切片）
    QImage m_processed;   ///< 成品（磨砂 + 折射）
    bool m_dirty = true;

    glass::Style m_style;
    qreal m_radius = 20.0;
};

} // namespace kc::ui
