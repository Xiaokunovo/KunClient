using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace KunFlashTool.App.Controls;

/// <summary>
/// 参考稿底部那种「通栏进度条 + 居中百分比」控件。
/// <para>
/// 语义：<see cref="Value"/> 是 0-100 的百分比，<see cref="Text"/> 留空时按
/// <see cref="Format"/> 自动生成（默认两位小数，与参考稿的 100.00% 一致）。
/// </para>
/// </summary>
public partial class ToolProgressBar : UserControl
{
    public ToolProgressBar()
    {
        InitializeComponent();
        Sync();
        // 主题切换后 Theme.TextSecondary / Theme.ProgressText 会被整体替换，
        // 这里必须重算一次文字颜色，否则深色主题下会留着浅色主题算出来的那一支。
        ThemeManager.ThemeChanged += _ => Sync();
    }

    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(ToolProgressBar),
        new PropertyMetadata(0d, (d, _) => ((ToolProgressBar)d).Sync()));

    /// <summary>进度百分比（0-100）。</summary>
    public double Value
    {
        get => (double)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    public static readonly DependencyProperty TextProperty = DependencyProperty.Register(
        nameof(Text), typeof(string), typeof(ToolProgressBar),
        new PropertyMetadata(null, (d, _) => ((ToolProgressBar)d).Sync()));

    /// <summary>显示文本；留空则按 <see cref="Format"/> 自动生成。</summary>
    public string? Text
    {
        get => (string?)GetValue(TextProperty);
        set => SetValue(TextProperty, value);
    }

    public static readonly DependencyProperty FormatProperty = DependencyProperty.Register(
        nameof(Format), typeof(string), typeof(ToolProgressBar),
        new PropertyMetadata("0.00", (d, _) => ((ToolProgressBar)d).Sync()));

    /// <summary>自动文本的数字格式（默认 "0.00" → 100.00%）。</summary>
    public string Format
    {
        get => (string)GetValue(FormatProperty);
        set => SetValue(FormatProperty, value);
    }

    public static readonly DependencyProperty ShowTextProperty = DependencyProperty.Register(
        nameof(ShowText), typeof(bool), typeof(ToolProgressBar),
        new PropertyMetadata(true, (d, _) => ((ToolProgressBar)d).Sync()));

    /// <summary>是否显示百分比文字（细条/装饰条可以关掉）。</summary>
    public bool ShowText
    {
        get => (bool)GetValue(ShowTextProperty);
        set => SetValue(ShowTextProperty, value);
    }

    private void Sync()
    {
        if (Bar == null || Txt == null) return;

        var value = Math.Clamp(Value, 0, 100);
        Bar.Value = value;

        Txt.Visibility = ShowText ? Visibility.Visible : Visibility.Collapsed;
        Txt.Text = string.IsNullOrEmpty(Text) ? value.ToString(Format) + "%" : Text;

        // 填充过半：文字压在蓝色填充上 → 白字；否则压在浅灰轨道上 → 次级色。
        var brushKey = value >= 50 ? "Theme.ProgressText" : "Theme.TextSecondary";
        if (TryFindResource(brushKey) is Brush brush) Txt.Foreground = brush;
    }
}
