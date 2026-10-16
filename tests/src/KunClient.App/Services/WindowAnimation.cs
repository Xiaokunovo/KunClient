using System;
using System.Runtime.CompilerServices;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace KunFlashTool.App.Services;

/// <summary>
/// 窗口级的入场 / 退场过渡。
/// <para>
/// 🔴 <c>FadeInAsync</c> 是**幂等**的：<see cref="App"/> 给所有 <see cref="Window"/> 挂了全局入场
/// （类处理器），而部分弹窗自己也调一次 —— 不挡住就会淡两遍，观感是"闪一下"。
/// </para>
/// </summary>
public static class WindowAnimation
{
    // 兜底超时：动画 Completed 事件在窗口未渲染 / 动画被系统抑制时可能永不触发，
    // 若直接 await 会导致调用方永久挂起（主界面曾因此停在 Opacity=0 而"打不开"）。
    private const int TimeoutFudgeMs = 800;

    /// <summary>已经播过入场动画的窗口实例。同一实例第二次调用直接返回。</summary>
    private static readonly ConditionalWeakTable<Window, StrongBox<bool>> _entranceDone = new();

    /// <summary>该窗口是否已经播过入场动画（供调用方判断，不必自己记账）。</summary>
    public static bool HasPlayedEntrance(Window window)
        => window != null && _entranceDone.TryGetValue(window, out var box) && box.Value;

    public static async Task FadeInAsync(Window window, int durationMs = 250)
    {
        if (window == null) return;

        // 🔴 幂等判定必须在**第一个 await 之前**同步完成 ——
        //    否则全局类处理器与弹窗自身的调用会同时通过检查，各播一遍。
        var box = _entranceDone.GetOrCreateValue(window);
        if (box.Value) return;
        box.Value = true;

        // 缩放锚点与变换：窗口若在 XAML 里自己声明了 RenderTransform，就不动它（只做淡入），
        // 避免把别人的变换覆盖掉。
        var prevTransform = window.RenderTransform;
        ScaleTransform? scale = null;
        var canScale = prevTransform == null || prevTransform is ScaleTransform;
        if (canScale)
        {
            scale = prevTransform as ScaleTransform ?? new ScaleTransform(1, 1);
            window.RenderTransform = scale;
            window.RenderTransformOrigin = new Point(0.5, 0.5);
        }

        try
        {
            if (!window.IsLoaded) await Task.Delay(10);

            // 透明度：Silk 曲线淡入（不过冲）。
            // 缩放：0.945 → 1 带极轻微过冲 —— 窗口"长出来"后轻轻落定，是发布会/系统级窗口的观感。
            var opacity = Motion.Tween(0, 1, durationMs, Motion.SilkEase);

            window.Opacity = 0;

            var tcs = new TaskCompletionSource();
            opacity.Completed += (_, _) => tcs.TrySetResult();

            window.BeginAnimation(UIElement.OpacityProperty, opacity);
            if (scale != null)
            {
                var settle = Motion.Settle(0.945, 1, durationMs, 0.008);
                scale.ScaleX = 0.945;
                scale.ScaleY = 0.945;
                scale.BeginAnimation(ScaleTransform.ScaleXProperty, settle);
                scale.BeginAnimation(ScaleTransform.ScaleYProperty, settle);
            }

            // 动画完成或超时，二者任一先到即继续，绝不无限等待
            await Task.WhenAny(tcs.Task, Task.Delay(durationMs + TimeoutFudgeMs));
        }
        finally
        {
            // 兜底复位：无论动画是否真正推进，窗口都必须可见
            try
            {
                window.BeginAnimation(UIElement.OpacityProperty, null);
                window.Opacity = 1;
                if (scale != null)
                {
                    scale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
                    scale.BeginAnimation(ScaleTransform.ScaleYProperty, null);
                    scale.ScaleX = 1;
                    scale.ScaleY = 1;
                    window.RenderTransformOrigin = new Point(0.5, 0.5);
                }
            }
            catch { }
        }
    }

    public static async Task FadeOutAsync(Window window, int durationMs = 200)
    {
        if (!window.IsLoaded || !window.IsVisible) return;

        try
        {
            // 退场一律用 Exit 曲线（慢起快收），不做过冲 —— 退场过冲会显得"舍不得走"
            var opacity = Motion.Tween(1, 0, durationMs, Motion.ExitEase);

            var tcs = new TaskCompletionSource();
            opacity.Completed += (_, _) => tcs.TrySetResult();

            window.BeginAnimation(UIElement.OpacityProperty, opacity);

            if (window.RenderTransform is ScaleTransform st)
            {
                var shrink = Motion.Tween(1, 0.97, durationMs, Motion.ExitEase);
                st.BeginAnimation(ScaleTransform.ScaleXProperty, shrink);
                st.BeginAnimation(ScaleTransform.ScaleYProperty, shrink);
            }

            await Task.WhenAny(tcs.Task, Task.Delay(durationMs + TimeoutFudgeMs));
        }
        finally
        {
            // 兜底复位：淡出未完成也应达到终态，避免残留半透明窗口
            try
            {
                window.BeginAnimation(UIElement.OpacityProperty, null);
                window.Opacity = 0;
            }
            catch { }
        }
    }

    public static async Task CloseWithFadeAsync(Window window, int durationMs = 200)
    {
        await FadeOutAsync(window, durationMs);
        window.Close();
    }
}
