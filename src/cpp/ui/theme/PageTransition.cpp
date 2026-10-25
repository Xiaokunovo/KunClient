#include "ui/theme/PageTransition.h"

#include "ui/theme/Motion.h"

#include <QApplication>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QPropertyAnimation>
#include <QRegion>

#include <cmath>
#include <cstdio>

namespace kc::ui {

namespace {

/// 源图切片数：够密则透视连续，不够密会看出台阶。
/// 🔴 **按角度自适应，而且上限压得很低**。透视是二阶效应，误差与片宽平方成正比：
///    16° / 10 片（片宽 128px）时最大偏差只有 0.4px，肉眼完全看不出台阶。
///    早先固定 32 片纯属白烧 —— 每片都是一次带平滑采样的大图缩放 blit，
///    两页合计每帧 78 次，实测 14~18ms/帧（400ms 只画出 9 帧），
///    就是用户反馈的「切换界面会卡顿不丝滑」（2026-10-17）。
///    现在 8~14 片，误差仍 < 0.7px，blit 次数降到 1/4。
int sliceCount(int width, qreal angleDeg)
{
    const int byAngle = 6 + static_cast<int>(qAbs(angleDeg) * 0.5);
    return qBound(8, qMin(byAngle, width / 80), 14);
}

/// 透视距离（像素）。越小透视越夸张；取 2.2 × 画布宽度 = 视觉舒适的弱透视。
qreal perspectiveDistance(int width)
{
    return qMax(320.0, width * 2.2);
}

/// 读取探针冻结值（0~1）。<0 = 不冻结。
qreal freezeProgress()
{
    const QByteArray env = qgetenv("KC_TRANSITION_FREEZE");
    if (env.isEmpty()) return -1.0;
    bool ok = false;
    const double v = env.toDouble(&ok);
    if (!ok) return -1.0;
    return qBound(0.0, v, 1.0);
}

/// 低成本近似高斯模糊：缩到 1/N → 再缩一半 → 回到 1/N，两次平滑缩放
/// ≈ 两次盒式模糊，肉眼看足够。**返回的仍是小图**（不放大回去）——
/// drawPageY 会把它放大着画，blit 的是小源图，比全尺寸图便宜一个数量级。
/// 不用 QGraphicsBlurEffect（那是挂在 widget 上的），也不做卷积核
/// （1280×820 每帧卷积跑不动）。
QPixmap blurPixmap(const QPixmap& src, int downscale)
{
    if (src.isNull() || downscale < 2) return src;
    QImage img = src.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QSize small = img.size() / downscale;
    if (small.width() < 16 || small.height() < 16) return src;
    img = img.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    img = img.scaled(small / 2, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    img = img.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return QPixmap::fromImage(img);
}

/// 三段缓出（0→1，起步快、尾段长而软）。
/// 转场里用在「副线动作」上：主线位移由 QPropertyAnimation 的 appleEase 驱动，
/// 失焦、投影这类**跟随**的动作各自再缓一次，层次才不会一起齐步走。
qreal easeOutQuint(qreal t)
{
    const qreal u = 1.0 - qBound(0.0, t, 1.0);
    return 1.0 - u * u * u * u * u;
}

} // namespace

PageTransition::PageTransition(QWidget* parent)
    : QWidget(parent)
{
    // 叠层不接鼠标：转场期间下层的真实新页照常可点（叠层只是快照投影）。
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // 🔴 paintEvent 第一件事就是铺满整块（壁纸快照或纯色兜底），
    //    所以不需要 Qt 再帮我们擦一遍背景 —— 省下的是一次全屏填充。
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    m_stats = !qgetenv("KC_TRANSITION_STATS").isEmpty();
    hide();
}

PageTransition::~PageTransition()
{
    // 🔴 必须**在自己的析构体里**（此刻 QWidget 部分还完好）就停掉动画。
    //    否则等基类 ~QObject 去销毁子动画时，动画停机仍会发 finished → 回到
    //    finish() → hide()/onDone()，而那会儿本控件的 QWidget 部分已经析构完了 ——
    //    段错误。症状：截图探针偶发 exit=139（约 1/12），且**只在开动画时出现**
    //    （KC_NO_ANIM=1 时为 0/12，2026-10-17 实测）。
    m_shuttingDown = true;
    if (m_anim) {
        m_anim->stop();   // 会发 finished，被 m_shuttingDown 挡掉
        m_anim = nullptr;
    }
    m_onDone = nullptr;
}

void PageTransition::begin(const QPixmap& from, const QPixmap& to, Mode mode, bool forward,
                           const QColor& backdrop, const QRect& origin,
                           const QPixmap& backdropImage)
{
    // 打断上一段：令牌作废，旧动画的 finished 不再收尾。
    ++m_generation;
    if (m_anim) {
        m_anim->stop();
        m_anim->deleteLater();
        m_anim = nullptr;
    }

    m_from = from;
    m_to = to;
    // 旧页模糊版：转场开始时算一次（约 4~6ms，一次性），之后每帧只做一次叠加。
    // 6 倍降采样 ≈ 半径 12px 的模糊 —— 够看出「退焦」，又不至于糊成一片色块。
    m_fromBlur = blurPixmap(from, 6);
    m_backdrop = backdrop;
    // 尺寸对不上就不用（外壳可能在转场前一刻才改过窗口尺寸）。
    m_backdropImage = (backdropImage.size() == size()) ? backdropImage : QPixmap();
    m_mode = mode;
    m_origin = origin;
    m_forward = forward;
    m_progress = 0.0;
    m_running = true;
    m_statFrames = 0;
    m_statTicks = 0;
    m_statTotalMs = 0.0;
    m_statMaxMs = 0.0;

    raise();
    show();
    update();
}

void PageTransition::drive(qreal p)
{
    if (!m_running) return;
    setProgress(qBound(0.0, p, 1.0));
}

void PageTransition::settle(qreal target, std::function<void()> onDone)
{
    if (!m_running) {
        if (onDone) onDone();
        return;
    }
    ++m_generation;
    if (m_anim) {
        m_anim->stop();
        m_anim->deleteLater();
        m_anim = nullptr;
    }
    m_onDone = std::move(onDone);

    const qreal from = m_progress;
    const qreal to = qBound(0.0, target, 1.0);
    // 补完的时长按「还差多少」定：只差 10% 却按 340ms 补，会明显发飘；
    // 差 80% 时给足时间，收尾才不像被硬拽过去。
    const int duration = qBound(130, 90 + static_cast<int>(260 * qAbs(to - from)), 340);

    auto* anim = new QPropertyAnimation(this, "progress", this);
    anim->setDuration(duration);
    anim->setStartValue(from);
    anim->setEndValue(to);
    // 往前补（落定）用 appleEase 收尾；往回收（回弹）用 exitEase —— 回弹要快、
    // 要有「弹回去」的果断感，用长尾缓动会显得黏。
    anim->setEasingCurve(to > from ? Motion::appleEase() : Motion::exitEase());

    const int generation = m_generation;
    connect(anim, &QPropertyAnimation::finished, this, [this, generation] {
        if (m_shuttingDown) return;
        if (generation != m_generation) return;
        finish();
    });

    m_anim = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void PageTransition::start(const QPixmap& from, const QPixmap& to, Mode mode, bool forward,
                           const QColor& backdrop, std::function<void()> onDone,
                           const QRect& origin, const QPixmap& backdropImage)
{
    begin(from, to, mode, forward, backdrop, origin, backdropImage);
    m_onDone = std::move(onDone);

    // 360 → 400：这一版加了「旧页后退 + 失焦 + 双层投影」，位移之外还有景深层次，
    // 太快的话这些层次来不及看清，反而显得糊里糊涂地闪了一下。
    // 曲线用 appleEase（0.32,0.72,0,1）：起步果断、尾段长而软，
    // 比 silkEase 的「前 20% 走完 80%」更像有质量的东西在移动。
    const int duration = (mode == Mode::Cube) ? 460 : (mode == Mode::Zoom ? 440 : 400);
    auto* anim = new QPropertyAnimation(this, "progress", this);
    anim->setDuration(duration);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setEasingCurve(Motion::appleEase());

    const int generation = m_generation;
    const qreal freeze = freezeProgress();

    if (freeze >= 0.0) {
        // 探针：冻结在指定进度，不自动收尾（截图取证用）。
        connect(anim, &QPropertyAnimation::valueChanged, this,
                [this, freeze](const QVariant& v) {
            if (v.toReal() >= freeze) {
                if (m_anim) m_anim->pause();
                m_progress = freeze;
                update();
            }
        });
    } else {
        connect(anim, &QPropertyAnimation::finished, this, [this, generation] {
            if (m_shuttingDown) return;               // 正在析构，别再碰本控件
            if (generation != m_generation) return;   // 已被更新的转场顶替
            finish();
        });
    }

    m_anim = anim;
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void PageTransition::cancel()
{
    if (!m_running) return;
    ++m_generation;
    if (m_anim) {
        m_anim->stop();
        m_anim->deleteLater();
        m_anim = nullptr;
    }
    finish();
}

void PageTransition::setProgress(qreal p)
{
    if (qFuzzyCompare(m_progress, p)) return;
    ++m_statTicks;
    m_progress = p;
    update();
}

void PageTransition::finish()
{
    if (m_shuttingDown) return;
    m_running = false;
    hide();
    if (m_stats && m_statFrames > 0) {
        std::fprintf(stderr, "[trans] ticks=%d frames=%d avg=%.2fms max=%.2fms\n",
                     m_statTicks, m_statFrames, m_statTotalMs / m_statFrames, m_statMaxMs);
    }
    // 收尾时断开对动画的引用：它随后会被 DeleteWhenStopped 自删，
    // 留着引用只会让下一次 start() 去碰一个正在析构的对象。
    m_anim = nullptr;
    auto done = std::move(m_onDone);
    m_onDone = nullptr;
    if (done) done();
}

// ─────────────────────────────────────────────────────────────────────────────
//  逐切片透视投影（源驱动）
//
//  源图按 drawW 宽度贴在画布上（左缘 drawX0，含整体横移 xOffset），再绕竖直轴
//  ox 旋转 θ，最后过透视除法落到画布：
//      u_d = u_s · cosθ · d / (d + u_s · sinθ),   u = x - ox,  d = 透视距离
//  按源图均匀切 N 片，每片左右两条边各投影一次，得到目标矩形与源矩形，
//  逐片 drawPixmap。旋转后的平面在屏幕上自然变窄（cosθ），无需额外算边界。
//  另按 |θ| 叠暗角补「光照」—— 没有它旋转看起来像贴纸弯折而不是刚体。
// ─────────────────────────────────────────────────────────────────────────────
void PageTransition::drawPageY(QPainter& p, const QPixmap& pm, qreal angleDeg, qreal originXRatio,
                               qreal xOffset, qreal scale, qreal opacity, qreal dim,
                               int slicesOverride, qreal scaleY, qreal yOffset,
                               bool opaqueBacking)
{
    if (pm.isNull() || opacity <= 0.001) return;

    const qreal W = width();
    const qreal H = height();
    const qreal theta = angleDeg * M_PI / 180.0;
    const qreal d = perspectiveDistance(static_cast<int>(W));
    const qreal cosT = std::cos(theta);
    const qreal sinT = std::sin(theta);

    const qreal srcW = pm.width();
    const qreal srcH = pm.height();
    if (srcW <= 0 || srcH <= 0) return;

    // scaleY <= 0 表示「跟 scale 一样」（绝大多数调用都是等比缩放）；
    // Zoom 转场要非等比，靠 scaleY + yOffset 把页面钉在图标中心再长到全屏。
    const qreal sy = (scaleY > 0.0) ? scaleY : scale;
    const qreal drawW = W * scale;
    const qreal drawH = H * sy;
    if (drawW <= 0 || drawH <= 0) return;
    const qreal drawX0 = (W - drawW) / 2.0 + xOffset;
    const qreal top = (H - drawH) / 2.0 + yOffset;
    // 🔴 旋转轴要相对**这一页自身**取，不能相对画布。
    //    推入时页面已平移到画布外很远，若把轴钉在画布边缘，页面离轴越远
    //    透视越狠 —— 右半幅会被压成一条，看着像纸张被扯变形。
    const qreal ox = drawX0 + drawW * originXRatio;
    const qreal k = srcW / drawW;   // 画布坐标 → 源图像素

    const int slices = slicesOverride > 0 ? slicesOverride
                                          : sliceCount(static_cast<int>(drawW), angleDeg);
    const qreal sliceW = drawW / slices;
    const qreal shade = std::abs(sinT) * 0.55 + qBound(0.0, dim, 1.0);

    // 衬底：有壁纸快照就切一块铺上 —— 页面在动、壁纸不动，透过玻璃看到的
    // 就是静止的背景（物理正确），也顺便挡掉了下层新页。
    // 🔴 只铺**一次整幅**，不逐片铺。衬底取的是**画布坐标**的壁纸，与页面的
    //    透视变形无关；逐片铺 14 次纯属重复劳动。多铺出页面轮廓的那一块也
    //    看不出来 —— 它本身就是背景（2026-10-17 实测省掉 13 次全幅 blit）。
    const bool haveBackdropImg = !m_backdropImage.isNull();

    p.save();

    if (opaqueBacking) {
        p.setOpacity(opacity);
        const QRectF whole(drawX0, top, drawW, drawH);
        if (haveBackdropImg) {
            p.drawPixmap(whole, m_backdropImage, whole);
        } else if (m_backdrop.isValid()) {
            p.fillRect(whole, m_backdrop);
        }
    }

    // ── 快速路径：几乎无旋转 ──────────────────────────────────────────────
    //   |θ| < 0.25° 时透视位移不到 0.3px，逐切片纯属白烧 —— 一次整幅
    //   drawPixmap 就够，成本是切片路径的 1/N（Zoom 的后段、Fluid 的新页
    //   尾段都会走到这里）。
    if (qAbs(angleDeg) < 0.25) {
        const QRectF dest(drawX0, top, drawW, drawH);
        p.setOpacity(opacity);
        p.drawPixmap(dest, pm, QRectF(0, 0, srcW, srcH));
        if (shade > 0.002) {
            p.fillRect(dest, QColor(0, 0, 0, static_cast<int>(qBound(0.0, shade, 1.0) * 255.0)));
        }
        p.restore();
        return;
    }

    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int i = 0; i < slices; ++i) {
        const qreal sc0 = i * sliceW;                 // 源切片左缘（画布坐标）
        const qreal sc1 = (i + 1) * sliceW;           // 源切片右缘
        const qreal us0 = drawX0 + sc0 - ox;          // 相对旋转轴
        const qreal us1 = drawX0 + sc1 - ox;

        const qreal ud0 = us0 * cosT * d / (d + us0 * sinT);
        const qreal ud1 = us1 * cosT * d / (d + us1 * sinT);

        const qreal xd0 = ox + ud0;
        const qreal xd1 = ox + ud1;
        const qreal destW = xd1 - xd0;
        if (destW <= 0.05) continue;

        const QRectF dest(xd0, top, destW, drawH);
        const QRectF src(sc0 * k, 0.0, (sc1 - sc0) * k, srcH);

        p.save();
        p.setOpacity(opacity);
        p.drawPixmap(dest, pm, src);
        // 受光变暗必须**叠黑**，不能乘进 opacity ——
        // 乘进 alpha 会让本该实心的新页半透明，下面的旧页透上来，
        // 又变回重影（切页时两套文字叠在一起）。
        if (shade > 0.002) {
            p.fillRect(dest, QColor(0, 0, 0, static_cast<int>(qBound(0.0, shade, 1.0) * 255.0)));
        }
        p.restore();
    }
    p.restore();
}

void PageTransition::paintEvent(QPaintEvent*)
{
    QElapsedTimer timer;
    if (m_stats) timer.start();
    auto mark = [this, &timer]() {
        if (!m_stats) return;
        const double ms = timer.nsecsElapsed() / 1e6;
        ++m_statFrames;
        m_statTotalMs += ms;
        if (ms > m_statMaxMs) m_statMaxMs = ms;
    };

    QPainter p(this);
    // 衬底先铺满：快照的玻璃页面底是半透明的，直接叠在透明叠层上会透出
    // 下层已切换的新页（旧页快照出现重影）。
    // 🔴 衬底必须是**壁纸**而不是纯色：只铺纯色的话，这 400ms 里窗口背景
    //    （照片 / 视频）整块消失，切完才「跳」回来 —— 用户反馈的
    //    「背景也会消失」就是这个（2026-10-17）。纯色只在拿不到壁纸时兜底。
    if (!m_backdropImage.isNull() && m_backdropImage.size() == size()) {
        p.drawPixmap(0, 0, m_backdropImage);
    } else if (m_backdrop.isValid()) {
        p.fillRect(rect(), m_backdrop);
    }

    if (m_from.isNull() && m_to.isNull()) { mark(); return; }

    const qreal t = m_progress;
    const qreal dir = m_forward ? 1.0 : -1.0;

    if (m_mode == Mode::Cube) {
        // ── Cube：两页各绕屏幕竖轴转 60°，旧页转出去、新页转进来 ──
        const qreal aFrom = dir * 60.0 * t;
        const qreal aTo = -dir * 60.0 * (1.0 - t);
        drawPageY(p, m_from, aFrom, 0.5, -dir * width() * 0.18 * t, 1.0 - 0.06 * t,
                  1.0 - 0.85 * t);
        drawPageY(p, m_to, aTo, 0.5, dir * width() * 0.18 * (1.0 - t), 0.94 + 0.06 * t,
                  0.15 + 0.85 * t, 0.0, 0, 0.0, 0.0, /*opaqueBacking=*/true);
    } else if (m_mode == Mode::Zoom && m_origin.isValid()) {
        // ── Zoom：新页从**被点的那个图标**里长出来 ──────────────────────
        //  点图标进页是最常见的动作，用推入太"公文"，这一下要有「应用打开了」的
        //  手感：页面从图标中心起跳、边放大边淡入，主屏被压暗并轻微失焦。
        const qreal W = width();
        const qreal H = height();

        // 1) 先把新页几何算出来（旧页的裁剪区要靠它）
        //    新页中心从图标中心插值到画布中心，尺寸从 0.5 长到 1.0。
        //    🔴 缩放要**在 90% 就到位**：拖到 t=1 才满幅的话，90% 之后
        //       只有 95% 大小，四周露出一圈主屏 —— 看着像页面没铺满。
        //    🔴 这里**不能再缓动一次**：m_progress 已经是 appleEase 过的值，
        //       再套 easeOutQuint 会让页面在头 10% 的时间里直接长到 95%，
        //       「从图标长出来」的过程根本看不见（2026-10-17 实测）。
        const qreal zt = qMin(1.0, t / 0.90);
        const qreal zoom = 0.50 + 0.50 * zt;
        // 淡入放在前 25%：快照压到 0.5 倍时是一团缩略图，直接满不透明地出现
        // 会"啪"一下糊在屏幕上；淡着长出来才像光从图标里透出来。
        const qreal op = qBound(0.0, t / 0.25, 1.0);
        const qreal ang = dir * 8.0 * (1.0 - zt);

        const QPointF c0 = QRectF(m_origin).center();
        const QPointF c1(W / 2.0, H / 2.0);
        const QPointF c = c0 + (c1 - c0) * t;

        const qreal pw = W * zoom;
        const qreal ph = H * zoom;
        const qreal pl = c.x() - pw / 2.0;
        const qreal pt = c.y() - ph / 2.0;

        // 2) 底下的主屏：原地不动，只压暗 + 失焦（它正在"被进入"，不该跟着动）
        //    🔴 新页一旦**完全不透明**（op==1，t≥25%），它底下那块主屏就再也
        //       看不见了 —— 直接挖掉不画。旧页清晰 + 失焦两层都是全屏绘制，
        //       挖掉之后省下的正是每帧最贵的两次全幅 blit。
        //       矩形向外放 3px：新页带 8° 旋转，轮廓比轴对齐矩形略宽，
        //       不放会沿边露出一线主屏（看着像毛边）。
        const qreal fromDim = 0.45 * t;
        const bool hideFromBehindTo = (op >= 0.999);
        QRegion fromClip(rect());
        if (hideFromBehindTo) {
            fromClip -= QRegion(QRectF(pl - 3.0, pt - 3.0, pw + 6.0, ph + 6.0).toRect());
        }
        if (!fromClip.isEmpty()) {
            p.save();
            p.setClipRegion(fromClip);
            drawPageY(p, m_from, 0.0, 0.5, 0.0, 1.0, 1.0, fromDim);
            if (!m_fromBlur.isNull() && t > 0.05) {
                drawPageY(p, m_fromBlur, 0.0, 0.5, 0.0, 1.0,
                          qBound(0.0, t * t, 1.0) * 0.50, fromDim);
            }
            p.restore();
        }

        // 3) 生长中的页面投在主屏上的影子：四条边各一道渐变。
        //    没有它，页面像直接"贴"在壁纸上；有了它才是"一张纸浮起来"。
        //    页面带 8° 以内的轻微旋转，用轴对齐矩形近似完全够（差值不到 2px）。
        const bool darkZoom = m_backdrop.lightness() < 128;
        const int zoomShadow = static_cast<int>((darkZoom ? 110 : 46) * (1.0 - 0.5 * t));
        if (zoomShadow > 0 && zoom < 0.999) {
            const qreal sw = 46.0;
            const QColor ink(0, 0, 0, zoomShadow);
            const QColor clear(0, 0, 0, 0);
            {
                QLinearGradient g(pl - sw, 0, pl, 0);
                g.setColorAt(0.0, clear);
                g.setColorAt(1.0, ink);
                p.fillRect(QRectF(pl - sw, pt, sw, ph), g);
            }
            {
                QLinearGradient g(pl + pw, 0, pl + pw + sw, 0);
                g.setColorAt(0.0, ink);
                g.setColorAt(1.0, clear);
                p.fillRect(QRectF(pl + pw, pt, sw, ph), g);
            }
            {
                QLinearGradient g(0, pt - sw, 0, pt);
                g.setColorAt(0.0, clear);
                g.setColorAt(1.0, ink);
                p.fillRect(QRectF(pl, pt - sw, pw, sw), g);
            }
            {
                QLinearGradient g(0, pt + ph, 0, pt + ph + sw);
                g.setColorAt(0.0, ink);
                g.setColorAt(1.0, clear);
                p.fillRect(QRectF(pl, pt + ph, pw, sw), g);
            }
        }

        // drawPageY 里页面是按画布居中再叠加偏移的 → 传"中心与画布中心的差"。
        // 衬底只在**淡入阶段**要（op<1 时新页半透明，底下还有主屏，不铺会透出
        // 图标显得脏）；op 到 1 之后主屏已经被挖掉了，衬底画的还是壁纸，
        // 跟叠层背景一模一样 —— 白画，关掉。
        drawPageY(p, m_to, ang, 0.5, c.x() - W / 2.0, zoom, op, 0.0, /*slicesOverride=*/0,
                  /*scaleY=*/zoom, /*yOffset=*/c.y() - H / 2.0,
                  /*opaqueBacking=*/op < 0.999);
    } else {
        // ── Fluid：新页迎面推入，旧页向反方向**退后** ────────────────────
        //    「推入」这件事只有位移是不够的 —— 两页同尺寸、同清晰度、同亮度
        //    平移，看着就是两个矩形在滑。真正读得出前后关系要靠三层：
        //      · 旧页缩小 + 压暗 + 失焦  → 它退到后面去了；
        //      · 旧页位移量比新页小（0.26W vs W）→ 视差；
        //      · 新页前缘的双层投影 + 1px 高光边 → 它是一张压上来的纸。
        const qreal W = width();
        const qreal H = height();

        // ── 0. 先把新页几何算出来（旧页的裁剪区要靠它）──
        //    角度在 72% 前收干净：末尾若还带角度，透视会让页面缩窄，
        //    边缘露出一条旧页的缝；而且后半程走快速路径能省下十几次切片 blit。
        //    角度也从 16° 收到 10° —— 推入时的"斜"主要靠旧页后退 + 投影表达，
        //    新页本身歪太多反而不像"一张纸压上来"。
        //    🔴 scale 保持 1.0（早先是 1.035）：页面比画布略大时每一片都要
        //       重采样；1.0 时切片宽度 ≈ 源宽度，光栅引擎能走接近 1:1 的快路径。
        //       画布外那一截本来就被裁掉，1.035 只带来成本不带来观感。
        const qreal aTo = dir * 10.0 * qMax(0.0, (0.72 - t) / 0.72);
        const qreal toScale = 1.0;
        const qreal slide = dir * W * (1.0 - t);
        const qreal drawW = W * toScale;
        // 前缘必须**钉在 slide 上**：页面比画布窄时若靠居中，前缘会缩进去一截，
        // 和旧页之间露出一条衬底的缝。前进钉左缘、后退钉右缘。
        const qreal toShift = slide - dir * (W - drawW) / 2.0;

        // 前缘在画布上的 x（前进=左缘，后退=右缘）。
        // 从**实际几何**反算，不要再用 slide 推 —— 页面比画布窄时两者不等，
        // 推错了投影和高光边会离纸边差出十几像素。
        const qreal drawX0 = (W - drawW) / 2.0 + toShift;
        const qreal edge = (dir > 0) ? drawX0 : drawX0 + drawW;
        const bool edgeInside = (edge > 1.0 && edge < W - 1.0);

        // ── 1+2. 旧页：后退 + 失焦 ──
        //    🔴 只画**新页还没盖到**的那条带（前进时是新页左缘以左）。
        //       旧页本来就被新页整块压住，全幅画它等于每帧白烧半屏像素 ——
        //       这是「切页卡顿」的大头之一。裁剪后视觉完全一致
        //       （被裁掉的部分正好被新页的不透明衬底盖住）。
        const qreal aFrom = dir * 7.0 * t;
        const qreal fromScale = 1.0 - 0.075 * t;          // 1.0 → 0.925
        const qreal fromShift = -dir * W * 0.26 * t;      // 视差：走得比新页慢
        const qreal fromDim = 0.20 * t;                   // 逐步压暗，沉进背景

        const qreal visibleW = dir > 0 ? qMax(0.0, edge) : qMax(0.0, W - edge);
        if (visibleW > 0.5) {
            p.save();
            p.setClipRect(dir > 0 ? QRectF(0, 0, visibleW, H)
                                  : QRectF(edge, 0, visibleW, H));
            drawPageY(p, m_from, aFrom, m_forward ? 1.0 : 0.0, fromShift, fromScale,
                      1.0, fromDim);

            // 旧页失焦层：同源图叠上去，浓度随进度升。
            // 同源叠加不会产生重影（两张图内容完全一致，只是一张糊），
            // 效果就是「逐渐变糊」而不是「突然糊掉」。
            // 🔴 用 t² 而不是线性：失焦要**晚到**。早先按 easeOutQuint 冲到 0.85，
            //    55% 时文字已经糊成一团白雾，看着像磨砂玻璃而不是"退远了的页面"。
            //    平方曲线让它前半程只是轻微发虚、后半程才真正散焦。
            // 🔴 角度传 0：已经糊成这样，7° 的透视根本看不出来，但切片放大
            //    （1/6 小图拉到全幅）是全场最贵的一次采样 —— 走快速路径
            //    一次画完，省下 8 次全幅放大 blit。
            if (!m_fromBlur.isNull()) {
                const qreal blurMix = qBound(0.0, t * t, 1.0) * 0.72;
                // 阈值 0.05（≈t>0.26）而不是 0.012：更早的混合浓度肉眼看不出来，
                // 但这一层是「1/6 小图放大到全幅」，每次采样要读四个源像素 ——
                // 是全场最贵的一笔，前 1/4 的时间白画它不值。
                if (blurMix > 0.05) {
                    drawPageY(p, m_fromBlur, 0.0, m_forward ? 1.0 : 0.0, fromShift,
                              fromScale, blurMix, fromDim);
                }
            }
            p.restore();
        }

        // ── 4. 双层投影：贴边一层浓的（接触影）+ 外面一层宽的（环境影） ──
        //    单层渐变看着像"贴纸下面糊了一块灰"；两层才有纸张离开桌面的厚度。
        //    浓度跟主题走：浅色底上同样的黑会糊成一条脏缝。
        const bool dark = m_backdrop.lightness() < 128;
        const int contactAlpha = dark ? 150 : 64;
        const int ambientAlpha = dark ? 80 : 40;
        auto drawShadow = [&](qreal shW, int alpha) {
            if (alpha <= 0 || shW <= 0.5) return;
            if (dir > 0) {
                if (edge <= 0.5) return;
                QLinearGradient g(edge - shW, 0, edge, 0);
                g.setColorAt(0.0, QColor(0, 0, 0, 0));
                g.setColorAt(0.6, QColor(0, 0, 0, alpha * 2 / 5));
                g.setColorAt(1.0, QColor(0, 0, 0, alpha));
                p.fillRect(QRectF(edge - shW, 0, shW, H), g);
            } else {
                if (edge >= W - 0.5) return;
                QLinearGradient g(edge, 0, edge + shW, 0);
                g.setColorAt(0.0, QColor(0, 0, 0, alpha));
                g.setColorAt(0.4, QColor(0, 0, 0, alpha * 2 / 5));
                g.setColorAt(1.0, QColor(0, 0, 0, 0));
                p.fillRect(QRectF(edge, 0, shW, H), g);
            }
        };
        if (edgeInside) {
            // 投影在头 35% 里淡入：纸边刚进画面时影子不该是满浓度，
            // 否则页面还在画布外，右缘就先黑了一块。
            const qreal ramp = easeOutQuint(qBound(0.0, t / 0.35, 1.0));
            drawShadow(150.0, static_cast<int>(ambientAlpha * ramp));  // 宽而淡：环境光被挡住的那一圈
            drawShadow(26.0, static_cast<int>(contactAlpha * ramp));   // 窄而浓：纸边贴着桌面的接触影
        }

        // 🔴 opaqueBacking 关掉：旧页已经裁到「新页还没盖到」的那条带里，
        //    两页在画布上**不重叠** —— 新页底下只有壁纸，不会透出旧页内容。
        //    开着的话每片都要多铺一次衬底，等于把新页画两遍。
        drawPageY(p, m_to, aTo, m_forward ? 0.0 : 1.0, toShift, toScale, 1.0,
                  0.0, 0, 0.0, 0.0, /*opaqueBacking=*/false);

        // ── 5. 前缘高光边：1.5px 的「纸边」 ──
        //    没有它，新页和投影之间是一条硬切；有了这一线，边缘才有厚度。
        //    深色底上用白光，浅色底上用极淡的墨（白光在浅底上等于没画）。
        if (edgeInside) {
            const QColor rim = dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 26);
            p.fillRect(QRectF(edge - 0.75, 0, 1.5, H), rim);
        }
    }
    mark();
}

} // namespace kc::ui
