#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosSwipeRow —— 侧滑删除行
//
//  把任意 QWidget 包进来，向左拖拽会露出右侧的红色动作块；松手自动吸附
//  （过半就停在打开态，否则弹回）。点红色块触发 onAction。
//
//      auto* sw = new IosSwipeRow(host);
//      sw->setContent(myRowWidget);          // 接管所有权
//      sw->onAction = [] { ... };
//
//  ⚠️ 内容里如果有按钮 / 开关这类**自己吃鼠标**的控件，从它们身上按下不会触发拖拽
//     （与 iOS 一致：滑列表要按行的空白处）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QWidget>

#include <functional>

class QPropertyAnimation;

namespace kc::ui {

class IosSwipeRow : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal offset READ offset WRITE setOffset)

public:
    explicit IosSwipeRow(QWidget* parent = nullptr);

    /// 接管 content 的所有权（会成为本控件的子控件）。
    void setContent(QWidget* content);

    void setActionText(const QString& text);
    void setActionWidth(int px);
    void setActionColor(const QColor& color);

    void open();
    void close();
    bool isOpen() const { return m_offset > m_actionW * 0.5; }

    qreal offset() const { return m_offset; }
    void setOffset(qreal v);

    /// 点了红色动作块（也可直接 connect actionTriggered）。
    std::function<void()> onAction;

    QSize sizeHint() const override;

signals:
    void actionTriggered();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void animateTo(qreal target);
    QRectF actionRect() const;

    QWidget* m_content = nullptr;
    int m_actionW = 88;
    qreal m_offset = 0.0;          ///< 0 = 关闭，m_actionW = 完全打开
    QString m_text = QStringLiteral("删除");
    QColor m_color{0xFF, 0x3B, 0x30};

    QPoint m_pressPos;
    qreal m_pressOffset = 0.0;
    bool m_dragging = false;
    QPropertyAnimation* m_anim = nullptr;
};

} // namespace kc::ui
