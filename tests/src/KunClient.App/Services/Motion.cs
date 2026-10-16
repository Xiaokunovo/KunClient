using System;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace KunFlashTool.App.Services;

/// <summary>
/// 「发布会风格」动效参数与动画工厂 —— 全局唯一缓动来源，避免各页面各写一套曲线。
/// <para>三个特征：</para>
/// <list type="number">
///   <item>临界阻尼曲线：先快后极缓，落点无生硬停帧；</item>
///   <item>位移/缩放带极轻微过冲后落定（"落定感"），不是线性刹停；</item>
///   <item>以淡入淡出 + 微缩放表达层次，位移量刻意做小（≤16px），不做夸张横移。</item>
/// </list>
/// <para>
/// 所有 await 都带超时兜底：动画 Completed 在窗口未渲染 / 被系统抑制时可能永不触发，
/// 直接 await 会让调用方永久挂起（历史 bug：主界面停在 Opacity=0 而"打不开"）。
/// </para>
/// </summary>
public static class Motion
{
    /// <summary>兜底超时增量：动画时长之外再给这么多毫秒，超过就强行继续。</summary>
    public const int TimeoutFudgeMs = 800;

    /// <summary>Apple 转场曲线 cubic-bezier(0.32, 0.72, 0, 1) —— 标准"丝滑"，用于位移。</summary>
    public static readonly KeySpline AppleEase = new(0.32, 0.72, 0, 1);

    /// <summary>Silk 曲线 cubic-bezier(0.16, 1, 0.3, 1) —— 先快后极缓，用于淡入。</summary>
    public static readonly KeySpline SilkEase = new(0.16, 1, 0.3, 1);

    /// <summary>退场曲线 cubic-bezier(0.4, 0, 0.6, 1) —— 慢起快收，用于淡出/退场。</summary>
    public static readonly KeySpline ExitEase = new(0.4, 0, 0.6, 1);

    /// <summary>落定曲线 cubic-bezier(0.2, 0.9, 0.3, 1) —— 过冲后的回落段。</summary>
    public static readonly KeySpline SettleEase = new(0.2, 0.9, 0.3, 1);

    /// <summary>
    /// 单段关键帧：从 <paramref name="from"/> 平滑到 <paramref name="to"/>。
    /// 首帧显式写 <paramref name="from"/>，保证动画一定从"当前可见值"出发，不受上一次残留影响。
    /// </summary>
    public static DoubleAnimationUsingKeyFrames Tween(double from, double to, int ms, KeySpline spline)
    {
        var anim = new DoubleAnimationUsingKeyFrames();
        anim.KeyFrames.Add(new SplineDoubleKeyFrame(from, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        anim.KeyFrames.Add(new SplineDoubleKeyFrame(to, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(ms)), spline));
        return anim;
    }

    /// <summary>
    /// 带轻微过冲的「落定」动画：先冲到略过目标的位置，再回落收住。
    /// <para>
    /// **只可用于缩放/位移** —— 透明度绝不能过冲（&gt;1 或 &lt;0 都是非法值，会直接抛异常）。
    /// </para>
    /// </summary>
    public static DoubleAnimationUsingKeyFrames Settle(double from, double to, int ms, double overshoot = 0.02)
    {
        var delta = to - from;
        var peak = to + delta * overshoot;

        var anim = new DoubleAnimationUsingKeyFrames();
        anim.KeyFrames.Add(new SplineDoubleKeyFrame(from, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        anim.KeyFrames.Add(new SplineDoubleKeyFrame(
            peak, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(ms * 0.72)), AppleEase));
        anim.KeyFrames.Add(new SplineDoubleKeyFrame(
            to, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(ms)), SettleEase));
        return anim;
    }

    /// <summary>启动动画并等待结束（含超时兜底）。</summary>
    public static async Task PlayAsync(IAnimatable target, DependencyProperty property, AnimationTimeline anim, int fallbackMs)
    {
        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        anim.Completed += (_, _) => tcs.TrySetResult();

        target.BeginAnimation(property, anim);
        await Task.WhenAny(tcs.Task, Task.Delay(fallbackMs + TimeoutFudgeMs));
    }

    /// <summary>
    /// 清掉动画（并把属性落到终值，可选）。
    /// 用完一定要清：留着 HoldEnd 的动画值会让后续直接赋值被动画覆盖。
    /// </summary>
    public static void Clear(IAnimatable target, DependencyProperty property, double? finalValue = null)
    {
        try
        {
            target.BeginAnimation(property, null);
            if (finalValue.HasValue && target is DependencyObject dobj)
                dobj.SetValue(property, finalValue.Value);
        }
        catch { }
    }

    /// <summary>
    /// <see cref="Clear"/> 的任意值版本 —— <c>GridLength</c>（列宽/行高）不是 double，
    /// 用不了上面那个重载。语义完全一致：摘动画 + 落终值。
    /// </summary>
    public static void ClearTo(IAnimatable target, DependencyProperty property, object? finalValue = null)
    {
        try
        {
            target.BeginAnimation(property, null);
            if (finalValue != null && target is DependencyObject dobj)
                dobj.SetValue(property, finalValue);
        }
        catch { }
    }

    // ─────────────────────────────────────────────────────────────────────
    //  「出现 / 消失」原语
    //  ─────────────────────────────────────────────────────────────────────
    //  项目里所有「展开 → 收起」「显示 → 隐藏」过去都是一行 Visibility 赋值，
    //  也就是硬切。下面两个方法把这类切换统一成「淡 + 微位移」，
    //  位移量刻意做小（默认 10px）：够表达方向，又不会把界面推得晃。
    //  ─────────────────────────────────────────────────────────────────────

    /// <summary>
    /// 每个元素一个递增令牌：同一元素上快速连续触发 显示/隐藏 时，
    /// 旧的那次必须发现自己"已被顶替"，否则它的复位会把新动画正在播的状态强行写成终态（闪一下）。
    /// 与 <c>PageTransitionService</c> 里的机制同源，只是这里服务的是通用元素。
    /// </summary>
    private static readonly ConditionalWeakTable<FrameworkElement, StrongBox<long>> _revealTokens = new();

    private static long NextRevealToken(FrameworkElement element)
    {
        var box = _revealTokens.GetOrCreateValue(element);
        return ++box.Value;
    }

    private static bool IsRevealCurrent(FrameworkElement element, long token)
        => _revealTokens.TryGetValue(element, out var box) && box.Value == token;

    /// <summary>
    /// 平滑显现元素：淡入 + 从 <paramref name="slideFrom"/> 滑到 0。
    /// <para>默认从**上方** 10px 滑下（负值）—— 展开类元素从"被压进去的方向"长出来最自然。</para>
    /// </summary>
    public static async Task RevealAsync(FrameworkElement element, int durationMs = 240,
        double slideFrom = -10, int delayMs = 0)
    {
        if (element == null) return;

        // 已经在完全可见状态：只保证终态，不重播 ——
        // 同一个提示条被连续 Show 两次（内容更新）时不会闪一下。
        if (element.Visibility == Visibility.Visible && element.Opacity >= 0.999 && delayMs <= 0)
        {
            element.Opacity = 1;
            return;
        }

        await SlideFadeCoreAsync(element, 0, 1, slideFrom, 0, durationMs, SilkEase, delayMs,
            collapseAfter: false);
    }

    /// <summary>
    /// 平滑隐藏元素：淡出 + 滑向 <paramref name="slideTo"/>，结束后按需折叠。
    /// </summary>
    public static Task ConcealAsync(FrameworkElement element, int durationMs = 160,
        double slideTo = -10, int delayMs = 0, bool collapseAfter = true)
    {
        if (element == null) return Task.CompletedTask;
        if (element.Visibility != Visibility.Visible) return Task.CompletedTask;

        return SlideFadeCoreAsync(element, 1, 0, 0, slideTo, durationMs, ExitEase, delayMs, collapseAfter);
    }

    private static async Task SlideFadeCoreAsync(FrameworkElement element, double fromOpacity, double toOpacity,
        double fromY, double toY, int ms, KeySpline spline, int delayMs, bool collapseAfter)
    {
        var token = NextRevealToken(element);

        // 🔴 变换必须**按元素复用同一个实例**，收尾时也还原到**最初捕获**的那个，而不是"这次调用进来时读到的那个"。
        //    原因：显示/隐藏快速交叠时，旧的那次会在令牌判定处提前 return（不回写），
        //    若它每次都 new 一个 TranslateTransform，新那次读到的"上一个变换"就是旧那次装的临时对象 ——
        //    于是收尾把**旧临时对象**还给了元素，它上面还挂着 -10px 的残留位移，元素从此永久偏移。
        //    复用实例 + 只认初次捕获的基值，两个问题一起消掉。
        var st = _slideStates.GetValue(element, _ => new SlideState());
        if (!st.Captured)
        {
            st.BaseTransform = element.RenderTransform;
            st.BaseOrigin = element.RenderTransformOrigin;
            st.BaseY = (element.RenderTransform as TranslateTransform)?.Y ?? 0;
            st.Slide = element.RenderTransform as TranslateTransform ?? new TranslateTransform(0, 0);
            st.Captured = true;
        }

        // 🔴 st.Slide 在上方首次捕获分支里已被赋值；若 st.Captured 早为 true，则上一轮已写过非 null。
        //    编译器推不出这个不变量，故用 ! 断言（此处不可能为 null，否则上面的捕获分支没走完）。
        var slide = st.Slide!;
        element.Visibility = Visibility.Visible;
        element.RenderTransform = slide;
        element.RenderTransformOrigin = new Point(0.5, 0.5);

        // 🔴 必须把**基值**也设成起点：带 BeginTime 的动画在延迟期间不参与计算，
        //    这段时间读到的就是基值。只写关键帧 From 的话，延迟期内元素会以原样（通常是不透明）
        //    先亮一下再开始淡入 —— 慢半拍的"闪一下"就是这么来的。
        element.Opacity = fromOpacity;
        slide.Y = fromY;

        var begin = delayMs > 0 ? TimeSpan.FromMilliseconds(delayMs) : (TimeSpan?)null;
        var fade = Tween(fromOpacity, toOpacity, ms, spline);
        var glide = Tween(fromY, toY, ms, spline);
        if (begin.HasValue) { fade.BeginTime = begin; glide.BeginTime = begin; }

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        fade.Completed += (_, _) => tcs.TrySetResult();

        element.BeginAnimation(UIElement.OpacityProperty, fade);
        slide.BeginAnimation(TranslateTransform.YProperty, glide);

        await Task.WhenAny(tcs.Task, Task.Delay(delayMs + ms + TimeoutFudgeMs));

        // 已被更新的动画顶替 → 状态归它管，这里绝不回写
        if (!IsRevealCurrent(element, token)) return;

        element.BeginAnimation(UIElement.OpacityProperty, null);
        slide.BeginAnimation(TranslateTransform.YProperty, null);
        slide.Y = ReferenceEquals(slide, st.BaseTransform) ? st.BaseY : toY;
        element.RenderTransform = st.BaseTransform;
        element.RenderTransformOrigin = st.BaseOrigin;

        if (collapseAfter)
        {
            element.Opacity = 1;                      // 复位成"下次直接赋值也看得见"
            element.Visibility = Visibility.Collapsed;
        }
        else
        {
            element.Opacity = toOpacity;
        }
    }

    /// <summary>元素自己的变换（基值）与我们临时装上去的位移变换，每个元素记一份。</summary>
    private sealed class SlideState
    {
        public Transform? BaseTransform;
        public Point BaseOrigin;
        public double BaseY;
        public TranslateTransform? Slide;
        public bool Captured;
    }

    private static readonly ConditionalWeakTable<FrameworkElement, SlideState> _slideStates = new();

    // ─────────────────────────────────────────────────────────────────────
    //  Grid 布局属性补间（侧栏列宽 / 抽屉行高）
    //  ─────────────────────────────────────────────────────────────────────

    /// <summary>
    /// 补间一个 <c>ColumnDefinition.Width</c> / <c>RowDefinition.Height</c>。
    /// <para>结束时必定摘掉动画并落终值 —— 留着 HoldEnd 的动画会让后续
    /// <c>Width = new GridLength(...)</c> 静默失效（这正是"改了没反应"类 bug 的温床）。</para>
    /// </summary>
    public static async Task AnimateGridLengthAsync(IAnimatable target, DependencyProperty property,
        double from, double to, GridUnitType unit, int ms, KeySpline spline)
    {
        if (target == null) return;

        var anim = new GridLengthAnimation
        {
            From = new GridLength(from, unit),
            To = new GridLength(to, unit),
            Spline = spline,
            FillBehavior = FillBehavior.HoldEnd,
        };

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        anim.Completed += (_, _) => tcs.TrySetResult();

        try { target.BeginAnimation(property, anim); }
        catch { ClearTo(target, property, new GridLength(to, unit)); return; }

        await Task.WhenAny(tcs.Task, Task.Delay(ms + TimeoutFudgeMs));
        ClearTo(target, property, new GridLength(to, unit));
    }

    /// <summary>补间一个 double 依赖属性（如 <c>MinHeight</c>、<c>Height</c>）。返回时已落终值并摘掉动画。</summary>
    public static async Task AnimateDoubleAsync(IAnimatable target, DependencyProperty property,
        double from, double to, int ms, KeySpline spline)
    {
        if (target == null) return;

        var anim = Tween(from, to, ms, spline);
        anim.FillBehavior = FillBehavior.HoldEnd;

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        anim.Completed += (_, _) => tcs.TrySetResult();

        try { target.BeginAnimation(property, anim); }
        catch { Clear(target, property, to); return; }

        await Task.WhenAny(tcs.Task, Task.Delay(ms + TimeoutFudgeMs));
        Clear(target, property, to);
    }
}
