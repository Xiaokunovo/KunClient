using System;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace KunFlashTool.App.Services;

/// <summary>
/// 页面过渡动画 - 发布会式（Apple keynote）转场：旧页轻微下沉淡出，新页带微缩放"落定"式淡入。
/// <para>
/// 苹果发布会转场的观感来自三点：位移很小但方向明确、缩放带极轻微过冲后落定、
/// 缓动是临界阻尼（先快后极缓）。这里全部复用 <see cref="Motion"/>，不自己写曲线。
/// </para>
/// </summary>
public static class PageTransitionService
{
    /// <summary>
    /// 每个元素一个递增令牌：快速连点导航时，旧的那次过渡必须发现自己"已被顶替"，
    /// 否则它的 finally 复位会把新过渡正在播的半透明/半缩放状态强行写成终态（闪一下）。
    /// </summary>
    private static readonly ConditionalWeakTable<UIElement, StrongBox<long>> _tokens = new();

    private static long NextToken(UIElement element)
    {
        var box = _tokens.GetOrCreateValue(element);
        return ++box.Value;
    }

    private static bool IsCurrent(UIElement element, long token)
        => _tokens.TryGetValue(element, out var box) && box.Value == token;

    /// <summary>
    /// 带过渡动画地切换页面内容。
    /// <para><paramref name="durationMs"/> 是「总预算」：退场占 32%、入场占 68%。
    /// 退场必须比入场快 —— 观众不想看旧页面走，只想看新页面来。</para>
    /// </summary>
    public static async Task TransitionToAsync(ContentControl host, UserControl newPage, int durationMs = 440)
    {
        if (host == null || newPage == null) return;

        // 🔴 环境护栏：正常运行期 Dispatcher 消息循环一定在，await 续体会回到 UI 线程；
        //    但在探针 / 单元测试里可能没有 SynchronizationContext，续体就落到**线程池** →
        //    后面碰 host.Content / 动画属性时直接抛「调用线程无法访问此对象」，
        //    而且它是 async void 链条上的未观察异常，会直接把进程带走（实测 nav_shot_probe 就是这么崩的）。
        //    统一把切换动作排到 host 自己的 Dispatcher 上，两种环境都安全；UI 线程调用时 CheckAccess 为真，零开销。
        if (!host.Dispatcher.CheckAccess())
        {
            await host.Dispatcher.InvokeAsync(() => TransitionToAsync(host, newPage, durationMs)).Task.Unwrap();
            return;
        }

        var exitMs = Math.Max(80, (int)Math.Round(durationMs * 0.32));
        var enterMs = Math.Max(140, durationMs - exitMs);

        var oldContent = host.Content as UIElement;

        // 没有旧内容（首屏）：直接上新页面 + 入场动画
        if (oldContent == null)
        {
            host.Content = newPage;
            await AnimateEntranceAsync(newPage, enterMs);
            return;
        }

        // 1. 旧内容淡出 + 轻微下沉/缩小（营造"向后退出"的层次）
        await AnimateExitAsync(oldContent, exitMs);

        // 2. 替换内容
        host.Content = newPage;

        // 3. 新内容淡入 + 轻微上浮/放大（"向前推入"），带落定过冲
        await AnimateEntranceAsync(newPage, enterMs);
    }

    /// <summary>入场：淡入 + 从下方轻微浮起 + 缩放 0.978 → 1（带过冲落定）。</summary>
    private static async Task AnimateEntranceAsync(UIElement element, int durationMs)
    {
        if (element is FrameworkElement fe && !fe.IsLoaded)
        {
            await Task.Delay(10);
        }

        var token = NextToken(element);

        // ⚠️ 工具箱版式（参考稿）下页签切换只做**淡入**，不做位移/缩放：
        //    那是卡片式落地页的动效语言，落在「分组框 + 表格」的工具面板上会让人误以为
        //    表格在飘；而且切页签本来就是高频操作，短淡入（140ms）更贴近桌面工具的手感。
        var fadeIn = Motion.Tween(0, 1, Math.Min(durationMs, 140), Motion.SilkEase);

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        fadeIn.Completed += (_, _) => tcs.TrySetResult();

        element.BeginAnimation(UIElement.OpacityProperty, fadeIn);

        await Task.WhenAny(tcs.Task, Task.Delay(durationMs + Motion.TimeoutFudgeMs));

        // 已被更新的过渡顶替 -> 状态归它管，这里绝不回写，否则会闪一下
        if (!IsCurrent(element, token)) return;

        // 无论动画是否真正推进完，都落到"完整可见"的终态：
        // 否则一旦被系统抑制，新页面会以半透明的样子停在屏幕上。
        Motion.Clear(element, UIElement.OpacityProperty, 1);
    }

    /// <summary>退场：淡出 + 轻微下沉 + 缩小到 0.986（"向后退出"）。</summary>
    private static async Task AnimateExitAsync(UIElement element, int durationMs)
    {
        if (element is FrameworkElement fe && (!fe.IsLoaded || !fe.IsVisible)) return;

        var token = NextToken(element);

        var slide = new TranslateTransform(0, 0);
        var scale = new ScaleTransform(1, 1);
        var group = new TransformGroup();
        group.Children.Add(scale);
        group.Children.Add(slide);

        var prevTransform = element.RenderTransform;
        var prevOrigin = element.RenderTransformOrigin;

        element.RenderTransform = group;
        element.RenderTransformOrigin = new Point(0.5, 0.35);

        var fadeOut = Motion.Tween(1, 0, durationMs, Motion.ExitEase);
        var slideX = Motion.Tween(0, -14, durationMs, Motion.ExitEase);
        var slideY = Motion.Tween(0, -8, durationMs, Motion.ExitEase);
        var scaleX = Motion.Tween(1, 0.986, durationMs, Motion.ExitEase);
        var scaleY = Motion.Tween(1, 0.986, durationMs, Motion.ExitEase);

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        fadeOut.Completed += (_, _) => tcs.TrySetResult();

        element.BeginAnimation(UIElement.OpacityProperty, fadeOut);
        slide.BeginAnimation(TranslateTransform.XProperty, slideX);
        slide.BeginAnimation(TranslateTransform.YProperty, slideY);
        scale.BeginAnimation(ScaleTransform.ScaleXProperty, scaleX);
        scale.BeginAnimation(ScaleTransform.ScaleYProperty, scaleY);

        // 与入场一致：加超时兜底，避免退场等待卡死阻塞后续页面切换
        await Task.WhenAny(tcs.Task, Task.Delay(durationMs + Motion.TimeoutFudgeMs));

        if (!IsCurrent(element, token)) return;

        ClearTransformAnimations(slide, scale);
        Motion.Clear(element, UIElement.OpacityProperty, 0);
        element.RenderTransform = prevTransform;
        element.RenderTransformOrigin = prevOrigin;
    }

    /// <summary>
    /// 把页面的一级子面板依次淡入（40ms 步进，最多排 6 个）。
    /// <para>
    /// 页面结构统一是 <c>BasePage &gt; Grid &gt; 各功能面板</c>，所以取 <c>Content as Panel</c> 的
    /// 直接子元素就能拿到「页头 / 横带 / 主区 / 日志 / 状态栏」这些行 —— 它们正是用户感知里的"几块内容"。
    /// 不递归下钻：子面板内部的控件各自有自己的显隐逻辑，乱动会打架。
    /// </para>
    /// </summary>
    private static void StaggerPanels(UIElement element)
    {
        if (element is not ContentControl cc || cc.Content is not Panel panel) return;

        var shown = new System.Collections.Generic.List<FrameworkElement>();
        foreach (var child in panel.Children)
        {
            if (child is not FrameworkElement fe) continue;
            if (fe.Visibility != Visibility.Visible) continue;
            if (fe.Opacity < 0.999) continue;      // 有自己初始透明度的，别抢
            shown.Add(fe);
            if (shown.Count >= 6) break;           // 排太多会拖沓
        }

        for (int i = 0; i < shown.Count; i++)
        {
            // 不 await：级联是并行起跑的，靠 delay 错峰；不阻塞页面过渡的主时序。
            _ = Motion.RevealAsync(shown[i], durationMs: 240, slideFrom: 8, delayMs: 30 + i * 40);
        }
    }

    /// <summary>清掉临时 transform 上的动画（清完即回到构造值：位移 0 / 缩放 1，也就是恒等变换）。</summary>
    private static void ClearTransformAnimations(TranslateTransform slide, ScaleTransform scale)    {
        Motion.Clear(slide, TranslateTransform.XProperty);
        Motion.Clear(slide, TranslateTransform.YProperty);
        Motion.Clear(scale, ScaleTransform.ScaleXProperty);
        Motion.Clear(scale, ScaleTransform.ScaleYProperty);
    }

    /// <summary>首屏 / 主窗入场：复用入场动画，供 MainWindow 加载后直接播放。</summary>
    public static Task PlayEntranceAsync(UIElement element, int durationMs = 420)
    {
        if (element == null) return Task.CompletedTask;
        return AnimateEntranceAsync(element, durationMs);
    }
}
