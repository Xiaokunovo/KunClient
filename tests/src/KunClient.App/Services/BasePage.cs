using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace KunFlashTool.App.Services;

/// <summary>
/// 所有页面的基类：提供入场动画、页面内通知条（NotifyBar）与日志框（TxtLog）的统一访问。
/// </summary>
public class BasePage : UserControl
{
    /// <summary>
    /// 通知条容器。⚠️ 只能在 UI 线程读：<c>FindName</c> 是 DispatcherObject 访问，
    /// 线程池线程上会抛「调用线程无法访问此对象…」。跨线程请用 <see cref="ShowSuccess"/> /
    /// <see cref="ShowError"/> / <see cref="HideNotify"/>（它们内部已切 UI 线程）。
    /// </summary>
    protected Border? NotifyBorder => FindName("NotifyBar") as Border;

    /// <summary>同上：仅 UI 线程可读。</summary>
    protected TextBox? LogBox => FindName("TxtLog") as TextBox;

    /// <summary>
    /// 日志目标：页面里的 TxtLog 可能是 TextBox（纯文本）也可能是 RichTextBox（按级别着色）。
    /// 统一交给 PageLog 分派，页面就不用关心自己用的是哪种日志框。
    /// <para>⚠️ 只读属性本身**要求 UI 线程**（内部是 <c>FindName</c>）。页面代码请一律走
    /// <see cref="LogInfo"/> 家族，别在后台回调里直接读它。</para>
    /// </summary>
    protected object? LogTarget => FindName("TxtLog") switch
    {
        RichTextBox rich => rich,
        TextBox text => text,
        _ => null
    };

    public BasePage()
    {
        Loaded += OnBasePageLoaded;
    }

    private void OnBasePageLoaded(object sender, RoutedEventArgs e)
    {
        // 折叠区补间挂在基类上：所有页面都继承 BasePage，这里挂一次就全覆盖
        // （样式是共享的，没法在 ToolFold 上用 EventSetter —— Handler 得写在每个页面里）。
        FoldAnimator.Attach(this);

        // ⚠️ 入场淡入**不在这里播**：外壳统一在 MainWindow.MainTabs_SelectionChanged /
        // MainWindow_Loaded 里调 PageTransitionService.PlayEntranceAsync（带 token 防重入、
        // 结束后把 Opacity 归位到 1）。原来这里还有第二层 140ms 淡入打在 Content 上 ——
        // 同一棵子树两层 Opacity 同时补间 = 每帧两次合成重绘，两段曲线叠乘后入场还比设计值慢一截，
        // 连点页签时两层动画互相抢同一条属性（旧的还没停、新的又起）。已并回外壳那一次。
    }

    /// <summary>
    /// 把一段「读 / 写本页可视元素」的动作排到本页 Dispatcher 上。
    /// <para>🔴 2026-10-08 修复（用户反馈「下载出错：调用线程无法访问此对象，因为另一个线程拥有该对象」）：
    /// 页面里到处是「后台线程回调 → 直接 LogInfo」的路径 —— <c>PlatformToolsManager</c> 的
    /// <c>_log</c> 回调在 <c>ConfigureAwait(false)</c> 之后触发（下载/解压/失败三个阶段都在
    /// 线程池上）、<c>BackgroundDownloader</c> 的进度回调同理。而 <see cref="LogTarget"/> /
    /// <see cref="NotifyBorder"/> 内部是 <c>FrameworkElement.FindName</c>，跨线程读直接抛
    /// <c>InvalidOperationException</c>。实测（net10.0-windows）：<c>FindName</c> 与
    /// <c>IsLoaded</c> 在后台线程上都抛同一条消息，<c>IsVisible</c> 不抛。</para>
    /// <para>PageLog / Notify 内部**已经**会把日志汇回 UI 线程，但它们拿到的是外面先算出来的
    /// 日志框对象 —— 也就是崩溃发生在「切线程」之前那一行。收口在这里，所有页面一次性覆盖。</para>
    /// <para>UI 线程调用时 <c>CheckAccess()</c> 为真，直接同步执行，零额外开销。</para>
    /// </summary>
    private void OnUiThread(Action action)
    {
        if (Dispatcher.CheckAccess()) action();
        else Dispatcher.BeginInvoke(action);
    }

    /// <summary>日志写入统一收口：先切 UI 线程，再解析日志框，再落笔。</summary>
    private void WriteLog(Action<object> write)
    {
        if (Dispatcher.CheckAccess())
        {
            if (LogTarget is { } target) write(target);
            return;
        }

        Dispatcher.BeginInvoke(new Action(() =>
        {
            if (LogTarget is { } target) write(target);
        }));
    }

    protected void ShowSuccess(string message, int autoHideMs = 4000)
    {
        OnUiThread(() =>
        {
            if (NotifyBorder is { } border) Notify.Show(border, message, isError: false, autoHideMs);
        });
    }

    protected void ShowError(string message, int autoHideMs = 5000)
    {
        OnUiThread(() =>
        {
            if (NotifyBorder is { } border) Notify.Show(border, message, isError: true, autoHideMs);
        });
    }

    protected void HideNotify()
    {
        OnUiThread(() =>
        {
            if (NotifyBorder is { } border) Notify.Hide(border);
        });
    }

    protected void LogInfo(string message) => WriteLog(t => PageLog.Info(t, message));

    protected void LogSuccess(string message) => WriteLog(t => PageLog.Success(t, message));

    protected void LogWarning(string message) => WriteLog(t => PageLog.Warning(t, message));

    protected void LogError(string message) => WriteLog(t => PageLog.Error(t, message));

    /// <summary>
    /// 「搜索分区」胶囊按钮的开关行为（三个页签共用）。
    /// <para>
    /// 参考稿里这个位置只有**一枚右对齐的胶囊按钮**，没有常驻输入框。所以这里把输入区默认收起，
    /// 点按钮才展开并聚焦；再点一次收起、同时清空过滤词（否则用户会以为表格"少了行"）。
    /// 输入框本身还是页面里那个 <c>x:Name</c> + <c>TextChanged</c>，过滤逻辑一行没动。
    /// </para>
    /// 按钮的 <c>Tag</c> 写它要开关的**容器名**：既可以是 TextBox 本身，也可以是「输入框 + 水印」
    /// 这类整块容器（EDL 页的水印 TxtPartitionSearchHint 由代码按输入内容控制显隐，
    /// 只藏输入框会留下一行孤零零的"搜索分区"提示文字）。
    /// </summary>
    protected void SearchToggle_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button btn || btn.Tag is not string target) return;
        if (FindName(target) is not FrameworkElement box) return;

        if (box.Visibility == Visibility.Visible)
        {
            box.Visibility = Visibility.Collapsed;
            // 清空 = 取消过滤，表格恢复全量；容器里若有 TextBox 就一并清掉
            foreach (var tb in Descendants(box).OfType<TextBox>())
                tb.Text = string.Empty;
        }
        else
        {
            box.Visibility = Visibility.Visible;
            var text = Descendants(box).OfType<TextBox>().FirstOrDefault() ?? box as TextBox;
            text?.Focus();
            text?.SelectAll();
        }
    }

    /// <summary>可视树 + 逻辑树双向下钻（容器里可能还没生成可视子元素，逻辑树更稳）。</summary>
    private static System.Collections.Generic.IEnumerable<DependencyObject> Descendants(DependencyObject root)
    {
        var stack = new System.Collections.Generic.Stack<DependencyObject>();
        stack.Push(root);
        while (stack.Count > 0)
        {
            var cur = stack.Pop();
            yield return cur;
            if (cur is System.Windows.Media.Visual || cur is System.Windows.Media.Media3D.Visual3D)
            {
                var n = System.Windows.Media.VisualTreeHelper.GetChildrenCount(cur);
                for (var i = 0; i < n; i++) stack.Push(System.Windows.Media.VisualTreeHelper.GetChild(cur, i));
            }
            foreach (var child in LogicalTreeHelper.GetChildren(cur))
                if (child is DependencyObject d) stack.Push(d);
        }
    }
}