#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  GlassSurface —— 一块可以直接摆进界面的「液态玻璃」面板
//
//  LiquidGlass 是纯函数库（你给我背板、我画一块玻璃）；这个控件把它包成能用的
//  东西，负责三件脏活：
//    ① **背板缓存**：磨砂 + 折射是 10ms 级开销，绝不能逐帧算。这里只在
//       「背板变了 / 尺寸变了 / 折射参数变了」时重算一次，动画期间复用成品图。
//    ② **指针镜面高光**：鼠标在面板上移动时，光源位置跟着走 —— iOS 上玻璃的
//       高光是「被手指擦过会亮」的，这是「液态」二字的来源。移动做了平滑，
//       不能一帧跳到位（那看起来像手电筒在扫）。
//    ③ **按压果冻**：按下时横向微胀、纵向微缩，松手带过冲回弹。用的是
//       体积守恒的形变曲线，不是简单的 scale。
//
//  用法（设置页的实时预览就是这么用的）：
//      auto* g = new GlassSurface(host);
//      g->setRadius(22);
//      g->setRawBackdrop(sampleImage);      // 或 grabBackdropFrom(某个控件)
//      g->setStyle(glassStyleFromSettings());
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/widgets/LiquidGlass.h"

#include <QImage>
#include <QWidget>

class QVariantAnimation;

namespace kc::ui {

class GlassSurface : public QWidget {
    Q_OBJECT

public:
    explicit GlassSurface(QWidget* parent = nullptr);
    ~GlassSurface() override;

    void setStyle(const glass::Style& style);
    const glass::Style& style() const { return m_style; }

    /// 圆角（px）。超过短边一半时自动收成胶囊。
    void setRadius(qreal radius);
    qreal radius() const { return m_radius; }

    /// 直接喂一张与控件**等尺寸**的成品背板（已磨砂 + 折射）。
    /// 调用方自己管重算时机 —— 只在你确定背板是静态的时候用。
    void setBackdrop(const QImage& processed);

    /// 喂一张原始背板（未处理）。控件自己磨砂 + 折射，尺寸变化时按需重算。
    /// 这是最常用的入口：把「玻璃背后那块真实画面」原样丢进来就行。
    void setRawBackdrop(const QImage& raw);

    /// 从 source 控件抓一张快照当原始背板（抓的是 source 在窗口里的位置，
    /// 与本体矩形对齐）。用于「玻璃浮在某个已知内容上」的场景。
    /// 🔴 内部会 grab()，一次全窗渲染 —— 只在面板打开时调，不要放进 paintEvent。
    void grabBackdropFrom(QWidget* source);

    /// 鼠标按压是否触后果冻形变（默认开）。纯展示用的预览卡可以关掉。
    void setInteractive(bool on);

    /// 背板缺失时是否画一块「自造环境光」兜底（默认开）。
    /// 关掉的话没背板就是一块纯染色 —— 深色主题下几乎看不见。
    void setFallbackBackdrop(bool on);

signals:
    /// 鼠标左键在面板内按下并抬起（未拖出面板）。
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void rebuild();
    void animatePress(qreal target);
    void retargetLight(const QPointF& pos);

    glass::Style m_style;
    qreal m_radius = 22.0;

    QImage m_raw;         ///< 原始背板（尺寸 = 控件尺寸）
    QImage m_processed;   ///< 成品（磨砂 + 折射）
    bool m_dirty = true;  ///< 需要重算 m_processed

    bool m_interactive = true;
    bool m_fallback = true;
    bool m_pressed = false;
    QPoint m_pressPos;

    /// 光源位置的平滑目标（鼠标位置归一化）。动画期间往这里追。
    QPointF m_lightTarget{0.22, 0.10};
    QVariantAnimation* m_pressAnim = nullptr;
    QVariantAnimation* m_lightAnim = nullptr;
};

} // namespace kc::ui
