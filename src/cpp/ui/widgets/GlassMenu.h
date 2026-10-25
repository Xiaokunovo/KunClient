#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  GlassMenu —— iOS 26 风格的悬浮毛玻璃菜单
//
//  替换原生 QMenu（方角、不透明、不跟壁纸）。观感要点：
//    · 玻璃底：向 MainShell 要背后那块壁纸，磨砂 + 边缘折射 + 顶沿高光；
//    · 行：图标 + 文字 +（可选）对勾，hover 用强调色淡染，不位移不放大；
//    · 入场：淡入 + 上浮 8px（iOS 弹层同族曲线），退场反过来；
//    · 关闭：点菜单外任意处 / Esc / 选中某项。
//
//  用法（同步，最接近 QMenu::exec）：
//      GlassMenu menu(parent);
//      menu.addItem(icon, "读取分区", [=]{ readPartition(name); });
//      menu.addSeparator();
//      menu.addItem(QIcon(), "擦除分区", [=]{ erasePartition(name); });
//      menu.execAt(table->mapToGlobal(pos));
//
//  🔴 必须**栈上**用 execAt（内部跑嵌套事件循环），或堆上 popupAt + 自己管生命周期。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/widgets/GlassPanel.h"

#include <QIcon>
#include <QRect>
#include <QVector>

class QKeyEvent;
class QHideEvent;

namespace kc::ui {

class GlassMenuRow;

class GlassMenu : public GlassPanel {
    Q_OBJECT

public:
    explicit GlassMenu(QWidget* parent = nullptr);
    ~GlassMenu() override;

    /// 追加一项。checked=true 时右侧画对勾（单选/多选态）。
    void addItem(const QIcon& icon, const QString& text,
                 std::function<void()> onTrigger, bool checked = false);
    /// 追加一条分隔线。
    void addSeparator();

    /// 在 anchorGlobal（触发控件的**全局**矩形）下方弹出；下方放不下自动翻到上方。
    void popupAt(const QRect& anchorGlobal);
    /// 阻塞式弹出：关闭后返回（等价 QMenu::exec）。
    void execAt(const QRect& anchorGlobal);

    /// 收起（默认带淡出动画）。
    void dismiss(bool animate = true);

signals:
    void hidden();

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    int computeWidth() const;
    int computeHeight() const;

    QVector<GlassMenuRow*> m_rows;
    QVector<QWidget*> m_all;
    bool m_openUp = false;
    bool m_dismissing = false;
};

} // namespace kc::ui
