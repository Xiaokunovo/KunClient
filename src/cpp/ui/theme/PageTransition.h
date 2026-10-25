#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PageTransition — 页面转场的「真 3D」叠层（ColorOS 17 设计语言）
//
//  做法：转场开始时把新旧两页各抓一张快照（QWidget::grab），叠在一层全屏
//  叠层控件上，用**逐切片透视投影**把快照按 rotateY 画出来（QTransform 只有
//  仿射、没有透视除法，所以按目标切片宽度反解源坐标，一片一片 drawPixmap）。
//  动画结束后叠层退场，真实控件无痕接位 —— 页面架构（QStackedWidget 常驻
//  缓存）一行不动，EDL 内嵌旧窗也照常参与（只是被抓张快照）。
//
//  两种转场（避免「太单一」）：
//   · Fluid（组内翻页）：新页绕前缘 swing 入场（rotateY 26°→0 + 淡入），
//     旧页沉后（rotateY 12° + 缩放 0.94 + 淡出）—— 纵深感、日常翻页用；
//   · Cube（跨组跳转）：新旧两页各绕屏幕竖轴转 60°，像翻动一块立方体 —
//     — 跨「刷机 / 工具 / 其他」的跳跃用，动作更大、仪式感更强。
//
//  性能：只动画 transform / opacity（切片本身就是 drawPixmap 的矩形变换），
//  48 片/页；一次转场两张 1280×820 快照，结束即释放。
//
//  降级：Motion::animationsEnabled() 为假（KC_NO_ANIM=1 或设置页关动画）时
//  外壳根本不会创建本叠层（见 MainShell::setCurrentIndex）。
//  探针：KC_TRANSITION_FREEZE=0.5 时动画冻结在 50% 不自动收尾，供截图取证。
// ─────────────────────────────────────────────────────────────────────────────

#include <QPointer>
#include <QWidget>

#include <functional>

class QPropertyAnimation;

namespace kc::ui {

class PageTransition : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal progress READ progress WRITE setProgress)

public:
    /// Fluid  = 页间推入（旧页后退失焦，新页迎面压上来）；
    /// Cube   = 旧的两页绕竖轴翻（保留，当前外壳不用）；
    /// Zoom   = 从主屏某个图标的位置「长」出新页（点图标进页专用）。
    enum class Mode { Fluid, Cube, Zoom };

    explicit PageTransition(QWidget* parent = nullptr);
    ~PageTransition() override;

    /// 跑一段转场。from = 旧页快照，to = 新页快照，forward = 前进（组内序号变大）。
    /// backdrop = 叠层衬底色（窗口底色，用于明暗判断 + 壁纸缺失时的兜底）。
    /// backdropImage = **当前壁纸在本叠层坐标系里的渲染结果**（外壳给）。
    ///   🔴 必须有它：快照的玻璃底是半透明的，衬底只铺纯色的话，转场这 400ms
    ///      里壁纸 / 视频背景会整块消失（用户反馈「背景也会消失」就是这个）。
    ///      壁纸当衬底还顺带解决了「透出下层已切换的新页」的重影问题。
    /// onDone 在动画正常结束后回调（被 cancel 打断时也会回调一次）。
    /// origin = Mode::Zoom 的生长起点（图标在**叠层坐标系**里的矩形）；
    ///          空矩形时 Zoom 会退化成 Fluid。
    void start(const QPixmap& from, const QPixmap& to, Mode mode, bool forward,
               const QColor& backdrop, std::function<void()> onDone,
               const QRect& origin = QRect(), const QPixmap& backdropImage = QPixmap());

    /// 立即打断当前转场：冻结在当前帧不补播，直接收尾并回调。
    void cancel();

    // ── 交互式（跟手）转场：主屏 ⇄ 控制中心的横滑 ────────────────────────
    /// 摆好两页快照、停在 progress = 0，由外部逐帧 drive() 推进。
    /// 与 start() 的区别：**不启动动画**，进度完全由手势决定 —— 手指往左
    /// 拖多少，控制中心就进来多少，松手前就能看见落点。
    void begin(const QPixmap& from, const QPixmap& to, Mode mode, bool forward,
               const QColor& backdrop, const QRect& origin = QRect(),
               const QPixmap& backdropImage = QPixmap());
    /// 外部驱动进度（0~1）。未 begin / 已收尾时安全忽略。
    void drive(qreal p);
    /// 松手补完：把进度补间到 target（0 = 回弹，1 = 落定）再收尾。
    /// 🔴 调用方要在 settle(1.0) **之前**先把页面栈切到目标页 —— 叠层是不透明的，
    ///    提前切完在视觉上完全看不出来，反而能保证收尾那一刻底下已经是对的画面。
    void settle(qreal target, std::function<void()> onDone);

    bool running() const { return m_running; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    qreal progress() const { return m_progress; }
    void setProgress(qreal p);
    void finish();

    /// 逐切片透视投影画一页快照。
    /// angleDeg = rotateY 角度（度，正负决定朝向）；originXRatio = 旋转轴在
    /// **这一页自身**宽度上的相对位置（0=左缘，0.5=中央，1=右缘）——
    /// 轴跟着页面走，推入到画布外时透视才不会失真；xOffset = 整体横移；
    /// scale = 整体缩放；opacity = 整体不透明度。
    /// dim = 额外压暗量（0~1，独立于 opacity）：让退后的那一页「沉进背景」，
    ///       不能靠降 opacity 实现 —— 降 alpha 会透出衬底，页面看着像在消失
    ///       而不是退远。
    /// slicesOverride > 0 时强制用这个切片数（模糊层用得上：糊了看不出台阶，
    ///       少切几片能省下大半 blit）。
    /// opaqueBacking = 先按切片形状铺一层衬底色再画快照。**新页必须开**：
    ///       页面快照是液态玻璃，本身带大量透明区，直接叠在旧页上会把旧页的
    ///       图标/文字透出来（Zoom 转场里尤其明显 —— 页面已经铺满屏了，底下
    ///       还看得见主屏图标）。按同一份切片几何铺底，边缘严丝合缝没有接缝。
    void drawPageY(QPainter& p, const QPixmap& pm, qreal angleDeg, qreal originXRatio,
                   qreal xOffset, qreal scale, qreal opacity, qreal dim = 0.0,
                   int slicesOverride = 0, qreal scaleY = 0.0, qreal yOffset = 0.0,
                   bool opaqueBacking = false);

    QPixmap m_from;
    QPixmap m_to;
    /// 旧页的模糊版（缩到 1/6 的小图，绘制时再放大 —— blit 便宜，且糊了看不出
    /// 分辨率不足）。转场开始时算一次，之后每帧只是把它按比例叠在清晰版上，
    /// 得到「逐渐失焦」而不是「突然糊掉」。
    QPixmap m_fromBlur;
    /// Mode::Zoom 的生长起点（图标矩形，叠层坐标系）。
    QRect m_origin;
    QColor m_backdrop;
    /// 壁纸快照（叠层坐标系、与叠层同尺寸）。非空时**代替纯色**当衬底：
    ///  · paintEvent 铺满它 → 转场期间背景不消失；
    ///  · drawPageY 的 opaqueBacking 按**画布坐标**切它一块铺在页面下面 →
    ///    透过新页玻璃看到的是静止的壁纸（页面在动、背景不动，物理上正确）。
    QPixmap m_backdropImage;
    Mode m_mode = Mode::Fluid;
    bool m_forward = true;
    qreal m_progress = 0.0;
    bool m_running = false;
    /// 🔴 必须是 QPointer，不能是裸指针：动画用 DeleteWhenStopped 启动，跑完会
    ///    deleteLater() 自删；裸指针此刻就悬垂了，下一次 start()/cancel() 里的
    ///    `m_anim->stop()` 是 use-after-free —— 表现为「切一次页正常、再切就闪退」
    ///    （2026-10-17 实测 exit=139）。QPointer 在对象析构时自动置空，判断即安全。
    QPointer<QPropertyAnimation> m_anim;
    std::function<void()> m_onDone;
    int m_generation = 0;
    /// 析构期间置真：动画停机时仍会发 finished，回调必须就此打住。
    bool m_shuttingDown = false;

    /// 帧耗时统计（KC_TRANSITION_STATS=1 时启用）：转场结束时打一行
    /// 「帧数 / 平均 / 最慢」。手感问题不能靠肉眼猜 —— 一帧超过 16.7ms
    /// 就掉到 60fps 以下，会明确表现为「不丝滑」。
    bool m_stats = false;
    int m_statFrames = 0;
    int m_statTicks = 0;
    double m_statTotalMs = 0.0;
    double m_statMaxMs = 0.0;
};

} // namespace kc::ui
