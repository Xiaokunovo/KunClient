#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  LiquidGlass —— iOS 26「液态玻璃」的 Qt 光栅实现
//
//  真机上的 Liquid Glass 不是「模糊 + 半透明」这么简单，它是六层叠加：
//    ① **背板折射（lens）**  —— 最灵魂的一层。玻璃边缘像一块厚透镜，把背后的
//       画面**向内挤压放大**，于是贴着边缘的内容会明显「被吸进去」。少了它，
//       无论模糊做得多细都只是一块磨砂塑料。
//    ② **色散（dispersion）** —— 折射时 R/G/B 三通道的偏折角略有差异，边缘
//       因此浮出一圈极淡的彩虹边。真机上很弱，但正是它让玻璃「有材质」。
//    ③ **高斯磨砂（frost）** —— 降采样再升采样，纯 CPU 毫秒级。
//    ④ **染色（tint）**     —— 决定这块玻璃「有多实」，保证文字对比度。
//    ⑤ **镜面高光（specular）** —— 一条跟随**光源位置**的亮带。iOS 上光源
//       默认在左上，指针移动时高光会跟着走 —— 这是「液态」的来源。
//    ⑥ **弹性形变（press）** —— 按下时横向微胀、纵向微缩，松手回弹。
//
//  🔴 磨砂与折射**必须**基于真实背板，不能拿纯色凑。没有背板时退化成
//     「染色 + 高光」，仍然像玻璃，但没有折射感 —— 调用方应尽量把背板抓进来。
//
//  性能口径：`process()`（折射 + 磨砂）只在**背板或尺寸变化时**调一次，
//  动画期间复用同一张成品图；逐帧重算是 10ms+ 级开销，会把动画拖成幻灯片。
//  高光与弹性形变是纯矢量绘制，逐帧零成本。
// ─────────────────────────────────────────────────────────────────────────────

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QRectF>

class QPainter;

namespace kc::ui::glass {

/// 一块玻璃的外观参数。默认值 = iOS 26 的「标准」档观感。
struct Style {
    /// 染色浓度：0 = 几乎全透（纯磨砂），1 = 完全不透（实心卡片）。
    qreal tint = 0.58;
    /// 磨砂半径（像素）。越大越糊；0 = 直接用原图。
    qreal frost = 22.0;
    /// 高光 / 收边强度倍率。1 = 标准，0 = 关掉（纯色平面）。
    qreal highlight = 1.0;
    /// 边缘折射强度 0~1。0 = 关（纯磨砂玻璃），1 = 强透镜（边缘内容明显被吸进去）。
    qreal refraction = 0.55;
    /// 色散强度 0~1。0 = 无色散；0.35 = 隐约彩虹边（默认）；>0.7 会明显发花。
    qreal dispersion = 0.35;
    /// 折射带的宽度（占短边的比例）。0.06~0.30 是合理区间。
    /// 默认 0.20：低于 0.14 时带子比磨砂半径还窄，边缘形变会被磨砂吃掉，
    /// 用户会得出「调了没反应」的结论（实测 0.14 就是这个问题）。
    qreal edgeBand = 0.20;
    /// 镜面高光的光源位置，归一化到 [0,1]²（默认左上）。指针移动时跟着走。
    QPointF lightPos{0.22, 0.10};
    /// 按压形变 0~1（0 = 静止）。由控件在鼠标按下时驱动。
    qreal press = 0.0;
};

/// 廉价磨砂：降采样 → 升采样 → 两遍 3×3 盒式模糊。
/// 比 QGraphicsBlurEffect 快一到两个数量级，效果对「玻璃背板」这个用途完全够。
QImage frost(const QImage& src, qreal radius);

/// 边缘折射 + 色散。src 必须已经磨砂过（顺序：先 frost 再 refract）。
/// @param radius     圆角半径（与最终绘制的圆角一致，四角按径向折射）
/// @param band       折射带宽度（像素）
/// @param strength   折射位移量（像素，建议 band × 0.5 左右）
/// @param dispersion 色散强度 0~1（三通道位移差 = strength × 0.18 × dispersion）
QImage refract(const QImage& src, qreal radius, qreal band, qreal strength,
               qreal dispersion);

/// 一步到位：磨砂 + 折射 + 色散。返回图与 src 同尺寸，可直接当 paint() 的
/// backdrop 参数。style.frost / refraction / dispersion / edgeBand 都会用到。
QImage process(const QImage& src, qreal radius, const Style& style);

/// 从整窗快照 shot 里裁出 region（控件坐标）那一块并磨砂。
/// 返回图与 region 同尺寸，可直接当作 paint() 的 backdrop 参数。
/// region 超出快照范围时按边缘像素外扩（不会返回空图，除非 shot 本身是空的）。
QImage frostedRegion(const QImage& shot, const QRect& region, qreal radius);

/// 画一块液态玻璃到 p 的 r 矩形上（内部自行裁剪成圆角）。
/// @param backdrop 与 r 等尺寸的**成品**背板（已 process）；为空则只画染色 + 高光。
/// @param dark     深色主题（决定染色与高光的颜色）。
void paint(QPainter& p, const QRectF& r, qreal radius, const QImage& backdrop,
           bool dark, const Style& style = Style());

/// 按压形变后的实际绘制矩形（以中心为锚点：横向微胀、纵向微缩）。
/// 控件自己做命中测试时应当用**原始**矩形，这个只用于绘制。
QRectF pressedRect(const QRectF& r, qreal press);

} // namespace kc::ui::glass
