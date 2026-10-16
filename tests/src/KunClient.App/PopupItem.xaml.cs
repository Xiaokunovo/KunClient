using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace KunFlashTool.App;

public partial class PopupItem : UserControl
{
    public string Text
    {
        get => (string)GetValue(TextProperty);
        set => SetValue(TextProperty, value);
    }

    public static readonly DependencyProperty TextProperty =
        DependencyProperty.Register(nameof(Text), typeof(string), typeof(PopupItem),
            new PropertyMetadata(string.Empty, (d, e) =>
            {
                if (d is PopupItem item && e.NewValue is string s)
                    item.Label.Text = s;
            }));

    public event MouseButtonEventHandler? ItemClick;

    public PopupItem()
    {
        InitializeComponent();
        Loaded += (_, _) => Label.Text = Text;
    }

    private void OnClicked(object sender, MouseButtonEventArgs e)
    {
        ItemClick?.Invoke(this, e);
        // 关闭父 Popup
        var parent = Parent;
        while (parent != null)
        {
            if (parent is System.Windows.Controls.Primitives.Popup popup)
            {
                popup.IsOpen = false;
                break;
            }
            parent = parent is Visual v ? VisualTreeHelper.GetParent(v) : null;
        }
    }

    /// <summary>
    /// 悬停底色跟随**主题强调色**。原先是写死的 <c>#0074F0</c> ——
    /// 换主题（如现在的靛蓝）后这个蓝会和强调色打架，弹层里就这一项是「另一个蓝」。
    /// </summary>
    private void OnHover(object sender, MouseEventArgs e)
    {
        var accent = TryFindResource("Theme.Accent") as SolidColorBrush;
        var c = accent?.Color ?? Color.FromRgb(0x3D, 0x4F, 0xD1);
        Bd.Background = new SolidColorBrush(Color.FromArgb(0x18, c.R, c.G, c.B));
    }

    private void OnLeave(object sender, MouseEventArgs e) =>
        Bd.Background = Brushes.Transparent;
}
