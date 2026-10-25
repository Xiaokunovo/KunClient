#include "ui/widgets/AppIcons.h"

#include "ui/widgets/SvgPath.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>

namespace kc::ui {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
//  字形数据 —— Phosphor Icons，fill 字重（MIT 许可，可商用）
//
//  2026-10-17 整包换掉：原来那套是 18×18 网格上的 1.5px **描边**轮廓，
//  缩到图标方块里（62px 方块、字形只有 ~32px）细节全糊在一起，看着廉价。
//  iOS 主屏图标本来就是**实心**字形 —— 换成 fill 之后轮廓面积大一倍，
//  小尺寸下识别度立刻不一样。
//
//  原始 viewBox 是 0 0 256 256，这里只留 `d`，渲染时按目标盒子缩放，
//  所以跨 DPI 依旧清晰、换色也只是换个 brush。图标页 https://phosphoricons.com/
// ─────────────────────────────────────────────────────────────────────────────
constexpr qreal kGlyphBox = 256.0;

const char* glyphData(AppIcon icon)
{
    switch (icon) {
    case AppIcon::Home:
        return R"SVG(M224,120v96a8,8,0,0,1-8,8H160a8,8,0,0,1-8-8V164a4,4,0,0,0-4-4H108a4,4,0,0,0-4,4v52a8,8,0,0,1-8,8H40a8,8,0,0,1-8-8V120a16,16,0,0,1,4.69-11.31l80-80a16,16,0,0,1,22.62,0l80,80A16,16,0,0,1,224,120Z)SVG";
    case AppIcon::Cpu:
        return R"SVG(M104,104h48v48H104Zm136,48a8,8,0,0,1-8,8H216v40a16,16,0,0,1-16,16H160v16a8,8,0,0,1-16,0V216H112v16a8,8,0,0,1-16,0V216H56a16,16,0,0,1-16-16V160H24a8,8,0,0,1,0-16H40V112H24a8,8,0,0,1,0-16H40V56A16,16,0,0,1,56,40H96V24a8,8,0,0,1,16,0V40h32V24a8,8,0,0,1,16,0V40h40a16,16,0,0,1,16,16V96h16a8,8,0,0,1,0,16H216v32h16A8,8,0,0,1,240,152ZM168,96a8,8,0,0,0-8-8H96a8,8,0,0,0-8,8v64a8,8,0,0,0,8,8h64a8,8,0,0,0,8-8Z)SVG";
    case AppIcon::Bolt:
        return R"SVG(M213.85,125.46l-112,120a8,8,0,0,1-13.69-7l14.66-73.33L45.19,143.49a8,8,0,0,1-3-13l112-120a8,8,0,0,1,13.69,7L153.18,90.9l57.63,21.61a8,8,0,0,1,3,12.95Z)SVG";
    case AppIcon::Package:
        return R"SVG(M223.68,66.15,135.68,18a15.88,15.88,0,0,0-15.36,0l-88,48.17a16,16,0,0,0-8.32,14v95.64a16,16,0,0,0,8.32,14l88,48.17a15.88,15.88,0,0,0,15.36,0l88-48.17a16,16,0,0,0,8.32-14V80.18A16,16,0,0,0,223.68,66.15ZM128,32l80.35,44L178.57,92.29l-80.35-44Zm0,88L47.65,76,81.56,57.43l80.35,44Zm88,55.85h0l-80,43.79V133.83l32-17.51V152a8,8,0,0,0,16,0V107.56l32-17.51v85.76Z)SVG";
    case AppIcon::Phone:
        return R"SVG(M176,16H80A24,24,0,0,0,56,40V216a24,24,0,0,0,24,24h96a24,24,0,0,0,24-24V40A24,24,0,0,0,176,16ZM80,32h96a8,8,0,0,1,8,8v8H72V40A8,8,0,0,1,80,32Zm96,192H80a8,8,0,0,1-8-8v-8H184v8A8,8,0,0,1,176,224Z)SVG";
    case AppIcon::PhoneScan:
        return R"SVG(M176,16H80A24,24,0,0,0,56,40V216a24,24,0,0,0,24,24h96a24,24,0,0,0,24-24V40A24,24,0,0,0,176,16ZM128,72a12,12,0,1,1,12-12A12,12,0,0,1,128,72Z)SVG";
    case AppIcon::Chip:
        return R"SVG(M88,111.31l48,48V220a4,4,0,0,1-4,4H48a16,16,0,0,1-16-16V48A16,16,0,0,1,48,32H68a4,4,0,0,1,4,4V153.38a24,24,0,1,0,16,0ZM80,184a8,8,0,1,0-8-8A8,8,0,0,0,80,184Zm104-80a8,8,0,1,0-8,8A8,8,0,0,0,184,104Zm24-72H156a4,4,0,0,0-4,4V68.69l13.66,13.66a24,24,0,1,1-11.31,11.31l-16-16A8,8,0,0,1,136,72V36a4,4,0,0,0-4-4H92a4,4,0,0,0-4,4V88.69l61.66,61.65A8,8,0,0,1,152,156v64a4,4,0,0,0,4,4h52a16,16,0,0,0,16-16V48A16,16,0,0,0,208,32Z)SVG";
    case AppIcon::Toolbox:
        return R"SVG(M232,96a72,72,0,0,1-100.94,66L79,222.22c-.12.14-.26.29-.39.42a32,32,0,0,1-45.26-45.26c.14-.13.28-.27.43-.39L94,124.94a72.07,72.07,0,0,1,83.54-98.78,8,8,0,0,1,3.93,13.19L144,80l5.66,26.35L176,112l40.65-37.52a8,8,0,0,1,13.19,3.93A72.6,72.6,0,0,1,232,96Z)SVG";
    case AppIcon::Monitor:
        return R"SVG(M232,64V176a24,24,0,0,1-24,24H48a24,24,0,0,1-24-24V64A24,24,0,0,1,48,40H208A24,24,0,0,1,232,64ZM160,216H96a8,8,0,0,0,0,16h64a8,8,0,0,0,0-16Z)SVG";
    case AppIcon::Grid:
        return R"SVG(M120,56v48a16,16,0,0,1-16,16H56a16,16,0,0,1-16-16V56A16,16,0,0,1,56,40h48A16,16,0,0,1,120,56Zm80-16H152a16,16,0,0,0-16,16v48a16,16,0,0,0,16,16h48a16,16,0,0,0,16-16V56A16,16,0,0,0,200,40Zm-96,96H56a16,16,0,0,0-16,16v48a16,16,0,0,0,16,16h48a16,16,0,0,0,16-16V152A16,16,0,0,0,104,136Zm96,0H152a16,16,0,0,0-16,16v48a16,16,0,0,0,16,16h48a16,16,0,0,0,16-16V152A16,16,0,0,0,200,136Z)SVG";
    case AppIcon::Folder:
        return R"SVG(M245,110.64A16,16,0,0,0,232,104H216V88a16,16,0,0,0-16-16H130.67L102.94,51.2a16.14,16.14,0,0,0-9.6-3.2H40A16,16,0,0,0,24,64V208h0a8,8,0,0,0,8,8H211.1a8,8,0,0,0,7.59-5.47l28.49-85.47A16.05,16.05,0,0,0,245,110.64ZM93.34,64,123.2,86.4A8,8,0,0,0,128,88h72v16H69.77a16,16,0,0,0-15.18,10.94L40,158.7V64Z)SVG";
    case AppIcon::Terminal:
        return R"SVG(M216,40H40A16,16,0,0,0,24,56V200a16,16,0,0,0,16,16H216a16,16,0,0,0,16-16V56A16,16,0,0,0,216,40Zm-91,94.25-40,32a8,8,0,1,1-10-12.5L107.19,128,75,102.25a8,8,0,1,1,10-12.5l40,32a8,8,0,0,1,0,12.5ZM176,168H136a8,8,0,0,1,0-16h40a8,8,0,0,1,0,16Z)SVG";
    case AppIcon::Download:
        return R"SVG(M247.93,124.52C246.11,77.54,207.07,40,160.06,40A88.1,88.1,0,0,0,81.29,88.67h0A87.48,87.48,0,0,0,72,127.73,8.18,8.18,0,0,1,64.57,136,8,8,0,0,1,56,128a103.66,103.66,0,0,1,5.34-32.92,4,4,0,0,0-4.75-5.18A64.09,64.09,0,0,0,8,152c0,35.19,29.75,64,65,64H160A88.09,88.09,0,0,0,247.93,124.52Zm-50.27,25.14-32,32a8,8,0,0,1-11.32,0l-32-32a8,8,0,0,1,11.32-11.32L152,156.69V96a8,8,0,0,1,16,0v60.69l18.34-18.35a8,8,0,0,1,11.32,11.32Z)SVG";
    case AppIcon::Gear:
        return R"SVG(M237.94,107.21a8,8,0,0,0-3.89-5.4l-29.83-17-.12-33.62a8,8,0,0,0-2.83-6.08,111.91,111.91,0,0,0-36.72-20.67,8,8,0,0,0-6.46.59L128,41.85,97.88,25a8,8,0,0,0-6.47-.6A111.92,111.92,0,0,0,54.73,45.15a8,8,0,0,0-2.83,6.07l-.15,33.65-29.83,17a8,8,0,0,0-3.89,5.4,106.47,106.47,0,0,0,0,41.56,8,8,0,0,0,3.89,5.4l29.83,17,.12,33.63a8,8,0,0,0,2.83,6.08,111.91,111.91,0,0,0,36.72,20.67,8,8,0,0,0,6.46-.59L128,214.15,158.12,231a7.91,7.91,0,0,0,3.9,1,8.09,8.09,0,0,0,2.57-.42,112.1,112.1,0,0,0,36.68-20.73,8,8,0,0,0,2.83-6.07l.15-33.65,29.83-17a8,8,0,0,0,3.89-5.4A106.47,106.47,0,0,0,237.94,107.21ZM128,168a40,40,0,1,1,40-40A40,40,0,0,1,128,168Z)SVG";
    case AppIcon::Info:
        return R"SVG(M128,24A104,104,0,1,0,232,128,104.11,104.11,0,0,0,128,24Zm-4,48a12,12,0,1,1-12,12A12,12,0,0,1,124,72Zm12,112a16,16,0,0,1-16-16V128a8,8,0,0,1,0-16,16,16,0,0,1,16,16v40a8,8,0,0,1,0,16Z)SVG";
    // 自绘：内存/字库模组 —— 外框圆角长条 + 四条插槽（偶数填充规则下成为镂空）。
    // Phosphor 没有「字库」这个语义的现成字形，自己画一个比硬套 Chip 更清楚：
    // 主屏上 MtkFlash 已经占了 Chip，两个一模一样的图标并排会认错。
    case AppIcon::Memory:
        return R"SVG(M36,72H220A12,12,0,0,1,232,84V172A12,12,0,0,1,220,184H36A12,12,0,0,1,24,172V84A12,12,0,0,1,36,72ZM52,96H68V160H52ZM92,96H108V160H92ZM132,96H148V160H132ZM172,96H188V160H172Z)SVG";
    }
    return R"SVG()SVG";
}

/// 解析结果按图标缓存：字形是静态的，没理由每帧重解析一遍路径。
/// 缓存的路径处于 256 的原始 viewBox 里，缩放交给 painter 变换 ——
/// 所以一份缓存能服务任意尺寸，不用按像素密度分桶。
const QPainterPath& glyphPath(AppIcon icon)
{
    static QHash<int, QPainterPath> cache;
    const int key = static_cast<int>(icon);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) return cached.value();

    QPainterPath path = svgPathFromString(QString::fromLatin1(glyphData(icon)));
    return *cache.insert(key, path);
}

} // namespace

void drawAppIcon(QPainter& painter, const QRectF& r, AppIcon icon, const QColor& color)
{
    const QPainterPath& path = glyphPath(icon);
    if (path.isEmpty() || r.width() <= 0.0) return;

    painter.save();
    painter.translate(r.topLeft());
    const qreal scale = r.width() / kGlyphBox;
    painter.scale(scale, scale);
    // 实心字形：填充、不描边。描边会让本就饱满的字形再胖一圈、糊掉细节。
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(path);
    painter.restore();
}

QPixmap appIconPixmap(AppIcon icon, const QColor& color, int size)
{
    if (size <= 0) return QPixmap();
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawAppIcon(p, QRectF(0, 0, size, size), icon, color);
    return pm;
}

AppIcon appIconForKey(const QString& key)
{
    static const QHash<QString, AppIcon> table = {
        { QStringLiteral("DeviceHome"),     AppIcon::Home },
        { QStringLiteral("EdlFlash"),       AppIcon::Cpu },
        { QStringLiteral("FastbootFlash"),  AppIcon::Bolt },
        { QStringLiteral("Payload"),        AppIcon::Package },
        { QStringLiteral("OnePlusFlash"),   AppIcon::Phone },
        { QStringLiteral("XiaomiFastboot"), AppIcon::PhoneScan },
        { QStringLiteral("MtkFlash"),       AppIcon::Chip },
        { QStringLiteral("AndroidTools"),   AppIcon::Toolbox },
        { QStringLiteral("ScreenMirror"),   AppIcon::Monitor },
        { QStringLiteral("AppManager"),     AppIcon::Grid },
        { QStringLiteral("FileTransfer"),   AppIcon::Folder },
        { QStringLiteral("StorageBrand"),   AppIcon::Memory },
        { QStringLiteral("CommandConsole"), AppIcon::Terminal },
        { QStringLiteral("CloudDriver"),    AppIcon::Download },
        { QStringLiteral("Settings"),       AppIcon::Gear },
        { QStringLiteral("About"),          AppIcon::Info },
    };
    return table.value(key, AppIcon::Home);
}

} // namespace kc::ui
