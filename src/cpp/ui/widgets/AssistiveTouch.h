#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AssistiveTouch —— 辅助触控悬浮球（iOS 的「小白点」）
//
//  为什么要有它：主壳把「主屏 / 控制中心 / 快捷指令 / 刷机记录 / 通知中心 /
//  待机 / 专注」这些入口摊在壁纸主屏上，用户一旦进到某个功能页里，入口就全被
//  页面盖住、只能先返回控制台。悬浮球是**任何页面都够得着**的一颗按钮，点开
//  就是同一套玻璃菜单 —— 相当于把主屏的入口随身带在身上。
//
//  实现要点：
//    · 它是**普通子控件**（挂在 MainShell 的 central 上），不是顶层窗 ——
//      跟着主窗走、不进任务栏、alt-tab 不会单独切出来、也不会盖住别的应用；
//    · 纯自绘：半透明圆底 + 1px 高光描边 + 九点图标，**不用字体字形**
//      （◐ ⟳ ⚡ 这类符号在 48px 的盒子里会渲染成小圆点/方块，见项目红线）；
//    · 拖动松手吸附最近的左/右边缘，位置以**比例**存 QSettings —— 用户改过
//      窗口尺寸或最大化之后，下次启动仍贴在原来那条边、同样的高度上；
//    · 专注 / 待机模式由外壳 setVisible(false) 收掉，本类不掺和模式判断
//      （模式是外壳的状态机，控件只负责"被显示"）。
//
//  🔴 菜单走 GlassMenu::execAt()（内部跑嵌套事件循环，等价 QMenu::exec），
//     所以菜单必须建在**堆上**并在关闭后 deleteLater —— 栈对象活不到 execAt 返回。
// ─────────────────────────────────────────────────────────────────────────────

#include <QPoint>
#include <QStringList>
#include <QWidget>

namespace kc::ui {

/// 辅助触控悬浮球（iOS AssistiveTouch）。
/// 挂在 MainShell 的 central 上（普通子控件，不是顶层窗）：
///   · 48×48 圆形，自绘「九点」图标 + 半透明深色底 + 1px 高光描边；
///   · 可拖动，松手吸附到最近的左/右边缘（带 8px 边距，与拖动时的内边距一致），
///     位置记在 QSettings("ui/assistiveTouchPos")；
///   · 单击（未拖动）弹出 GlassMenu，条目见 menuActionIds()；
///   · 专注模式 / 待机模式下由外壳 setVisible(false) 隐藏。
class AssistiveTouch : public QWidget {
    Q_OBJECT

public:
    explicit AssistiveTouch(QWidget* parent = nullptr);

    /// 外壳在 resize 时调：按记住的相对位置重新贴边（xRatio 0~1）。
    void repositionToParent();

    /// 菜单条目 id（顺序即显示顺序），外壳按这些 id 分派动作：
    ///   console / control / shortcuts / history / standby / focus
    static QStringList menuActionIds();

signals:
    /// 用户在菜单里选了某项（id 见 menuActionIds）。
    void actionTriggered(const QString& id);
    /// 浮球被拖动（外壳可用来取消待机计时等）。
    void dragged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void openMenu();
    void snapToEdge();

    QPoint m_dragOffset;
    QPoint m_pressGlobal;
    bool m_dragging = false;
    bool m_hovered = false;
    qreal m_xRatio = 1.0;   ///< 相对父宽度的水平位置比例（0=最左，1=最右）
    /// 相对父高度的垂直位置比例。规格只点名了 xRatio，但 QSettings 里存的是
    /// "x,y" 两段、repositionToParent() 也要按高度比例还原 —— y 必须有地方放。
    qreal m_yRatio = 0.62;
};

} // namespace kc::ui
