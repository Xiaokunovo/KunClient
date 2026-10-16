using System;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using System.Runtime.InteropServices;
using KunFlashTool.App.Localization;
using KunFlashTool.App.Dialogs;
using KunFlashTool.App.Services;

namespace KunFlashTool.App;

public partial class App : Application
{
    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int attrValue, int attrSize);
    private const int DWMWA_TRANSITIONS_FORCEDISABLED = 3;
    /// <summary>Win11 22H2(22621)+ 才有：设置窗口的「系统背景」，即 Acrylic/Mica 磨砂。</summary>
    private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
    /// <summary>DWMSBT_TRANSIENTWINDOW = Acrylic（对话框/浮层的标准档，Mica 那是主窗档）。</summary>
    private const int DWMSBT_TRANSIENTWINDOW = 3;
    
    [DllImport("user32.dll")]
    private static extern bool SetProcessDPIAware();

    protected override void OnStartup(StartupEventArgs e)
    {
        try { SetProcessDPIAware(); } catch { }

        // 注意：不要无条件启用软件渲染——分层窗口(AllowsTransparency)在软件渲染下
        // 圆角区域会出现白底/黑底伪影。仅在用户显式传入 --disable-gpu 时启用。
        foreach (string arg in e.Args)
        {
            if (arg.Equals("--disable-gpu", StringComparison.OrdinalIgnoreCase))
            {
                System.Windows.Media.RenderOptions.ProcessRenderMode = System.Windows.Interop.RenderMode.SoftwareOnly;
            }
            else if (arg.Equals("--clear-cache", StringComparison.OrdinalIgnoreCase))
            {
                ClearCacheOnExit = true;
            }
        }

        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += OnDomainUnhandledException;
        TaskScheduler.UnobservedTaskException += OnUnobservedTaskException;

        // 全局窗口入场：任何 Window 在 Loaded 时统一播一次淡入 + 轻微缩放"落定"。
        // 这是**补齐**而不是叠加 —— 原来只有主窗 / 登录 / 更新 / 下载 4 处自己有淡入，
        // 其余 9 个弹窗都是"啪"地出现。WindowAnimation.FadeInAsync 内部按窗口实例幂等，
        // 自己已经播过的那几个不会被淡第二遍。
        // 🔴 StartupWindow 是 1x1、Opacity=0 的隐藏辅助窗，必须跳过（淡入会把它显示出来）。
        EventManager.RegisterClassHandler(
            typeof(Window), FrameworkElement.LoadedEvent,
            new RoutedEventHandler(OnAnyWindowLoaded), handledEventsToo: true);

        LocalizationManager.Initialize();
        ThemeManager.ApplyTheme(ThemeManager.DetectSystemTheme());

        base.OnStartup(e);

        // ── 启动即环境检测（2026-10-09，用户要求「一打开就开始检测」）─────────
        // 不等到登录之后再查：登录前就把命中结果送上去，服务端才能在**没有账号可归因**时
        // 按 IP 处置（封 IP + 邮件通知开发者）。
        // 🔴 两条硬约束：
        //   ① 丢线程池、绝不阻塞启动 —— 检测要走 WMI / 进程枚举 / 证书存储，慢的时候很明显；
        //   ② 任何异常都不许冒到启动路径上（EnvReportService 内部已全吞，这里再兜一层）。
        _ = Task.Run(() =>
        {
            try { EnvReportService.ReportAnonymousIfNeeded(); }
            catch { }
        });

        // 合规前置：未同意当前版本的使用条款 / 隐私说明，就不进登录界面。
        // 点「不同意」时 DisclaimerDialog 内部会直接 Shutdown()，这里必须立刻返回，
        // 否则会继续把登录框弹出来。
        if (AppSettings.Current.LegalAcceptedVersion < DisclaimerDialog.LegalVersion)
        {
            var disclaimer = new DisclaimerDialog(enforce: true)
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen
            };
            if (disclaimer.ShowDialog() != true)
            {
                Shutdown();
                return;
            }

            AppSettings.Current.LegalAcceptedVersion = DisclaimerDialog.LegalVersion;
            AppSettings.Current.Save();
        }

        // Show login dialog directly
        var loginDialog = new LoginDialog();
        loginDialog.WindowStartupLocation = WindowStartupLocation.CenterScreen;
        loginDialog.ShowDialog();
    }

    /// <summary>是否在退出时清理 %TEMP%\KunClient 下载缓存（仅 --clear-cache 启动参数生效）。</summary>
    internal static bool ClearCacheOnExit { get; private set; }

    /// <summary>
    /// 所有窗口（含 13 个弹窗）的统一入场。挂在 <see cref="FrameworkElement.LoadedEvent"/> 上，
    /// 该事件是 Direct 路由，所以只会对 Window 自身触发，不会打到里面的子元素。
    /// </summary>
    private static void OnAnyWindowLoaded(object sender, RoutedEventArgs e)
    {
        if (sender is not Window window) return;
        if (window is StartupWindow) return;                      // 隐藏辅助窗，不能淡入

        // 液态玻璃底（2026-10-08，用户反馈「液态玻璃效果没有实现」）：
        // 主窗口有自己的壁纸层 + DynamicBackground，跳过；其余对话框统一挂 Acrylic。
        // ⚠️ 这里必须写全限定名：在 App 里裸写 MainWindow 会解析到继承来的
        //    Application.MainWindow **属性**（非静态），直接 CS0120。
        if (window is not KunFlashTool.App.MainWindow) ApplyGlassBackdrop(window);

        if (WindowAnimation.HasPlayedEntrance(window)) return;    // 自己已播过的（主窗等）不重复
        _ = WindowAnimation.FadeInAsync(window);
    }

    /// <summary>
    /// 给普通对话框挂上 Win11 的 Acrylic 系统背景 —— 这就是「液态玻璃」的**磨砂底**。
    /// <para>⚠️ 顺序很关键：**先试着挂背景，挂上了才把窗口底色清成 Transparent**。
    /// 反过来（先透明再挂）在 Win10 / 关了透明效果 / DWM 调用失败时，窗口就**直接透出桌面**，
    /// 文字糊在壁纸上没法看。挂不上时保持不透明底色 —— 那正好就是改动前的观感，零回归。</para>
    /// <para>对话框内容区应该自己用半透明的 <c>Theme.BgTint</c>（而不是实色 <c>Theme.Bg</c>）：
    /// 实色会把磨砂整块盖死，那正是「玻璃没实现」的外观病根。</para>
    /// </summary>
    private static void ApplyGlassBackdrop(Window window)
    {
        if (TryEnableAcrylicBackdrop(window))
        {
            window.Background = System.Windows.Media.Brushes.Transparent;
            return;
        }

        try
        {
            if (Application.Current?.TryFindResource("Theme.Bg") is System.Windows.Media.Brush opaque)
                window.Background = opaque;
        }
        catch { }
    }

    /// <summary>
    /// 尝试开启 Win11 的 Acrylic 系统背景。返回是否真的挂上了
    /// （调用方据此决定窗口底色透明还是不透明，见 <see cref="ApplyGlassBackdrop"/>）。
    /// <para>🔴 2026-10-08 实测踩到的坑（截图取证）：<c>DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)</c>
    /// **返回 0（成功）并不代表磨砂真的会被画出来**。本机
    /// <c>HKCU\...\Themes\Personalize\EnableTransparency = 0</c>（系统「透明效果」关闭）时，
    /// DWM 照样返回成功，但客户区被渲染成**纯黑** —— 窗口底色一透明，整块变黑洞。
    /// 所以必须**再查一次这个开关**，只有它开着才认为挂上了。</para>
    /// <para>查不到（键不存在 / 读取失败）一律按「没开」处理 —— 宁可退回不透明的旧观感，
    /// 也不能赌一把把用户界面变成黑框。</para>
    /// </summary>
    internal static bool TryEnableAcrylicBackdrop(Window window)
    {
        try
        {
            if (!IsTransparencyEnabled()) return false;

            // DWMWA_SYSTEMBACKDROP_TYPE 只存在于 Win11 22H2(22621)+，更早的系统调用只会返回失败
            if (Environment.OSVersion.Version.Build < 22621) return false;

            var hwnd = new System.Windows.Interop.WindowInteropHelper(window).Handle;
            if (hwnd == IntPtr.Zero) return false;

            var backdrop = DWMSBT_TRANSIENTWINDOW;
            return DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int)) == 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// 系统「透明效果」是否开启（设置 → 个性化 → 颜色 → 透明效果）。
    /// 关掉时 DWM 会把系统背景画成不透明黑 —— 见 <see cref="TryEnableAcrylicBackdrop"/> 的说明。
    /// </summary>
    private static bool IsTransparencyEnabled()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            // 只有明确读到非 0 才算开：读不到就按关处理（安全侧）
            return key?.GetValue("EnableTransparency") is int v && v != 0;
        }
        catch
        {
            return false;
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        // 退出清理：%TEMP%\KunClient 与 %LOCALAPPDATA%\KunClient **整个目录**全删
        // （用户要求「关闭工具就不留东西」）。
        //
        // 这两处删掉的都必须是**可再生的缓存**，不能有跨会话状态，也**不能再放大件**：
        //   - %LOCALAPPDATA%\KunClient：云端背景缓存、云端驱动、WebView2 缓存、crash.log
        //   - %TEMP%\KunClient：刷机临时产物、下载中的半截包
        //
        // ⚠️ 下面三类状态**不能**放在这两个目录里，已经统一迁到
        //    %LOCALAPPDATA%\KunClientState（见 Core/Configuration/AppStatePaths.cs）：
        //      1) settings.json 的 LegalAcceptedVersion —— 丢了每次启动重新弹合规条款（不同意直接 Shutdown()）；
        //      2) login.dat 登录态 —— 丢了每次启动都要重新登录；
        //      3) KunClient.config.json 服务器地址 —— 丢了静默回落内置默认地址。
        //    新增持久化状态时一律走 AppStatePaths，别再往被清理的目录里写。
        //    另外 EDL 原生日志（EdlLog*.txt，FlashSession.StartNativeLog）也放在
        //    KunClientState\logs —— 它是用户反馈刷机失败时的唯一证据，写完就被删等于白写。
        //
        // 🔴 2026-10-08（用户反馈「下载速度太慢了现在」）：**大件工具链也搬走了**，
        //    现在在 %LOCALAPPDATA%\KunClientState\cache\{platform-tools,scrcpy}。
        //    原来 adb/fastboot(5.1MB) 与 scrcpy(11.3MB) 就住在这两个被删的目录里，
        //    于是**每次启动都要重下 16.4MB** —— 单连接口径 ~48 秒，这才是「下载慢」的主因。
        //    「不留痕迹」针对的是**使用痕迹**（日志/配置/临时产物），而公共工具链不含任何用户数据、
        //    对所有用户字节相同，留着不影响隐私，却直接省掉每次启动的十几 MB。
        //    ⚠️ 以后新增「体积大 + 内容与用户无关」的资源，一律放 AppStatePaths.Cache，
        //       不要放回下面这两个目录。
        DeleteDirectoryQuiet(Path.Combine(Path.GetTempPath(), "KunClient"));
        DeleteDirectoryQuiet(Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KunClient"));

        // 退出时结束「本工具自己拉起」的 adb server。
        // 只 kill 路径等于我们 platform-tools 目录下 adb.exe 的进程（见 PlatformToolsManager.KillAdbServer），
        // 不会误伤 Android Studio / scrcpy 等外部工具自带的 adb，避免残留后台进程占着 5037 端口。
        try
        {
            new KunFlashTool.Core.PlatformTools.PlatformToolsManager().KillAdbServer();
        }
        catch { }

        base.OnExit(e);
    }

    /// <summary>
    /// 安静地删掉整个目录：退出路径上不能因为删不掉文件而抛异常（否则表现为「关不掉 / 崩一下」）。
    /// 整目录删除会在「有文件被占用」或「文件带只读属性」时整批失败，
    /// 所以失败后退化为逐个删文件、再从最深层往上删目录 —— 能删多少算多少。
    /// </summary>
    private static void DeleteDirectoryQuiet(string dir)
    {
        if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return;

        try
        {
            Directory.Delete(dir, recursive: true);
            return;
        }
        catch { }

        try
        {
            foreach (var file in Directory.GetFiles(dir, "*", SearchOption.AllDirectories))
            {
                try
                {
                    // 只读文件 File.Delete 会直接抛，先去掉只读属性
                    var info = new FileInfo(file);
                    if (info.IsReadOnly) info.IsReadOnly = false;
                    File.Delete(file);
                }
                catch { }
            }
        }
        catch { }

        try
        {
            // 从最深的子目录往上删，父目录才可能空掉
            var dirs = Directory.GetDirectories(dir, "*", SearchOption.AllDirectories)
                .OrderByDescending(d => d.Length);
            foreach (var d in dirs)
            {
                try { Directory.Delete(d, recursive: true); } catch { }
            }
        }
        catch { }

        try { Directory.Delete(dir, recursive: true); } catch { }
    }

    internal static void DisableDwmTransitions(IntPtr hwnd)
    {
        try
        {
            int value = 1;
            DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, ref value, sizeof(int));
        }
        catch { }
    }

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        WriteCrashLog("UI thread", e.Exception);
        MessageBox.Show($"发生未处理异常:\n{e.Exception.Message}\n\n{e.Exception.StackTrace}",
            "KunClient 错误", MessageBoxButton.OK, MessageBoxImage.Error);
        e.Handled = true;
    }

    private void OnDomainUnhandledException(object sender, UnhandledExceptionEventArgs e)
    {
        WriteCrashLog(e.IsTerminating ? "fatal" : "non-UI", e.ExceptionObject as Exception);
    }

    private void OnUnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs e)
    {
        WriteCrashLog("unobserved task", e.Exception);
        e.SetObserved();
    }

    /// <summary>统一异常落盘入口，供各对话框 / 服务在 catch 中调用。</summary>
    internal static void WriteCrashLog(string source, Exception? ex)
    {
        try
        {
            var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KunClient");
            Directory.CreateDirectory(dir);
            var sb = new StringBuilder();
            sb.AppendLine($"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {source}");
            if (ex != null) sb.AppendLine($"  {ex}");
            File.AppendAllText(Path.Combine(dir, "crash.log"), sb.ToString());
        }
        catch { }
    }
}
