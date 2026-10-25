#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosPageControl —— iOS 页码点
//
//  一排圆点，当前那个拉长成胶囊并滑动过去。
//      auto* dots = new IosPageControl(host);
//      dots->setCount(5);
//      dots->setCurrentIndex(2);
//      connect(dots, &IosPageControl::currentIndexChanged, ...);  // 点圆点也能切
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>

class QVariantAnimation;

namespace kc::ui {

class IosPageControl : public QWidget {
    Q_OBJECT

public:
    explicit IosPageControl(QWidget* parent = nullptr);

    void setCount(int count);
    int count() const { return m_count; }

    void setCurrentIndex(int index);
    int currentIndex() const { return m_index; }

    QSize sizeHint() const override;

signals:
    void currentIndexChanged(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void animateIndicator(qreal target);

    int m_count = 0;
    int m_index = 0;
    qreal m_indicator = 0.0;
    QVariantAnimation* m_anim = nullptr;
};

} // namespace kc::ui
