#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosSpinner —— iOS 活动指示器（12 根辐条渐隐旋转）
//
//  纯自绘 + 16ms 定时器，不用 GIF / 不用 QProgressBar(indeterminate)。
//  用法：
//      auto* sp = new IosSpinner(host);
//      sp->start();            // 开始转
//      sp->stop();             // 停（停在当前角度）
//  尺寸默认 20px，可 setDiameter()。
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QWidget>

class QTimer;

namespace kc::ui {

class IosSpinner : public QWidget {
    Q_OBJECT

public:
    explicit IosSpinner(QWidget* parent = nullptr);

    void setDiameter(int px);
    void setLineWidth(qreal px);
    void setColor(const QColor& color);   ///< 不设则跟随主题前景色

    void start();
    void stop();
    bool isRunning() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void tick();

    int m_diameter = 20;
    qreal m_lineWidth = 2.0;
    QColor m_color;              ///< 无效 = 用主题色
    qreal m_angle = 0.0;         ///< 0..360
    QTimer* m_timer = nullptr;
};

} // namespace kc::ui
