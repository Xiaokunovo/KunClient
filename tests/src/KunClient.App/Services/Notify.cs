using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Threading;

namespace KunFlashTool.App.Services;

/// <summary>
/// 内联通知帮助类 - 替代MessageBox
/// <para>
/// 出现/消失都走 <see cref="Motion"/> 的淡入上滑：过去是 <c>Visibility = Visible</c>
/// 一行硬切，提示条会"啪"地蹦出来；现在从上方 10px 滑落淡入，
/// 自动隐藏时反向滑走再折叠。
/// </para>
/// </summary>
public static class Notify
{
    // 2026-10-02 修复：自动隐藏计时器改为**按容器**各挂各的（存进 _timers 字典）。
    // 原先全应用共用一个 static _activeTimer —— A 页提示条还在倒计时、B 页又弹一条时，
    // A 的计时器被顶掉，A 的提示条从此**永远停在屏幕上**（页面按页签缓存，切回来它还在）。
    private static readonly System.Collections.Generic.Dictionary<Border, DispatcherTimer> _timers = new();

    private static DispatcherTimer? TakeTimer(Border container)
    {
        if (_timers.TryGetValue(container, out var t))
        {
            t.Stop();
            _timers.Remove(container);
            return t;
        }
        return null;
    }

    /// <summary>出现动画时长（与页面转场同一族曲线，手感一致）。</summary>
    private const int RevealMs = 240;

    /// <summary>消失动画时长。退场必须比入场快 —— 和页面转场同一个原则。</summary>
    private const int ConcealMs = 160;

    public static void Show(Border container, string message, bool isError = false, int autoHideMs = 4000)
    {
        if (container == null) return;

        // 取消这个容器**自己**的隐藏计时器（不影响别的页面的提示条，见 _timers 注释）
        TakeTimer(container);

        // 判断是不是「已经在展示中」——必须在动 Visibility/Opacity 之前读，
        // 否则下面的分支永远走不到（这正是第一次实现会漏掉动画的坑）。
        var alreadyShown = container.Visibility == Visibility.Visible && container.Opacity >= 0.999;

        container.Visibility = Visibility.Visible;
        // 新出现：先落到透明，交给 RevealAsync 淡入；已在展示：只换内容，不重播（避免闪一下）
        container.Opacity = alreadyShown ? 1 : 0;

        // ⚠️ 底色/描边/文字色一律取**主题令牌**，不再写死 Tailwind 那套浅绿浅红：
        //    写死的话，页面 XAML 里刚设好的 Theme.InfoBg / Theme.Info 会被这里当场覆盖，
        //    深色主题下更会直接糊出一块浅色补丁（用户反馈过「提示条不跟着主题变」）。
        var backgroundKey = isError ? "Theme.DangerBg" : "Theme.SuccessBg";
        var borderKey = isError ? "Theme.Danger" : "Theme.Success";
        var textKey = isError ? "Theme.Danger" : "Theme.Success";

        container.Background = ThemeBrush(backgroundKey);
        container.BorderBrush = ThemeBrush(borderKey);

        if (container.Child is StackPanel sp)
        {
            foreach (var child in sp.Children)
            {
                if (child is TextBlock tb && tb.Name == "NotifyIcon")
                {
                    tb.Text = isError ? "\u2717" : "\u2713";
                    tb.Foreground = ThemeBrush(textKey);
                }
                else if (child is TextBlock tbMsg && tbMsg.Name == "NotifyText")
                {
                    tbMsg.Text = message;
                    tbMsg.Foreground = ThemeBrush(textKey);
                }
            }
        }

        if (!alreadyShown)
            _ = Motion.RevealAsync(container, RevealMs);

        if (autoHideMs > 0)
        {
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(autoHideMs) };
            _timers[container] = timer;
            timer.Tick += (_, _) =>
            {
                TakeTimer(container);
                Conceal(container);
            };
            timer.Start();
        }
    }

    public static void Hide(Border container)
    {
        TakeTimer(container);
        Conceal(container);
    }

    /// <summary>
    /// 取主题画刷；取不到（窗口还没挂上资源字典，例如单元测试壳）时回落到透明，
    /// 绝不在这个位置写死色值 —— 写死就是深色主题下的一块浅色补丁。
    /// </summary>
    private static Brush ThemeBrush(string key)
        => Application.Current?.TryFindResource(key) as Brush ?? Brushes.Transparent;

    /// <summary>淡出 + 上滑，动画结束才折叠（动画未完成时被再次 Show，令牌会让这次收尾自动让位）。</summary>
    private static void Conceal(Border container)
    {
        if (container == null) return;
        _ = Motion.ConcealAsync(container, ConcealMs);
    }
}

/// <summary>
/// 页面日志帮助类。
/// 同时支持两类日志框：
///   - <see cref="RichTextBox"/>（推荐）：按级别着色。时间戳压暗、[LEVEL] 标签着级别色并加粗；
///     WARN/ERROR 连正文一起着色（异常行整行醒目），INFO/OK 正文保持主题正文色，
///     这样"有颜色"但不至于满屏彩色；
///   - <see cref="TextBox"/>：退化为纯文本，历史页面不改也能照常编译运行。
/// </summary>
public static class PageLog
{
    /// <summary>日志行的三种形态（参考稿：区段标题、普通操作行、收尾统计行）。</summary>
    private enum LogLineKind
    {
        /// <summary>普通行：`[09:07:22] 检测设备连接...` + 行尾 OK/Error。</summary>
        Normal,
        /// <summary>区段标题：两侧带横线的居中蓝字（`读分区表` / `解析固件`）。</summary>
        Section,
        /// <summary>收尾行：右对齐的蓝字统计（`完成, 耗时: 65ms`）。</summary>
        Footer,
    }

    /// <summary>
    /// 每渲染**一批**日志行后触发一次（参数：日志框 + 这批的行数），供页面刷新行数等 UI 状态。
    /// <para>
    /// ⚠️ 原来是「每行一次」：一批 N 行就 N 次回调 → N 次 <c>TxtLogCount.Text</c> 赋值 →
    /// N 次文本测量 + 布局失效，刷机高峰等于拿 UI 线程当计数器。
    /// 改成批回调后调用方自己把行数加起来即可（行数徽章的语义不变，只是少刷 N-1 次）。
    /// </para>
    /// </summary>
    public static event Action<object, int>? Appended;

    /// <summary>RichTextBox 日志保留的最大行数：超出后从最旧的丢，避免长时间运行越堆越卡。</summary>
    private const int MaxRichLines = 2000;

    /// <summary>详细行的级别名（引擎内部细节，默认档位隐藏）。</summary>
    private const string DetailLevel = "TRACE";

    /// <summary>「常规」档位的筛选键：显示除详细行之外的一切。</summary>
    private const string NormalFilter = "NORMAL";

    /// <summary>调用方逐行加的冗余来源前缀，展示时统一剥掉。</summary>
    private const string EdlPrefix = "[EDL] ";

    /// <summary>
    /// 一行日志的结构化记录。只记结构化字段（不记渲染结果），
    /// 这样切换「级别筛选」时能把文档整个重放出来，而不必让调用方自己缓存文本。
    /// </summary>
    private sealed class LogLine
    {
        public DateTime Time;
        public string Level = "";
        public string Body = "";
        public string Color = "";
        public bool ColorizeBody;

        /// <summary>行种类：普通行 / 区段标题（读分区表 —— 两侧带横线）/ 收尾行（完成, 耗时: 65ms）。</summary>
        public LogLineKind Kind = LogLineKind.Normal;

        /// <summary>整行压暗：正文也用时间戳那一档（正文色 55% 不透明度）。详细行的观感。</summary>
        public bool Dim;

        /// <summary>VT 式操作行：无级别徽章、无正文着色，等待追加状态后缀。</summary>
        public bool OperationStyle;

        /// <summary>这一行在等 `OK`/`Error` 后缀（只有 BeginOperation 会置位）。</summary>
        public bool AwaitingStatus;

        /// <summary>追加到行尾的状态后缀（VT 的 OK/Error/协议名），null = 还在进行中。</summary>
        public string? StatusText;
        public string? StatusColor;
        public bool StatusBold;

        /// <summary>版本行：Body 是名称（级别色加粗），BodySuffix 是值（正文色）。</summary>
        public bool NameValue;
        public string BodySuffix = "";

        /// <summary>
        /// 这一行已经画出来的段落（只有普通行才有）。补 OK/Error 后缀时直接拿它，
        /// 免得每次完成操作都从 2000 个 Block 里线性找一遍（CompleteOperation 是高频操作）。
        /// 被裁剪 / 档位重建后它就不再挂在文档上，调用方检测到会退回线性扫。
        /// </summary>
        public Paragraph? Rendered;
    }

    // 每个日志框一份历史/筛选/自动滚底开关。日志框是长生命周期控件，用引用相等做键最稳妥
    // （控件一般不重写 Equals，但用引用比较可以彻底避免意外命中）。
    private static readonly Dictionary<object, List<LogLine>> _history =
        new(ReferenceEqualityComparer.Instance);
    private static readonly Dictionary<object, string?> _filter =
        new(ReferenceEqualityComparer.Instance);
    private static readonly Dictionary<object, bool> _autoScroll =
        new(ReferenceEqualityComparer.Instance);

    // ─────────────────────────────────────────────────────────────
    //  批量渲染
    //  ─────────────────────────────────────────────────────────────
    //  一行的代价并不在"造一个 Paragraph"，而在**渲染之后那次布局 + 自动滚底**：
    //  `ScrollToEnd()` 会把整个文档重新排版一次。刷一个整包会打几百上千行，
    //  逐行 Append + 逐行 ScrollToEnd 等于把同样一段文档重排上千遍 —— 用户看到的就是
    //  「日志越刷越卡，进度条都不动了」。
    //  所以：Append 只负责把结构化行记进 _history 并排进待渲染队列，真正的段落构建、
    //  裁剪与滚底集中在 Flush 里，每 80ms（或不忙时）整批做一次。
    //  ⚠️ 顺序不会乱：同一个框的队列是 FIFO，flush 时按入队顺序逐行画。
    private const int FlushIntervalMs = 80;

    private static readonly Dictionary<object, List<LogLine>> _pending =
        new(ReferenceEqualityComparer.Instance);

    private static DispatcherTimer? _flushTimer;

    /// <summary>画刷缓存：颜色只有固定几档，而日志框每行都要两个画刷 —— 逐行 new 是纯浪费。</summary>
    private static readonly Dictionary<string, SolidColorBrush> _brushCache = new(StringComparer.Ordinal);

    /// <summary>压暗画刷按「正文色」缓存（浅色/深色主题各一份）。</summary>
    private static readonly Dictionary<Color, SolidColorBrush> _dimBrushCache = new();

    private static readonly FontFamily MonoFont = new("Cascadia Mono, Consolas, monospace");

    private static SolidColorBrush CachedBrush(string key, Color color)
    {
        if (_brushCache.TryGetValue(key, out var cached)) return cached;
        var brush = new SolidColorBrush(color);
        brush.Freeze();
        _brushCache[key] = brush;
        return brush;
    }

    private static SolidColorBrush LevelBrush(string hex)
        => CachedBrush(hex, (Color)ColorConverter.ConvertFromString(hex));

    /// <summary>级别徽章底：级别色的 22% 不透明度（与画刷一起缓存）。</summary>
    private static SolidColorBrush BadgeBrush(string hex)
    {
        var key = "bg:" + hex;
        if (_brushCache.TryGetValue(key, out var cached)) return cached;
        var c = (Color)ColorConverter.ConvertFromString(hex);
        var brush = new SolidColorBrush(Color.FromArgb(0x38, c.R, c.G, c.B));
        brush.Freeze();
        _brushCache[key] = brush;
        return brush;
    }

    private static SolidColorBrush DimBrush(Color baseColor)
    {
        if (_dimBrushCache.TryGetValue(baseColor, out var cached)) return cached;
        var brush = new SolidColorBrush(Color.FromArgb(0x8C, baseColor.R, baseColor.G, baseColor.B));
        brush.Freeze();
        _dimBrushCache[baseColor] = brush;
        return brush;
    }

    /// <summary>把一行排进待渲染队列。只有真的会显示（通过当前筛选）的行才排。</summary>
    /// <summary>
    /// 全局镜像日志框（外壳左栏的「运行日志」）。
    /// <para>
    /// 历史与待渲染都是**按日志框**存的，所以这里把同一行再排一份给镜像框 ——
    /// 镜像框拥有自己的一份历史与筛选，和页面内的日志框互不影响。
    /// 这样「所有页面都有一份完整日志」不需要改 12 个页面的调用点，只在这一处收口。
    /// </para>
    /// <para>
    /// 2026-10-07 起这个 setter 会挂 <c>IsVisibleChanged</c>：左栏在「页面自带左栏」的页签上
    /// （EDL / FASTBOOT / Payload …）是**被摘出可视树**的，此时镜像框 IsVisible = false。
    /// 见 <see cref="Enqueue"/> 里「只记不画」的分支。
    /// </para>
    /// </summary>
    private static object? _globalMirror;

    public static object? GlobalMirror
    {
        get => _globalMirror;
        set
        {
            if (ReferenceEquals(_globalMirror, value)) return;
            if (_globalMirror is FrameworkElement oldFe) oldFe.IsVisibleChanged -= OnMirrorVisibilityChanged;
            _globalMirror = value;
            if (value is FrameworkElement fe) fe.IsVisibleChanged += OnMirrorVisibilityChanged;
        }
    }

    /// <summary>
    /// 左栏重新出现：把收起期间**只记没画**的历史一次性重放出来。
    /// 不这么做的话，左栏会停在它被收起那一刻的画面（之后的日志一条都不补）。
    /// </summary>
    private static void OnMirrorVisibilityChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (sender is not RichTextBox rich || !rich.IsVisible) return;
        // 队列里可能还压着「可见的最后一瞬」入队的行 —— Rebuild 是整篇重画，先丢队列避免画两遍。
        _pending.Remove(rich);
        Rebuild(rich);
    }

    private static void Enqueue(object logBox, LogLine line)
    {
        // 2026-10-02 修复：日志统一**先汇到 UI 线程**再入队。_history/_pending 字典与
        // _flushTimer（DispatcherTimer）都不是线程安全的 —— 线程池线程直接入队会与 UI 线程
        // 的 Flush 竞争，还会在跨线程 Start() 上抛 InvalidOperationException（调用点在
        // Task.Run + async-void 链上就直接把页面任务打断）。主页的设备探测等调用方就是从
        // 线程池调 LogSuccess/LogInfo 的。转 UI 线程后所有内部结构都不必加锁。
        var app = System.Windows.Application.Current;
        if (app != null && !app.Dispatcher.CheckAccess())
        {
            app.Dispatcher.BeginInvoke(new Action(() => Enqueue(logBox, line)));
            return;
        }

        if (!Matches(logBox, line)) return;
        if (!_pending.TryGetValue(logBox, out var list))
            _pending[logBox] = list = new List<LogLine>(32);
        list.Add(line);

        // 同一行再排给全局镜像（排除镜像自己，避免递归）。
        var mirror = _globalMirror;
        if (mirror != null && !ReferenceEquals(mirror, logBox) && Matches(mirror, line))
        {
            // 镜像框的历史**必须自己记**：它没有调用方（全靠这里收口），
            // 少记的话档位重放 / 左栏恢复时会画出一个空文档。
            var hist = _history.TryGetValue(mirror, out var existing)
                ? existing
                : (_history[mirror] = new List<LogLine>(MaxRichLines));
            hist.Add(line);
            if (hist.Count > MaxRichLines) hist.RemoveRange(0, hist.Count - MaxRichLines);

            // 左栏被收起（不在可视树）时**只记不画**：镜像框此刻不参与排版，
            // 每行照建一套 Paragraph/Run 等于白烧一半的日志渲染预算 ——
            // 而 EDL / FASTBOOT / Payload 这些主力页恰恰都是自带左栏、镜像常年不可见。
            // 真正重新可见时由 OnMirrorVisibilityChanged 一次性 Rebuild 补回来。
            if (mirror is not FrameworkElement fe || !fe.IsVisible)
            {
                // 留在队列里的旧账一并清掉，恢复可见时 Rebuild 会按历史整篇重画。
                _pending.Remove(mirror);
            }
            else
            {
                if (!_pending.TryGetValue(mirror, out var mlist))
                    _pending[mirror] = mlist = new List<LogLine>(32);
                mlist.Add(line);
            }
        }

        ScheduleFlush();
    }

    private static void ScheduleFlush()
    {
        if (_flushTimer == null)
        {
            // Background 优先级：界面忙（重绘、滚动）时先让渲染跑，空闲了再一次性补上日志，
            // 绝不会跟渲染抢时间片。
            _flushTimer = new DispatcherTimer(DispatcherPriority.Background)
            {
                Interval = TimeSpan.FromMilliseconds(FlushIntervalMs)
            };
            _flushTimer.Tick += (_, _) => FlushAll();
        }
        if (!_flushTimer.IsEnabled) _flushTimer.Start();
    }

    /// <summary>把所有待渲染行立刻画出来（切筛选 / 复制 / 追加 OK 后缀前必须调用）。</summary>
    public static void FlushAll()
    {
        if (_pending.Count == 0)
        {
            _flushTimer?.Stop();
            return;
        }

        var boxes = new List<object>(_pending.Keys);
        foreach (var box in boxes)
        {
            if (!_pending.TryGetValue(box, out var lines)) continue;
            _pending.Remove(box);
            FlushOne(box, lines);
        }
    }

    /// <summary>只把某个日志框的待渲染行画出来（不动别人的队列）。</summary>
    private static void FlushBox(object? logBox)
    {
        if (logBox == null) return;
        if (!_pending.TryGetValue(logBox, out var lines)) return;
        _pending.Remove(logBox);
        FlushOne(logBox, lines);
    }

    private static void FlushOne(object logBox, List<LogLine> lines)
    {
        if (logBox is RichTextBox rich)
        {
            // ⚠️ FlowDocument.BeginChange/EndChange 是 **internal**，这里用不了（CS1061）。
            // 好在 WPF 的布局是延迟的：同一轮里连续 Add 只会触发一次排版，
            // 真正被省掉的是**逐行 ScrollToEnd**（那才会把整篇文档重排上千遍）。
            foreach (var line in lines) AppendRichLine(rich, line);
            TrimDocument(rich);

            // 滚底整批只做一次（原来是一行一次）。
            // 只有**看得见**的框才滚：页签切走后日志框不在可视树里，滚它纯属白做
            // （切回来时由可见性钩子补一次，见 EnsureVisibilityHook）。
            EnsureVisibilityHook(rich);
            if (rich.IsVisible && (!_autoScroll.TryGetValue(rich, out var asc) || asc))
                rich.ScrollToEnd();
        }
        else if (logBox is TextBox textBox)
        {
            var sb = new System.Text.StringBuilder(lines.Count * 48);
            foreach (var line in lines)
            {
                // TextBox 侧没有富文本，区段标题/收尾统计退化成纯文本形态
                if (line.Kind == LogLineKind.Section)
                {
                    sb.Append("────── ").Append(line.Body).Append(" ──────").Append('\n');
                    continue;
                }
                if (line.Kind == LogLineKind.Footer)
                {
                    sb.Append("                    ").Append(line.Body).Append('\n');
                    continue;
                }

                var tag = line.OperationStyle ? "" : $"[{line.Level}] ";
                sb.Append('[').Append(line.Time.ToString("HH:mm:ss")).Append("] ").Append(tag).Append(line.Body).Append('\n');
            }
            textBox.AppendText(sb.ToString());
            EnsureVisibilityHook(textBox);
            if (textBox.IsVisible && (!_autoScroll.TryGetValue(textBox, out var asc) || asc))
                textBox.ScrollToEnd();
        }
        else
        {
            return;
        }

        // 一批只回调一次（原来一行一次：N 行 = N 次行数徽章赋值 = N 次文本测量/布局失效）
        Appended?.Invoke(logBox, lines.Count);
    }

    /// <summary>
    /// 裁剪最旧的行。**攒一批再裁**：每 flush 都把文档精确压回 2000 行的话，
    /// 超限之后每次 flush 都要从头部 Remove 一次；攒到多出 <see cref="TrimSlack"/> 行才统一裁，
    /// 把「逐行裁」摊成「每 64 行裁一次」。
    /// </summary>
    private const int TrimSlack = 64;

    private static void TrimDocument(RichTextBox rich)
    {
        if (rich.Document.Blocks.Count <= MaxRichLines + TrimSlack) return;
        while (rich.Document.Blocks.Count > MaxRichLines && rich.Document.Blocks.FirstBlock != null)
            rich.Document.Blocks.Remove(rich.Document.Blocks.FirstBlock);
    }

    /// <summary>
    /// 「重新可见时补一次滚底」的钩子（每个框只挂一次，用 CWT 避免静态集合扣住控件）。
    /// <para>
    /// 页签切走期间我们**跳过** ScrollToEnd（框不在可视树，滚它等于白做一次排版）；
    /// 切回来时必须补上，否则视口停在切走那一刻的偏移，看着像"自动滚底失效"。
    /// 同时把这段期间攒下的待渲染行一次画掉。
    /// </para>
    /// </summary>
    private static readonly System.Runtime.CompilerServices.ConditionalWeakTable<object, object> _visHooked = new();

    private static void EnsureVisibilityHook(object logBox)
    {
        if (logBox is not FrameworkElement fe) return;
        if (_visHooked.TryGetValue(fe, out _)) return;
        _visHooked.Add(fe, new object());

        fe.IsVisibleChanged += (_, _) =>
        {
            if (!fe.IsVisible) return;
            var asc = !_autoScroll.TryGetValue(fe, out var on) || on;
            switch (fe)
            {
                case RichTextBox rich when asc:
                    FlushBox(rich);
                    rich.ScrollToEnd();
                    break;
                case TextBox text when asc:
                    FlushBox(text);
                    text.ScrollToEnd();
                    break;
            }
        };
    }

    /// <summary>
    /// 常规信息行。经典工具箱风格：**常态行不带级别徽章**（满屏 `INFO` 是噪音），
    /// 只有 `Warn` / `Error` 保留彩色徽章，让异常一眼可辨。
    /// </summary>
    public static void Info(object? logBox, string message)
        => Append(logBox, "INFO", message, InfoColor, colorizeBody: false, operationStyle: true);

    /// <summary>区段标题：两侧带横线的蓝字（参考稿的「读分区表」「解析固件」）。
    /// 一次操作开始前调一次，同时把外壳状态栏的「任务」段刷成同一个名字。</summary>
    public static void Section(object? logBox, string title)
    {
        Append(logBox, "INFO", title, SectionColor, colorizeBody: false,
               operationStyle: true, kind: LogLineKind.Section);
        ShellStatus.BeginTask(title);
    }

    /// <summary>收尾行：右对齐的蓝字统计（参考稿的「完成, 耗时: 65ms」）。</summary>
    public static void Footer(object? logBox, string text)
        => Append(logBox, "INFO", text, SectionColor, colorizeBody: false,
                  operationStyle: true, kind: LogLineKind.Footer);

    public static void Success(object? logBox, string message)
        => Append(logBox, "OK", message, OkColor, colorizeBody: false);

    public static void Warning(object? logBox, string message)
        => Append(logBox, "WARN", message, WarnColor, colorizeBody: true);

    public static void Error(object? logBox, string message)
        => Append(logBox, "ERROR", message, ErrorColor, colorizeBody: true);

    /// <summary>
    /// 引擎内部细节行（对应 FlashSession 的 `Verbose()`，UI 标记 `__EDL_DETAIL__|`）：
    /// 参数 dump、逐 LUN 扇区数、patch 明细、原始响应原文这类"工程师要看、刷机的人不想看"的内容。
    ///
    /// 它是独立级别 <c>TRACE</c>，**整行压暗 + 无级别徽章**，默认档位「常规」不显示；
    /// 在上面那个级别下拉里切「详细」才会出现（历史是结构化存的，切档会整篇重放）。
    /// </summary>
    public static void Detail(object? logBox, string message)
        => Append(logBox, DetailLevel, message, DetailColor, colorizeBody: false,
                  operationStyle: true, dim: true);

    /// <summary>自动滚到底：用户往上翻看历史时关掉，新日志就不会再把视口拽走。</summary>
    public static void SetAutoScroll(object? logBox, bool enabled)
    {
        if (logBox == null) return;
        _autoScroll[logBox] = enabled;
    }

    // ─────────────────────────────────────────────────────────────
    //  VT 式操作行（模仿 VioletToolBox 的 EDL 刷写日志）：
    //  `__EDL_FLASH_BEGIN__|xxx` 起一行操作（时间戳 + 操作文本，无级别徽章），
    //  `__EDL_FLASH_OK__` / `__EDL_FLASH_ERROR__` 把 `OK`（绿粗）/ `Error`（红粗）
    //  追加到**同一行**尾部 —— 一次操作只占一行，成败一眼可辨。
    // ─────────────────────────────────────────────────────────────

    // 日志级别色（与 DesignTokens 的 Theme.Log* 同值；这里写死是因为
    // 日志行是**按行缓存**的 RichTextBox 内联元素，走画刷会让每行都挂一个动态资源引用）。
    // 参考稿配色：OK = 中绿、操作行/区段标题 = 饱和蓝、告警 = 琥珀、错误 = 砖红。
    private const string InfoColor = "#2F6FD0";
    private const string SectionColor = "#2F6FD0";
    private const string OkColor = "#1F8B4C";
    private const string WarnColor = "#B26A00";
    private const string ErrorColor = "#C93B2E";
    private const string DetailColor = "#8A9199";

    /// <summary>开始一行操作（对应 `__EDL_FLASH_BEGIN__|前缀`）。只有它会被 CompleteOperation 认领。</summary>
    public static void BeginOperation(object? logBox, string body)
        => Append(logBox, "INFO", body, InfoColor, colorizeBody: false, operationStyle: true, awaitingStatus: true);

    /// <summary>给最近一行未完结的操作行追加 OK / Error 后缀。</summary>
    public static void CompleteOperation(object? logBox, bool ok)
    {
        if (logBox is not RichTextBox rich) return;
        // 目标段落可能还在待渲染队列里 —— 不先画出来就没有可追加后缀的 Paragraph。
        FlushBox(rich);
        if (!_history.TryGetValue(rich, out var list)) return;

        for (var i = list.Count - 1; i >= 0; i--)
        {
            var line = list[i];
            if (!line.AwaitingStatus || line.StatusText != null)
                continue;
            line.StatusText = ok ? "OK" : "Error";
            line.StatusColor = ok ? OkColor : ErrorColor;
            line.StatusBold = true;
            AttachStatusRun(rich, line);
            return;
        }
    }

    /// <summary>连接探测行：把协议状态（sahara/firehose/Unknown/Error）追加到行尾。</summary>
    public static void CompleteProbe(object? logBox, string text)
    {
        if (logBox is not RichTextBox rich) return;
        FlushBox(rich);
        if (!_history.TryGetValue(rich, out var list)) return;

        var color = text.Equals("Error", StringComparison.OrdinalIgnoreCase) ? ErrorColor
            : text.Equals("Unknown", StringComparison.OrdinalIgnoreCase) ? WarnColor
            : InfoColor;
        for (var i = list.Count - 1; i >= 0; i--)
        {
            var line = list[i];
            if (!line.AwaitingStatus || line.StatusText != null)
                continue;
            line.StatusText = text;
            line.StatusColor = color;
            line.StatusBold = false;
            AttachStatusRun(rich, line);
            return;
        }
    }

    /// <summary>版本信息块头（VT 的 __EDL_VERSION_HEADER__）：强调色加粗标题 + 设备摘要。</summary>
    public static void VersionHeader(object? logBox, string summary)
    {
        Append(logBox, "INFO", "设备版本信息", OkColor, colorizeBody: true, operationStyle: true);
        if (!string.IsNullOrWhiteSpace(summary))
        {
            var line = new LogLine
            {
                Time = DateTime.Now,
                Level = "INFO",
                Body = summary,
                Color = InfoColor,
                OperationStyle = true,
                BodySuffix = string.Empty,
            };
            if (logBox is RichTextBox rich)
            {
                var list = _history.TryGetValue(rich, out var existing)
                    ? existing
                    : (_history[rich] = new List<LogLine>());
                list.Add(line);
                Enqueue(rich, line);
            }
            else if (logBox is TextBox text)
            {
                text.AppendText($"{line.Time:HH:mm:ss} {summary}" + Environment.NewLine);
                Appended?.Invoke(logBox, 1);
            }
        }
    }

    /// <summary>版本信息行（VT 的 __EDL_VERSION_ROW__|名称|值）：名称加粗、值正文色。</summary>
    public static void VersionRow(object? logBox, string name, string value)
    {
        if (logBox == null) return;
        var line = new LogLine
        {
            Time = DateTime.Now,
            Level = "INFO",
            Body = name,
            Color = InfoColor,
            NameValue = true,
            ColorizeBody = true,   // 名称用强调色（VT 的版本行无徽章、名称着色）
            OperationStyle = true, // 无级别徽章
            BodySuffix = value.Length > 0 ? "：" + value : string.Empty,
        };
        if (logBox is RichTextBox rich)
        {
            var list = _history.TryGetValue(rich, out var existing)
                ? existing
                : (_history[rich] = new List<LogLine>());
            list.Add(line);
            if (list.Count > MaxRichLines) list.RemoveRange(0, list.Count - MaxRichLines);
            Enqueue(rich, line);
        }
        else if (logBox is TextBox text)
        {
            text.AppendText($"{line.Time:HH:mm:ss} {name}：{value}\n");
            Appended?.Invoke(logBox, 1);
        }
    }

    /// <summary>版本信息块结束（VT 的 __EDL_VERSION_DONE__）。</summary>
    public static void VersionDone(object? logBox)
    {
        // 视觉上的块分隔由 VersionRow 的行距承担；此处仅为标记完整性保留。
    }

    /// <summary>把状态后缀补画到已渲染的段落上（筛选重建时由 AppendRichLine 直接渲染）。</summary>
    private static void AttachStatusRun(RichTextBox rich, LogLine line)
    {
        // O(1) 路径：渲染时就把段落记在行上，直接补后缀。
        if (line.Rendered is { } cached && ReferenceEquals(cached.Parent, rich.Document))
        {
            AppendStatusRun(cached, rich.Foreground, line);
            return;
        }

        // 兜底：段落已被裁掉或还没画（档位重建后），退回线性找。
        foreach (var block in rich.Document.Blocks)
        {
            if (block is Paragraph para && ReferenceEquals(para.Tag, line))
            {
                // 统一补一个空格，分区写入行成 "... OK"。
                AppendStatusRun(para, rich.Foreground, line);
                return;
            }
        }
    }

    /// <summary>
    /// 按级别过滤（null/空 = 全部）。只有 RichTextBox 支持重建；TextBox 退化为只记历史。
    /// </summary>
    public static void ApplyFilter(object? logBox, string? level)
    {
        if (logBox == null) return;
        _filter[logBox] = string.IsNullOrEmpty(level) ? null : level;
        // 待渲染队列里有旧档位下排进去的行 —— 丢掉它们，否则 Rebuild 之后这批行会再画一遍，
        // 而且画到新档位下（该被过滤掉的行会漏出来）。
        _pending.Remove(logBox);
        if (logBox is RichTextBox rich) Rebuild(rich);
    }

    /// <summary>清空某个日志框（历史 + 界面）。清空按钮必须走这里，否则历史残留会「复活」旧日志。</summary>
    public static void Clear(object? logBox)
    {
        if (logBox == null) return;
        _history.Remove(logBox);
        _pending.Remove(logBox);
        if (logBox is RichTextBox rich) rich.Document.Blocks.Clear();
        else if (logBox is TextBox text) text.Clear();
    }

    /// <summary>
    /// 剥掉调用方逐个加的 `[EDL] ` 前缀：「运行日志」面板本身就长在 EDL 页上，
    /// 每行再加一次等于白占 7 个字符。日志列最窄只有 192px，长消息本来就要折行，
    /// 这 7 个字符会把它多折一行 —— 放在展示层统一剥，比改 150 处文案稳，
    /// 以后新加的日志行也不会再漏出来。
    /// </summary>
    private static string StripEdlPrefix(string body)
        => body.StartsWith(EdlPrefix, StringComparison.Ordinal) ? body[EdlPrefix.Length..] : body;

    /// <summary>导出纯文本（复制全部用）。</summary>
    public static string GetText(object? logBox)
    {
        FlushBox(logBox);
        if (logBox is RichTextBox rich)
            return new TextRange(rich.Document.ContentStart, rich.Document.ContentEnd).Text;
        if (logBox is TextBox text) return text.Text;
        return "";
    }

    private static void Append(object? logBox, string level, string body, string color, bool colorizeBody,
        bool operationStyle = false, bool awaitingStatus = false, bool dim = false,
        LogLineKind kind = LogLineKind.Normal)
    {
        if (logBox == null) return;

        var line = new LogLine
        {
            Time = DateTime.Now,
            Level = level,
            Body = StripEdlPrefix(body),
            Color = color,
            ColorizeBody = colorizeBody,
            OperationStyle = operationStyle,
            AwaitingStatus = awaitingStatus,
            Dim = dim,
            Kind = kind,
        };

        if (logBox is RichTextBox rich)
        {
            var list = _history.TryGetValue(rich, out var existing)
                ? existing
                : (_history[rich] = new List<LogLine>());
            list.Add(line);
            if (list.Count > MaxRichLines) list.RemoveRange(0, list.Count - MaxRichLines);

            // 行数徽章只统计**真的画出来**的行：默认档位藏着详细行时，
            // 徽章报的必须是用户看得见的行数，否则「40 行」旁边只显示 12 行。
            Enqueue(rich, line);
        }
        else if (logBox is TextBox text)
        {
            // TextBox 侧没有结构化历史，纯文本直接入队（flush 时一次性 AppendText）
            if (!_pending.TryGetValue(text, out var queued))
                _pending[text] = queued = new List<LogLine>(32);
            queued.Add(line);
            ScheduleFlush();
        }
        else
        {
            return;
        }
    }

    // 档位语义：
    //   null / ""  → 「详细」= 全都显示（含 TRACE 详细行）
    //   NORMAL     → 「常规」= 除 TRACE 之外的一切
    //   其它       → 精确匹配单个级别（INFO / OK / WARN / ERROR）
    private static bool Matches(object logBox, LogLine line)
    {
        if (!_filter.TryGetValue(logBox, out var level) || string.IsNullOrEmpty(level)) return true;
        if (string.Equals(level, NormalFilter, StringComparison.OrdinalIgnoreCase))
            return !string.Equals(line.Level, DetailLevel, StringComparison.OrdinalIgnoreCase);
        return string.Equals(line.Level, level, StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>
    /// 主题切换：把所有日志框整篇重画。
    /// <para>
    /// 段落里的 Run 画刷是**渲染时定格**的 —— 时间戳是当时前景色的 55% 压暗版、正文直接
    /// 引用当时的 <c>rich.Foreground</c> 实例。主题切换只换资源字典，画在 Run 上的旧实例
    /// 不会跟着变：浅色下的黑字留在深色主题里 = 整片日志看不见（截图验收时踩到的正是这个）。
    /// </para>
    /// </summary>
    public static void RecolorAll()
    {
        var app = Application.Current;
        if (app != null && !app.Dispatcher.CheckAccess())
        {
            app.Dispatcher.BeginInvoke(new Action(RecolorAll));
            return;
        }

        foreach (var box in _history.Keys)
        {
            if (box is not RichTextBox rich) continue;
            // 队列里还压着旧画刷下排进去的行 —— Rebuild 是整篇重画，先丢队列避免画两遍
            //（与 ApplyFilter / OnMirrorVisibilityChanged 同一个套路）。
            _pending.Remove(rich);
            Rebuild(rich);
        }
    }

    private static void Rebuild(RichTextBox rich)
    {
        rich.Document.Blocks.Clear();
        if (!_history.TryGetValue(rich, out var list)) return;
        foreach (var line in list)
            if (Matches(rich, line)) AppendRichLine(rich, line);
        rich.ScrollToEnd();
    }

    private static void AppendRichLine(RichTextBox rich, LogLine line)
    {
        // 画刷/字体全部走缓存：整页固定就这么几档颜色，逐行 new 会让 GC 在长刷写里持续抖动。
        var accent = LevelBrush(line.Color);
        // 徽章底：级别色的 22% 透明度当底、级别色当字，浅色/深色主题下都看得清，
        // 也不会像实心色块那样满屏花。
        var badge = BadgeBrush(line.Color);
        // 时间戳用正文色的 55% 不透明度，压暗但跟随主题（深色主题下不会变成看不清的深灰）
        var dim = DimBrush((rich.Foreground as SolidColorBrush)?.Color ?? Colors.Black);

        // 区段标题 / 收尾统计不走普通行模板：它们没有时间戳，形态是「横线 + 蓝字」与「右对齐蓝字」。
        if (line.Kind != LogLineKind.Normal)
        {
            rich.Document.Blocks.Add(line.Kind == LogLineKind.Section
                ? BuildSectionBlock(line, accent)
                : BuildFooterBlock(line, accent));
            return;
        }

        var para = new Paragraph { Margin = new Thickness(0), LineHeight = 17, Tag = line };

        // 时间戳带方括号（参考稿：`[09:07:22] 检测设备连接...OK`）——
        // 方括号让「时间」和「正文」在等宽字体里一眼分段，比裸空格清楚。
        para.Inlines.Add(new Run($"[{line.Time:HH:mm:ss}]")
        {
            Foreground = dim,
            FontFamily = MonoFont,
        });

        if (!line.OperationStyle)
        {
            // 级别徽章：等宽字体 + 固定宽度感（左右各留一个空格做内边距）
            para.Inlines.Add(new Run($" {line.Level} ")
            {
                Foreground = accent,
                Background = badge,
                FontWeight = FontWeights.SemiBold,
                FontFamily = MonoFont,
            });
        }

        para.Inlines.Add(new Run(" " + line.Body)
        {
            // 详细行整行压暗（和左边时间戳同一档），一眼就能和操作行区分开
            Foreground = line.Dim ? dim : (line.ColorizeBody ? accent : rich.Foreground),
            FontWeight = line.NameValue ? FontWeights.SemiBold : FontWeights.Normal,
        });

        if (line.BodySuffix.Length > 0)
        {
            para.Inlines.Add(new Run(line.BodySuffix) { Foreground = rich.Foreground });
        }

        AppendStatusRun(para, rich.Foreground, line);

        // 裁剪与滚底已收进 FlushOne（整批一次），这里只负责画这一行。
        rich.Document.Blocks.Add(para);
        // 记下段落：完成操作（OK/Error 后缀）时直接补这里，不再扫全部 Block。
        line.Rendered = para;
    }

    /// <summary>
    /// 区段标题块：`────── 读分区表 ──────`（两侧横线 + 居中蓝字）。
    /// ⚠️ 必须用 <see cref="BlockUIContainer"/> 而不是 Paragraph：只有 Block 级元素才会
    /// 被拉伸到整行宽度，横线才可能通栏；放 Paragraph 里 Grid 只会按内容宽塌成一小段。
    /// </summary>
    private static Block BuildSectionBlock(LogLine line, Brush accent)
    {
        var grid = new Grid { Margin = new Thickness(0, 7, 0, 5) };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var left = new Border
        {
            Height = 1,
            Background = BorderBrush(),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 9, 0),
        };
        var right = new Border
        {
            Height = 1,
            Background = BorderBrush(),
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(9, 0, 0, 0),
        };
        var text = new TextBlock
        {
            Text = line.Body,
            Foreground = accent,
            FontSize = 12,
            FontWeight = FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
        };

        Grid.SetColumn(text, 1);
        grid.Children.Add(left);
        grid.Children.Add(text);
        grid.Children.Add(right);

        return new BlockUIContainer(grid) { Tag = line };
    }

    /// <summary>收尾统计块：右对齐蓝字（`完成, 耗时: 65ms`）。</summary>
    private static Block BuildFooterBlock(LogLine line, Brush accent)
        => new Paragraph(new Run(line.Body)
        {
            Foreground = accent,
            FontFamily = MonoFont,
        })
        {
            TextAlignment = TextAlignment.Right,
            Margin = new Thickness(0, 2, 0, 6),
            LineHeight = 16,
            Tag = line,
        };

    /// <summary>区段标题两侧横线的颜色：跟随主题描边色（深浅两套都不用改代码）。</summary>
    private static Brush BorderBrush()
    {
        if (Application.Current?.TryFindResource("Theme.Border") is Brush brush) return brush;
        return Brushes.LightGray;
    }

    /// <summary>把状态后缀（VT 的 OK/Error/协议名）追加到段落尾部。
    /// 分区写入行（body 以 "..." 结尾，KMFlashTool 风格）补空格成 "... OK"；
    /// 其余同样补一个空格，避免和正文粘连。</summary>
    private static void AppendStatusRun(Paragraph para, Brush baseForeground, LogLine line)
    {
        if (string.IsNullOrEmpty(line.StatusText))
            return;

        para.Inlines.Add(new Run(" ")
        {
            Foreground = baseForeground,
        });
        para.Inlines.Add(new Run(line.StatusText)
        {
            Foreground = LevelBrush(line.StatusColor!),
            FontWeight = line.StatusBold ? FontWeights.Bold : FontWeights.SemiBold,
            FontFamily = MonoFont,
        });
    }
}
