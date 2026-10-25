#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosSwitch —— iOS 风格开关
//
//  QCheckBox 换成这个：绿色轨道 + 白色滑块，切换时滑块带过冲（OutBack）滑过去，
//  轨道颜色同步淡变。纯自绘，不依赖 QSS（QSS 的 indicator 只能做静态两态）。
//
//  用法与 QCheckBox 几乎一致：
//      auto* sw = new IosSwitch(host);
//      sw->setChecked(true);
//      connect(sw, &IosSwitch::toggled, this, ...);   // 继承自 QAbstractButton
//  🔴 它不显示文字：需要文案时旁边单独摆一个 QLabel。
// ─────────────────────────────────────────────────────────────────────────────

#include <QAbstractButton>
#include <QColor>

class QVariantAnimation;

namespace kc::ui {

class IosSwitch : public QAbstractButton {
    Q_OBJECT

public:
    explicit IosSwitch(QWidget* parent = nullptr);

    QSize sizeHint() const override;

    /// 打开态轨道颜色（默认 iOS 绿 #30D158）。
    void setOnColor(const QColor& color);

    /// 设置状态但**不播**滑动动画（读配置时用，避免每次进设置页都滑一遍）。
    void setCheckedSilently(bool on);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void animateTo(qreal target);

    qreal m_pos = 0.0;          ///< 0 = 关，1 = 开（动画值，可能轻微过冲）
    QColor m_onColor{0x30, 0xD1, 0x58};
    QVariantAnimation* m_anim = nullptr;
    bool m_hover = false;
};

} // namespace kc::ui
