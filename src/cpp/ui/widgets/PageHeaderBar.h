#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PageHeaderBar — 统一的页面标题栏
//
//  移植自 tests/src/KunClient.App/Controls/PageHeaderBar.{xaml,xaml.cs}。
//  布局：标题（16pt SemiBold）+ 副标题（11pt 三级色）+ 右侧动作区；
//  底部发丝线把标题区与内容分开。品牌图标方块（原 32x32 圆角 logo）已按需求移除。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;

namespace kc::ui {

class PageHeaderBar : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString title READ title WRITE setTitle)
    Q_PROPERTY(QString subtitle READ subtitle WRITE setSubtitle)

public:
    explicit PageHeaderBar(QWidget* parent = nullptr);
    explicit PageHeaderBar(const QString& title, const QString& subtitle,
                           QWidget* parent = nullptr);

    QString title() const { return m_title; }
    void setTitle(const QString& title);

    QString subtitle() const { return m_subtitle; }
    void setSubtitle(const QString& subtitle);

    /// 右侧动作区容器（可为空）。把按钮等挂到这里。
    QWidget* actionsHost() const { return m_actionsHost; }

    /// 便捷：把一个动作控件追加到右侧动作区。
    void addAction(QWidget* action);

protected:
    void changeEvent(QEvent* event) override;

private:
    void buildUi();
    void applyTheme();

    QString m_title;
    QString m_subtitle;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_subtitleLabel = nullptr;
    QWidget* m_actionsHost = nullptr;
    QHBoxLayout* m_actionsLayout = nullptr;
    // 重入守卫：applyTheme() 的 setStyleSheet() 会同步投递 StyleChange →
    // changeEvent → applyTheme，构成无限递归。见 .cpp 注释。
    bool m_applyingTheme = false;
};

} // namespace kc::ui
