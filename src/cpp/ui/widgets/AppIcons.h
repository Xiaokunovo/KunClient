#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AppIcons —— 应用矢量图标表（控制台主屏 / 各处快捷入口共用）
//
//  从原 ColorOS 17 导航轨的 NavButton 里抽出来，2026-10-17 外壳改成 iOS 启动器式
//  控制台后成为主屏图标网格的资产 —— 抽成公共组件，避免「控制台一套、别处再画一套」。
//
//  绘制语言（2026-10-17 第二代）：字形取自 **Phosphor Icons 的 fill 字重**（MIT），
//  以 SVG path 形式内嵌、由 SvgPath 解析成 QPainterPath 后**实心填充**。
//  第一代是 18×18 网格上的 1.5px 描边，在 62px 方块里细节糊成一团；换成实心
//  字形后小尺寸识别度明显更好，也更接近 iOS 主屏图标的观感。
//
//  仍然零图片资源、零 Qt6::Svg 依赖：路径是矢量的，跨 DPI 清晰，换色只是换 brush。
//  调用方给任意正方形盒子，内部等比缩放（字形原始 viewBox 为 256×256）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QPixmap>
#include <QRectF>
#include <QString>

class QPainter;

namespace kc::ui {

enum class AppIcon {
    Home, Cpu, Bolt, Package, Phone, PhoneScan, Chip,
    Toolbox, Monitor, Grid, Folder, Terminal, Download, Gear, Info,
    /// 内存/字库模组（「小米字库牌子」页）：带插槽的长条，区别于 MtkFlash 的 Chip。
    Memory,
};

/// 在 r（正方形盒子）里画图标；color 决定描边色。
void drawAppIcon(QPainter& painter, const QRectF& r, AppIcon icon, const QColor& color);

/// 把图标渲染成一张 size×size 的 pixmap（供 QIcon / 菜单行 / 小按钮用）。
QPixmap appIconPixmap(AppIcon icon, const QColor& color, int size);

/// 页面 key → 图标（未知 key 回落到 Home）。
AppIcon appIconForKey(const QString& key);

} // namespace kc::ui
