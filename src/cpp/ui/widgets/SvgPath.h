#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  SvgPath —— SVG path 的 `d` 属性 → QPainterPath
//
//  为什么需要它：图标要好看就得用专业图标库的字形（Phosphor / Lucide / Tabler
//  那类），而那些库只发 SVG。项目里不想为 15 个静态图标引入 Qt6::Svg
//  （静态单文件构建还得连带静态 QtSvg，且运行时栅格化 SVG 更慢），
//  所以这里做一个**最小够用的解析器**：把 `d` 直接变成矢量路径，交给 QPainter 填充。
//  好处是跨 DPI 永远清晰、可任意换色、零额外依赖。
//
//  支持的命令：M/m L/l H/h V/v C/c A/a Z/z —— 即 Phosphor / Lucide / Tabler
//  实际用到的全集（已核对：这 15 个图标只出现 M L H V C A Z 及其小写）。
//  其余命令（S/Q/T）故意不实现：用不到，实现了也没人测，不如让它显式地失败。
//
//  坐标系保持原样，调用方自己 scale（图标库通常是 256 或 24 的 viewBox）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QPainterPath>
#include <QString>

namespace kc::ui {

/// 解析 SVG path 数据。无法识别的命令会被跳过（不抛异常、不死循环）。
/// 返回值默认是 Qt::WindingFill —— 图标库的字形都按 nonzero 规则设计，
/// 用 OddEvenFill 会把「包」这类同一方向重叠的子路径挖出洞来。
QPainterPath svgPathFromString(const QString& d);

} // namespace kc::ui
