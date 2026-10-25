#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosBadge —— iOS 角标（红底白字，超过 99 显示 99+）
//
//      auto* badge = new IosBadge(tile);      // 父控件即挂载点
//      badge->setCount(3);
//      badge->move(tile->width() - badge->width() - 6, 6);
//  数量为 0 时自动隐藏。
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QWidget>

namespace kc::ui {

class IosBadge : public QWidget {
    Q_OBJECT

public:
    explicit IosBadge(QWidget* parent = nullptr);

    void setCount(int count);
    int count() const { return m_count; }

    void setBadgeColor(const QColor& color);   ///< 默认 iOS 红 #FF3B30

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString label() const;

    int m_count = 0;
    QColor m_color{0xFF, 0x3B, 0x30};
};

} // namespace kc::ui
