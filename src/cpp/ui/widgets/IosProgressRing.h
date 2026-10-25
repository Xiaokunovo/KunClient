#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosProgressRing —— iOS 风格环形进度
//
//  轨道 + 圆弧 + 圆头端点，中间可显示百分比。改值带动画（200ms）。
//      auto* ring = new IosProgressRing(host);
//      ring->setDiameter(56);
//      ring->setValue(0.42);      // 0..1
//      ring->setTextVisible(true);
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QWidget>

class QVariantAnimation;

namespace kc::ui {

class IosProgressRing : public QWidget {
    Q_OBJECT

public:
    explicit IosProgressRing(QWidget* parent = nullptr);

    void setValue(qreal v);          ///< 0..1，带缓动
    qreal value() const { return m_value; }

    void setDiameter(int px);
    void setLineWidth(qreal px);
    void setColor(const QColor& color);        ///< 无效 = 主题强调色
    void setTextVisible(bool on);
    /// 自定义中心文字（空 = 显示百分比）。
    void setCenterText(const QString& text);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qreal m_value = 0.0;
    qreal m_shown = 0.0;          ///< 动画中的显示值
    int m_diameter = 56;
    qreal m_lineWidth = 5.0;
    QColor m_color;
    bool m_textVisible = true;
    QString m_centerText;
    QVariantAnimation* m_anim = nullptr;
};

} // namespace kc::ui
