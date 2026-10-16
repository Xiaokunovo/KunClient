using System.Windows;
using System.Windows.Controls;

namespace KunFlashTool.App.Controls;

/// <summary>
/// 统一的页面标题栏（品牌图标 + 标题 + 副标题 + 右侧动作区）。
/// 各页面用它保持一致的外观，取代此前「有的页面写两行标题 TextBlock、有的干脆没有」的散乱做法。
/// </summary>
public partial class PageHeaderBar : UserControl
{
    public PageHeaderBar()
    {
        InitializeComponent();
        // ★ 必须在这里把两个文本同步一次。
        //   DependencyProperty 的回调只在「值发生变化」时触发；这里做一次初始同步即可。
        TitleText.Text = Title;
        SubtitleText.Text = Subtitle;
    }

    public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(
        nameof(Title), typeof(string), typeof(PageHeaderBar),
        new PropertyMetadata(string.Empty, (d, e) => ((PageHeaderBar)d).TitleText.Text = e.NewValue as string ?? string.Empty));

    /// <summary>页面主标题，如「EDL 刷写」。</summary>
    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public static readonly DependencyProperty SubtitleProperty = DependencyProperty.Register(
        nameof(Subtitle), typeof(string), typeof(PageHeaderBar),
        new PropertyMetadata(string.Empty, (d, e) => ((PageHeaderBar)d).SubtitleText.Text = e.NewValue as string ?? string.Empty));

    /// <summary>副标题（一行说明），为空时那一行会自然收缩为 0 高。</summary>
    public string Subtitle
    {
        get => (string)GetValue(SubtitleProperty);
        set => SetValue(SubtitleProperty, value);
    }

    /// <summary>
    /// 保留但**不再渲染**：标题栏左侧的「品牌图标方块」已按需求整块移除
    /// （那就是残留的 logo 标记——32x32 圆角方块 + 闪电字形）。
    /// 之所以不删掉这个依赖属性，是为了让既有的 14 个页面里
    /// <c>Glyph="&#xE945;"</c> 之类的写法继续可编译，避免大面积改动引入风险。
    /// </summary>
    public static readonly DependencyProperty GlyphProperty = DependencyProperty.Register(
        nameof(Glyph), typeof(string), typeof(PageHeaderBar),
        new PropertyMetadata("\uE8A5"));

    /// <summary>历史遗留：Segoe MDL2 字形。当前不参与渲染，仅保留以兼容既有页面。</summary>
    public string Glyph
    {
        get => (string)GetValue(GlyphProperty);
        set => SetValue(GlyphProperty, value);
    }

    /// <summary>右侧动作区内容（按钮等）。</summary>
    public static readonly DependencyProperty ActionsProperty = DependencyProperty.Register(
        nameof(Actions), typeof(object), typeof(PageHeaderBar),
        new PropertyMetadata(null, (d, e) => ((PageHeaderBar)d).ActionsHost.Content = e.NewValue));

    /// <summary>
    /// 右侧动作区内容（按钮等）。
    /// 必须是 DependencyProperty：页面用属性元素语法 &lt;ctrl:PageHeaderBar.Actions&gt; 挂内容，
    /// 普通 CLR 属性在 XAML 里虽然也能赋值，但拿不到变更通知，复用/切换时容易不刷新。
    /// </summary>
    public object? Actions
    {
        get => GetValue(ActionsProperty);
        set => SetValue(ActionsProperty, value);
    }
}
