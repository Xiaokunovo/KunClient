#include "ui/widgets/LiquidGlass.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <vector>

namespace kc::ui::glass {
namespace {

/// 一遍 3×3 盒式模糊（原地）。用来抹掉「缩小再放大」留下的方块边界。
/// 分开算行 / 列两趟，复杂度 O(w·h)，纯整数运算。
void boxBlur3(QImage& img) {
    if (img.isNull()) return;
    const int w = img.width();
    const int h = img.height();
    if (w < 3 || h < 3) return;
    img = img.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // ── 横向 ──
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        QRgb left = line[0];
        for (int x = 0; x < w; ++x) {
            const QRgb right = line[std::min(x + 1, w - 1)];
            const QRgb cur = line[x];
            const int a = (qAlpha(left) + 2 * qAlpha(cur) + qAlpha(right)) / 4;
            const int r = (qRed(left) + 2 * qRed(cur) + qRed(right)) / 4;
            const int g = (qGreen(left) + 2 * qGreen(cur) + qGreen(right)) / 4;
            const int b = (qBlue(left) + 2 * qBlue(cur) + qBlue(right)) / 4;
            left = cur;
            line[x] = qRgba(r, g, b, a);
        }
    }
    // ── 纵向 ──
    for (int x = 0; x < w; ++x) {
        QRgb top = img.pixel(x, 0);
        for (int y = 0; y < h; ++y) {
            const QRgb bottom = img.pixel(x, std::min(y + 1, h - 1));
            const QRgb cur = img.pixel(x, y);
            const int a = (qAlpha(top) + 2 * qAlpha(cur) + qAlpha(bottom)) / 4;
            const int r = (qRed(top) + 2 * qRed(cur) + qRed(bottom)) / 4;
            const int g = (qGreen(top) + 2 * qGreen(cur) + qGreen(bottom)) / 4;
            const int b = (qBlue(top) + 2 * qBlue(cur) + qBlue(bottom)) / 4;
            top = cur;
            img.setPixel(x, y, qRgba(r, g, b, a));
        }
    }
}

/// 取像素（带边界钳制）。premultiplied ARGB32。
inline QRgb at(const QRgb* base, int stride, int w, int h, int x, int y) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return base[static_cast<qsizetype>(y) * stride + x];
}

} // namespace

QImage frost(const QImage& src, qreal radius) {
    if (src.isNull()) return QImage();
    if (radius <= 0.5) return src;

    QImage work = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    // 降采样比例：半径越大缩得越狠。下限 4、上限 24 —— 再狠就糊成色块，
    // 再小则一次降采样压不住，得靠后面的盒式模糊补，反而更慢。
    const int factor = std::clamp(static_cast<int>(radius / 2.0), 4, 24);
    const QSize small(std::max(1, work.width() / factor),
                      std::max(1, work.height() / factor));

    // 缩 → 放：这一步等价于一次 factor×factor 的盒式模糊，代价几乎为零。
    work = work.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
               .scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // 再补两遍小半径盒式模糊：放大留下的「台阶」是可见的方块感，必须抹平。
    boxBlur3(work);
    boxBlur3(work);
    return work;
}

QImage refract(const QImage& src, qreal radius, qreal band, qreal strength,
               qreal dispersion) {
    if (src.isNull() || band <= 0.5 || strength <= 0.05) return src;

    QImage work = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = work.width();
    const int h = work.height();
    if (w < 4 || h < 4) return work;

    const qreal rad = std::min(radius, std::min(w, h) / 2.0);
    const qreal bw = std::min(band, std::min(w, h) * 0.45);

    // 色散：R 偏折得多一点、B 少一点，边缘因此浮出彩虹边。
    // 0.32 是「硬边上明显能看到红/青分离」的经验系数 —— 之前 0.18 太弱，
    // 在磨砂过的图上完全看不出来。
    const qreal dispK = 0.32 * std::clamp(dispersion, 0.0, 1.0);
    const qreal shiftR = strength * (1.0 + dispK);
    const qreal shiftG = strength;
    const qreal shiftB = strength * (1.0 - dispK);

    const QImage srcCopy = work;   // 采样源与写入目标必须分开，否则会自己吃自己
    const auto* srcBase = reinterpret_cast<const QRgb*>(srcCopy.constBits());
    const int srcStride = static_cast<int>(srcCopy.bytesPerLine() / 4);

    auto* dstBase = reinterpret_cast<QRgb*>(work.bits());
    const int dstStride = static_cast<int>(work.bytesPerLine() / 4);

    const qreal hw = w * 0.5;
    const qreal hh = h * 0.5;

    // 折射带内才有位移。带外直接整行拷贝（这一步占了绝大多数像素）。
    for (int y = 0; y < h; ++y) {
        const qreal qy = std::abs(y + 0.5 - hh) - (hh - rad);
        // 粗筛：这一行**有可能**落在折射带内吗？条件取宽（-bw-rad），
        // 宁可多算几行也不能漏 —— 漏一行就是边缘少一条折射，看得出接缝。
        const bool yInBand = qy > -(bw + rad);

        auto* dstLine = dstBase + static_cast<qsizetype>(y) * dstStride;
        const auto* srcLine = srcBase + static_cast<qsizetype>(y) * srcStride;
        if (!yInBand) {
            std::copy(srcLine, srcLine + w, dstLine);
            continue;
        }

        for (int x = 0; x < w; ++x) {
            // ── 圆角矩形的内向距离 ──
            const qreal qx = std::abs(x + 0.5 - hw) - (hw - rad);
            const qreal outside = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0));
            const qreal inside = std::min(std::max(qx, qy), 0.0);
            const qreal dist = -(outside + inside - rad);   // 内部为正

            if (dist >= bw) {
                dstLine[x] = srcLine[x];
                continue;
            }

            // ── 折射方向：指向中心（直边 = 法线，四角 = 45°）──
            // 🔴 方向必须是**朝内**：左边缘的像素要采样右边（更靠中心）的内容，
            //    这样原图 [shift, band] 的内容被映射到显示的 [0, band]，边缘
            //    内容被拉伸放大 —— 这才是透镜。
            //    写成朝外的话采样点会立刻跑出画布被边界钳住，位移全被吃掉，
            //    表现就是「怎么调折射都看不出变化」（2026-10-23 踩过）。
            qreal dirX = 0.0;
            qreal dirY = 0.0;
            if (qx > 0.0) dirX = (x + 0.5 < hw) ? 1.0 : -1.0;
            if (qy > 0.0) dirY = (y + 0.5 < hh) ? 1.0 : -1.0;
            if (dirX == 0.0 && dirY == 0.0) {
                // 兜底（理论上不会到这里）：按最近的边推。
                dirX = (x < w * 0.5) ? 1.0 : -1.0;
            }

            // ── 位移曲线：边缘最大、向内平滑收敛 ──
            // 用 smoothstep(u) = u²(3-2u)，**不是** u²：
            //  · u² 的平均位移只有 strength 的 1/3，band 内大半个区域几乎不动
            //    —— 500×86 的 Dock 上实测「折射调了跟没调一样」；
            //  · smoothstep 在内侧边界（u→0）导数为 0，不会留台阶（这是当初
            //    不用线性的原因）；在外侧（u→1）迅速逼近 1，边缘那一圈真的
            //    压在 strength 上。平均位移提到 1/2，才读得出「内容被吸进去」。
            const qreal u = 1.0 - dist / bw;
            qreal fall = u * u * (3.0 - 2.0 * u);

            // 圆角处曲率最大 → 折射最强。真机上四角是整个面板最亮、最扭曲的
            // 地方，按直边的量给会让四角显得「没做玻璃」。1.45 倍是肉眼刚好
            // 能看出「角上内容被吸进去」而不过火的量。
            if (qx > 0.0 && qy > 0.0) fall *= 1.45;

            const qreal sxG = x + dirX * shiftG * fall;
            const qreal syG = y + dirY * shiftG * fall;
            const qreal sxR = x + dirX * shiftR * fall;
            const qreal syR = y + dirY * shiftR * fall;
            const qreal sxB = x + dirX * shiftB * fall;
            const qreal syB = y + dirY * shiftB * fall;

            const QRgb cg = at(srcBase, srcStride, w, h, qRound(sxG), qRound(syG));
            // 色散只取 R / B 两个通道，G 用基准采样 —— 这是最省的「假色散」。
            const QRgb cr = (dispK > 0.001)
                                ? at(srcBase, srcStride, w, h, qRound(sxR), qRound(syR))
                                : cg;
            const QRgb cb = (dispK > 0.001)
                                ? at(srcBase, srcStride, w, h, qRound(sxB), qRound(syB))
                                : cg;

            dstLine[x] = qRgba(qRed(cr), qGreen(cg), qBlue(cb), qAlpha(cg));
        }
    }
    return work;
}

QImage process(const QImage& src, qreal radius, const Style& style) {
    if (src.isNull()) return QImage();

    const qreal shortEdge = std::min(src.width(), src.height());
    const qreal band = shortEdge * std::clamp(style.edgeBand, 0.04, 0.30);
    // 位移量：带宽的 0.9 倍是「肉眼一看就知道边缘在折射」的量级。
    // 取 0.5 倍时实测在磨砂图上完全看不出来 —— 位移比磨砂半径还小的时候，
    // 采样点落在同一团糊里，等于没做。
    const qreal strength = band * 0.9 * std::clamp(style.refraction, 0.0, 1.0);

    // 🔴 三步顺序是**实测出来的**，不能随便换：
    //   ① 结构折射必须在**磨砂之前**。先磨砂再位移的话，位移量（十几 px）
    //      和磨砂造成的过渡带（~2×半径）是同一个量级，采样点落在同一团糊里，
    //      边缘形变完全看不出来 —— 这就是「怎么调折射都没反应」的真正原因。
    //   ② 色散必须在**磨砂之后**补。放在磨砂之前会被糊成一团灰，边缘那条
    //      红/青分离的彩边就没了；而它对结构不敏感，磨砂后补反而更清楚。
    QImage out = src;
    if (style.refraction > 0.02) {
        out = refract(out, radius, band, strength, /*dispersion=*/0.0);
    }
    out = frost(out, style.frost);
    if (style.refraction > 0.02 && style.dispersion > 0.02) {
        out = refract(out, radius, band, strength * 0.50, style.dispersion);
    }
    return out;
}

QImage frostedRegion(const QImage& shot, const QRect& region, qreal radius) {
    if (shot.isNull() || region.isEmpty()) return QImage();

    // 先把 region 裁进快照范围；裁过就记下偏移，最后补边。
    QRect src = region.intersected(QRect(QPoint(0, 0), shot.size()));
    if (src.isEmpty()) {
        // 完全在快照外（面板比窗口还大）：取中心一块兜底，总比返回空图好。
        src = QRect(QPoint(0, 0), shot.size());
    }
    QImage crop = shot.copy(src).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    crop = frost(crop, radius);

    if (crop.size() == region.size()) return crop;

    // 尺寸对不上（边缘越界）：拉伸到目标尺寸。玻璃层本来就是糊的，
    // 拉伸带来的形变完全看不出来。
    return crop.scaled(region.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

QRectF pressedRect(const QRectF& r, qreal press) {
    const qreal k = std::clamp(press, 0.0, 1.0);
    if (k <= 0.001) return r;
    // 体积守恒的果冻形变：横向胀 1.8%、纵向缩 2.6%，中心不动。
    const qreal sx = 1.0 + 0.018 * k;
    const qreal sy = 1.0 - 0.026 * k;
    const qreal w = r.width() * sx;
    const qreal h = r.height() * sy;
    return QRectF(r.center().x() - w / 2.0, r.center().y() - h / 2.0, w, h);
}

void paint(QPainter& p, const QRectF& rect, qreal radius, const QImage& backdrop,
           bool dark, const Style& style) {
    if (rect.isEmpty()) return;

    const QRectF r = pressedRect(rect, style.press);
    const qreal rad = std::min(radius, std::min(r.width(), r.height()) / 2.0);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath shape;
    shape.addRoundedRect(r, rad, rad);
    p.setClipPath(shape);

    // ① 背板：磨砂 + 折射后的真实画面。
    if (!backdrop.isNull()) {
        p.setOpacity(1.0);
        p.drawImage(r, backdrop);
    }

    // ② 染色：竖直方向的浅→深，模拟玻璃的厚度。
    //    深色主题用近黑蓝，浅色主题用极淡冷白 —— 两边都要保证上面的文字有对比度。
    //    🔴 浅色主题的 alpha 必须比深色低（205 vs 235）：浅色玻璃压在同一张
    //    深色壁纸上，alpha 一高就变成一块**纯白板**，和 iOS 的毛玻璃完全不是
    //    一回事（2026-10-24 实测：浅色主题下 Dock 白得发亮，壁纸整个被挡掉）。
    const QColor tintTop = dark ? QColor(0x2A, 0x30, 0x3C) : QColor(0xFC, 0xFD, 0xFF);
    const QColor tintBottom = dark ? QColor(0x1A, 0x1E, 0x26) : QColor(0xF0, 0xF4, 0xFA);
    const int alphaTop = static_cast<int>(std::clamp(style.tint, 0.0, 1.0) * (dark ? 235.0 : 205.0));
    const int alphaBottom = static_cast<int>(alphaTop * 1.08);

    QLinearGradient body(r.topLeft(), r.bottomLeft());
    QColor c1 = tintTop;
    c1.setAlpha(std::min(255, alphaTop));
    QColor c2 = tintBottom;
    c2.setAlpha(std::min(255, alphaBottom));
    body.setColorAt(0.0, c1);
    body.setColorAt(1.0, c2);
    p.fillPath(shape, body);

    // ③ 镜面高光：一团跟着**光源位置**走的冷色光斑。
    //    真机玻璃边缘会聚光，光斑位置决定「这块玻璃被哪边的光照着」——
    //    指针移动时它跟着滑，观感立刻从「贴纸」变成「有厚度的材质」。
    if (style.highlight > 0.01) {
        const qreal lx = r.left() + r.width() * std::clamp(style.lightPos.x(), 0.0, 1.0);
        const qreal ly = r.top() + r.height() * std::clamp(style.lightPos.y(), 0.0, 1.0);
        const qreal glowR = std::max(r.width(), r.height()) * 0.62;
        QRadialGradient glow(QPointF(lx, ly), glowR);
        QColor g0 = dark ? QColor(0x9F, 0xD8, 0xFF) : QColor(0xFF, 0xFF, 0xFF);
        g0.setAlpha(static_cast<int>((dark ? 52.0 : 38.0) * style.highlight));
        QColor g1 = g0;
        g1.setAlpha(0);
        glow.setColorAt(0.0, g0);
        glow.setColorAt(0.55, QColor(g0.red(), g0.green(), g0.blue(), 10));
        glow.setColorAt(1.0, g1);
        p.fillPath(shape, glow);
    }

    // ④ 顶高光：玻璃上沿那条「亮线」是液态玻璃最好认的特征。
    //    光源越靠上这条越强（光从下面照上来时几乎不该有顶边亮线）。
    if (style.highlight > 0.01) {
        const qreal topBias = std::clamp(1.15 - style.lightPos.y() * 1.4, 0.15, 1.15);
        // 亮带只覆盖上沿 32%：42% 时半块玻璃都是白的，读起来像「蒙了一层雾」
        // 而不是「玻璃上沿反光」。
        QLinearGradient sheen(r.topLeft(), QPointF(r.left(), r.top() + r.height() * 0.32));
        QColor s0 = dark ? QColor(255, 255, 255, 58) : QColor(255, 255, 255, 148);
        QColor s1 = s0;
        s1.setAlpha(0);
        s0.setAlpha(static_cast<int>(s0.alpha() * style.highlight * topBias));
        sheen.setColorAt(0.0, s0);
        sheen.setColorAt(1.0, s1);
        p.fillPath(shape, sheen);

        // ⑤ 底部内阴影：让玻璃有「厚度」，而不是一张贴纸。
        QLinearGradient foot(QPointF(r.left(), r.bottom() - r.height() * 0.28),
                             QPointF(r.left(), r.bottom()));
        QColor f0(0, 0, 0, 0);
        QColor f1 = dark ? QColor(0, 0, 0, 70) : QColor(0, 0, 0, 34);
        f1.setAlpha(static_cast<int>(f1.alpha() * style.highlight));
        foot.setColorAt(0.0, f0);
        foot.setColorAt(1.0, f1);
        p.fillPath(shape, foot);
    }

    p.setClipping(false);

    // ⑥ 内侧描边：上亮下暗的一圈 1.2px —— 没有它玻璃会「融进」背景。
    if (style.highlight > 0.01) {
        QLinearGradient rim(r.topLeft(), r.bottomLeft());
        QColor rTop = dark ? QColor(255, 255, 255, 108) : QColor(255, 255, 255, 186);
        QColor rMid = dark ? QColor(255, 255, 255, 34) : QColor(255, 255, 255, 76);
        QColor rBot = dark ? QColor(255, 255, 255, 18) : QColor(0, 0, 0, 18);
        rTop.setAlpha(static_cast<int>(rTop.alpha() * style.highlight));
        rim.setColorAt(0.0, rTop);
        rim.setColorAt(0.45, rMid);
        rim.setColorAt(1.0, rBot);

        QPen pen(QBrush(rim), 1.2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(0.6, 0.6, -0.6, -0.6), rad, rad);
    }

    // ⑦ 外描边：1px 极淡暗线，把玻璃从背景上「切」出来。
    p.setPen(QPen(dark ? QColor(0, 0, 0, 90) : QColor(15, 23, 42, 26), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);

    p.restore();
}

} // namespace kc::ui::glass
