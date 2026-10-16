using System;
using System.Windows;
using System.Windows.Media.Animation;

namespace KunFlashTool.App.Services;

/// <summary>
/// <see cref="GridLength"/> 的补间动画。
/// <para>
/// 🔴 WPF **没有内置**这个类型 —— 有 <c>DoubleAnimation</c> / <c>ThicknessAnimation</c> /
/// <c>ColorAnimation</c>，就是没有 <c>GridLengthAnimation</c>。而「展开 / 收起」类交互
/// 恰好全都落在 Grid 的行高列宽上（侧栏收放、日志抽屉开合），没有它就只能
/// <c>Width = new GridLength(72)</c> 硬跳 —— 这是本项目里最后一类"没得动画"的属性。
/// </para>
/// <para>
/// 缓动沿用 <see cref="Motion"/> 的 <see cref="KeySpline"/> 词汇（不引 <c>IEasingFunction</c>
/// 那套），这样补间曲线和全局其它动效是同一条，不会出现"侧栏和页面转场手感不一样"。
/// </para>
/// <para>
/// 只做数值插值，不碰单位类型：<c>Pixel</c> 与 <c>Star</c> 都能算，
/// 单位取 <see cref="To"/> 的单位。<c>Auto</c> 没有可比数值 —— 遇到就退化成"过半即终值"，
/// 至少不会抛异常把动画链打断。
/// </para>
/// </summary>
public sealed class GridLengthAnimation : AnimationTimeline
{
    public override Type TargetPropertyType => typeof(GridLength);

    protected override Freezable CreateInstanceCore() => new GridLengthAnimation();

    /// <summary>起点。起点单位无意义，数值按 <see cref="To"/> 的单位解释。</summary>
    public static readonly DependencyProperty FromProperty =
        DependencyProperty.Register(nameof(From), typeof(GridLength), typeof(GridLengthAnimation),
            new PropertyMetadata(new GridLength(0)));

    public GridLength From
    {
        get => (GridLength)GetValue(FromProperty);
        set => SetValue(FromProperty, value);
    }

    /// <summary>终点。动画结束后落在的值（含单位）。</summary>
    public static readonly DependencyProperty ToProperty =
        DependencyProperty.Register(nameof(To), typeof(GridLength), typeof(GridLengthAnimation),
            new PropertyMetadata(new GridLength(0)));

    public GridLength To
    {
        get => (GridLength)GetValue(ToProperty);
        set => SetValue(ToProperty, value);
    }

    /// <summary>
    /// 缓动曲线。为 null 时是线性 —— 但本项目一律传 <see cref="Motion"/> 里的曲线，
    /// 线性只作为兜底存在。
    /// </summary>
    public static readonly DependencyProperty SplineProperty =
        DependencyProperty.Register(nameof(Spline), typeof(KeySpline), typeof(GridLengthAnimation));

    public KeySpline? Spline
    {
        get => (KeySpline?)GetValue(SplineProperty);
        set => SetValue(SplineProperty, value);
    }

    public override object GetCurrentValue(object defaultOriginValue, object defaultDestinationValue,
        AnimationClock animationClock)
    {
        var from = From;
        var to = To;

        // Auto 行高没有可比数值：过半直接给终值，别去算无意义的插值。
        if (from.IsAuto || to.IsAuto)
            return (animationClock.CurrentProgress ?? 0.0) >= 0.5 ? to : from;

        // CurrentProgress 为 null = 时钟已停（被 Clear 或被系统抑制）。
        // 此时给**终值**：宁可瞬间落定，也不能把控件留在半开状态。
        var progress = animationClock.CurrentProgress;
        if (progress is null) return to;

        // KeySpline.GetSplineProgress 是 public —— 正好把 cubic-bezier 的词汇直接搬过来用，
        // 不必为了适配 IEasingFunction 再写一套贝塞尔求值。
        var eased = Spline is { } spline ? spline.GetSplineProgress(progress.Value) : progress.Value;
        if (double.IsNaN(eased)) eased = progress.Value;

        var value = from.Value + (to.Value - from.Value) * eased;
        return new GridLength(value, to.GridUnitType);
    }
}
