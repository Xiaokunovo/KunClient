#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  GlassDialog —— 液态玻璃弹窗基类
//
//  现有 AnimatedDialog 家族靠 QSS 的 `#msgRoot` 实色卡片 + `QDialog{background}`
//  撑外观；那个体系动一处会影响十几个弹窗（含启动登录框），不适合整体翻新。
//
//  这个基类是**独立的一条路**：frameless + 透明窗口 + 自绘玻璃底，内容用自建
//  布局（标题 / 正文 / 按钮行），完全不依赖 `#msg*` 皮肤，因此：
//    · 迁移一个弹窗 = 改基类 + 把内容塞进 bodyLayout()，不会波及其它弹窗；
//    · 观感与菜单 / Toast / 抽屉同族（同一套 glass::paint 与背板来源）。
//
//  用法：
//      auto* dlg = new GlassDialog(parent);
//      dlg->setTitle("擦除分区");
//      dlg->bodyLayout()->addWidget(new QLabel("...", dlg));
//      dlg->addButton("取消", false, [dlg]{ dlg->reject(); });
//      dlg->addButton("擦除", true,  [dlg]{ ...; dlg->accept(); });
//      dlg->exec();
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/dialogs/AnimatedDialog.h"
#include "ui/widgets/LiquidGlass.h"

#include <QImage>
#include <QString>

#include <functional>
#include <utility>

class QLabel;
class QHBoxLayout;
class QPushButton;
class QVBoxLayout;

namespace kc::ui {

class GlassDialog : public AnimatedDialog {
    Q_OBJECT

public:
    explicit GlassDialog(QWidget* parent = nullptr);
    ~GlassDialog() override;

    void setTitle(const QString& text);
    /// 正文容器布局（往这里塞任意控件）。
    QVBoxLayout* bodyLayout() const { return m_bodyLayout; }

    /// 按钮种类：Ghost（描边）/ Primary（强调色实心）/ Danger（危险红实心）。
    enum class ButtonKind { Ghost, Primary, Danger };

    /// 追加一个底部按钮。
    QPushButton* addButton(const QString& text, ButtonKind kind,
                           std::function<void()> onClick);
    /// 兼容重载：primary=true 等价 Primary，false 等价 Ghost。
    QPushButton* addButton(const QString& text, bool primary,
                           std::function<void()> onClick)
    {
        return addButton(text, primary ? ButtonKind::Primary : ButtonKind::Ghost,
                         std::move(onClick));
    }
    /// 按钮行前插入弹性空白（默认已加）。
    void setRadius(qreal radius);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void refreshBackdrop();

    QLabel* m_title = nullptr;
    QWidget* m_body = nullptr;
    QVBoxLayout* m_bodyLayout = nullptr;
    QHBoxLayout* m_buttonRow = nullptr;

    QImage m_backdrop;
    glass::Style m_style;
    qreal m_radius = 22.0;
};

} // namespace kc::ui
