#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  SegmentedControl —— iOS 分段选择器
//
//  一条胶囊轨道 + 一枚会滑动的「药丸」指示器；选中段文字加重，其余淡化。
//  纯自绘（QSS 做不出滑动指示器）。
//
//  为了能**直接替换 QComboBox**，这里实现了最常用的子集：
//      addItem(text, data) / findData(data) / setCurrentIndex(i) /
//      currentIndex() / currentData() / currentIndexChanged(int)
//  所以调用方把类型换掉即可，逻辑一行不用改。
// ─────────────────────────────────────────────────────────────────────────────

#include <QVariant>
#include <QVector>
#include <QWidget>

class QVariantAnimation;

namespace kc::ui {

class SegmentedControl : public QWidget {
    Q_OBJECT

public:
    explicit SegmentedControl(QWidget* parent = nullptr);

    void addItem(const QString& text, const QVariant& data = QVariant());
    int count() const { return m_items.size(); }

    int currentIndex() const { return m_index; }
    void setCurrentIndex(int index);

    QVariant currentData() const;
    void setCurrentData(const QVariant& data);
    int findData(const QVariant& data) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void currentIndexChanged(int index);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Item {
        QString text;
        QVariant data;
    };

    QRectF innerRect() const;
    qreal segmentWidth() const;
    int segmentAt(const QPoint& pos) const;
    void animateIndicator(qreal target);

    QVector<Item> m_items;
    int m_index = 0;
    qreal m_indicator = 0.0;    ///< 指示器位置（以「段」为单位，可小数）
    QVariantAnimation* m_anim = nullptr;
    int m_hover = -1;
};

} // namespace kc::ui
