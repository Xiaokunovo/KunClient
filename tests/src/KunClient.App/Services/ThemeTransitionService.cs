using System;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace KunFlashTool.App.Services;

/// <summary>
/// 主题切换的「柔洗」过渡。
/// <para>
/// 问题：<c>ThemeManager.ApplyTheme</c> 是一次性把整套 <c>Theme.*</c> 画刷换掉，
/// 几十个键同时从浅色跳到深色 —— 这就是用户看到的「硬切」。
/// </para>
/// <para>
/// 做法：在界面上盖一层「洗色层」，用**目标主题的底色**在 200ms 内升到全屏覆盖，
/// 在完全覆盖的那一刻切换调色板（人眼看不到），再用 320ms 褪去，露出新主题。
/// 整段就像画面被柔和地"洗"了一遍，不是啪一下换掉。
/// </para>
/// <para>
/// 为什么用纯色洗而不是截图交叉淡化：动态背景用的是 MediaElement / GIF，
/// 某些驱动下截不到（会留一块黑），纯色洗对所有情况都稳定。
/// </para>
/// </summary>
public static class ThemeTransitionService
{
    /// <summary>覆盖阶段时长（升到全屏同色，遮住底下的整块替换）。</summary>
    private const int WashInMs = 200;

    /// <summary>褪去阶段时长（露出新主题）。比覆盖略长，收尾更从容。</summary>
    private const int WashOutMs = 320;

    /// <summary>
    /// 带柔洗过渡地切换主题。
    /// </summary>
    /// <param name="wash">覆盖层（需位于界面最上层、铺满、IsHitTestVisible=False）。为 null 时退化为直接切换。</param>
    /// <param name="onPeak">全屏覆盖、调色板刚换完的那一刻回调 —— 需要跟着主题换的东西（如图标字形）放这里改，改动静默。</param>
    public static async Task ToggleAnimatedAsync(Border? wash, Action? onPeak = null)
    {
        if (wash == null)
        {
            ThemeManager.Toggle();
            onPeak?.Invoke();
            return;
        }

        // 洗色用目标主题的底色：整段过渡是"朝新的样子去"，而不是"闪一下旧色"
        var destBg = ThemeManager.PeekColor(ThemeManager.NextTheme, "Theme.Bg");
        wash.Background = new SolidColorBrush(destBg);
        wash.Opacity = 0;
        wash.Visibility = Visibility.Visible;

        // 1) 升到全屏覆盖
        await Motion.PlayAsync(
            wash, UIElement.OpacityProperty,
            Motion.Tween(0, 1, WashInMs, Motion.AppleEase), WashInMs);

        // 2) 在完全被遮住的这一刻换调色板 —— 硬替换此刻不可见
        ThemeManager.Toggle();
        onPeak?.Invoke();

        // 3) 褪去洗色层，露出新主题
        await Motion.PlayAsync(
            wash, UIElement.OpacityProperty,
            Motion.Tween(1, 0, WashOutMs, Motion.SilkEase), WashOutMs);

        // 收尾：清掉动画与 Visibility，别让一层实色盖在界面上（会挡住点击与动态背景）
        Motion.Clear(wash, UIElement.OpacityProperty, 0);
        wash.Visibility = Visibility.Collapsed;
        wash.Background = null;
    }
}
