#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosStepper —— iOS 步进器（左右两半 − / ＋，中间一条分隔线）
//
//      auto* st = new IosStepper(host);
//      st->setRange(1, 64);
//      st->setValue(8);
//      connect(st, &IosStepper::valueChanged, ...);
//  到边界时对应那一半自动变灰、点击无效。
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>

namespace kc::ui {

class IosStepper : public QWidget {
    Q_OBJECT

public:
    explicit IosStepper(QWidget* parent = nullptr);

    void setRange(int lo, int hi);
    void setSingleStep(int step);
    void setValue(int value);
    int value() const { return m_value; }

    QSize sizeHint() const override;

signals:
    void valueChanged(int value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QRectF halfRect(bool plus) const;
    void nudge(int delta);

    int m_value = 0;
    int m_min = 0;
    int m_max = 99;
    int m_step = 1;
    int m_pressed = 0;   ///< -1 = 左半，+1 = 右半
};

} // namespace kc::ui
