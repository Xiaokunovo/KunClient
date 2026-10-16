using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using KunFlashTool.App.Controls;
using KunFlashTool.App.Dialogs;
using KunFlashTool.App.Pages;
using KunFlashTool.App.Services;

namespace KunFlashTool.App;

public partial class MainWindow : Window
{
    // ═══════════════════════════════════════════════════════════════════
    // 导航模型：**分组 → 页签**（对应参考稿的菜单栏 + 页签条）
    //   · 分组：EDL操作 / 快捷操作 / 其他操作 —— 点菜单切换整排页签
    //   · 页签：分组内的页面；页面实例按分组缓存，切回来不重建
    //   · OwnRail：该页自带左栏（信息 + 日志）→ 外壳把共用左栏整列收起，
    //              否则会出现「两列设备信息 / 两个日志框」
    // ⚠️ 新增页面只改这一张表：不要再加第二套导航（图标工具条 / 侧边栏），
    //    否则又会回到「菜单 / 工具条 / 页签三层导航互相重复」的老问题。
    // ═══════════════════════════════════════════════════════════════════
    private sealed record PageDef(string Key, string Title, Func<UserControl> Factory, bool OwnRail = false);

    private static readonly Dictionary<string, PageDef[]> Groups = new()
    {
        // 2026-10-03 重分类：原先「EDL操作」只有 3 个页签、而「快捷操作」塞了 11 个，
        // 页签条一排放不下（1000 宽就溢出），而且刷机功能散在两个分组里。
        // 改后按「是不是刷机」重新分：**刷机快捷操作 7 个 / 常用工具 7 个 / 其他 2 个**。
        ["edl"] = new[]
        {
            // 2026-10-09：EDL 页**整套改为 SWT-EDL 的界面与功能** ——
            // 用户拍板「把 SWT-EDL 的 EDL 功能全部移植进来替换掉现有 EDL，UI 也改成一样的」。
            // 页面还是这个 key/类名（EdlFlashPage），但 XAML/代码后置是照
            // `third_party/swt-edl/src/ui/MainWindow.cpp` 重写的，底层换成 C# 逐文件移植的
            // `KunFlashTool.Core.SwtEdl.*`。决策与验收记录见 docs/swt-edl-integration.md。
            new PageDef("EdlFlash", "Qualcomm", () => new EdlFlashPage(), OwnRail: true),
            new PageDef("FastbootFlash", "FASTBOOT", () => new FastbootFlashPage(), OwnRail: true),
            new PageDef("Payload", "Payload", () => new PayloadPage(), OwnRail: true),
            // 「可视刷写」2026-10-03 已从页签里去掉，合并进 FASTBOOT 页内的模式切换 ——
            // 它的内容本来就是本页的子集（只有分区表 + 刷写进度，没有 ROM / 擦除 / 回读），
            // 两个页签并排会让用户不知道该点哪个。页签数 7 → 6。
            // 品牌定制刷机本质也是刷机，原先散在「快捷操作」里，一并归位
            new PageDef("OnePlusFlash", "欧加线刷", () => new OnePlusFlashPage(), OwnRail: true),
            new PageDef("XiaomiFastboot", "小米线刷", () => new XiaomiFastbootPage(), OwnRail: true),
            new PageDef("XiaomiEdl", "小米 EDL", () => new XiaomiEdlPage(), OwnRail: true),
        },
        ["quick"] = new[]
        {
            // ⚠️ OwnRail: true = 该页**自带左列**（信息 + 日志），外壳共用左栏必须让位，
            //    否则同屏会出现两条左栏、两个日志框。改版后自带左列的页面：
            //    主页 / 安卓工具 / 投屏 / 应用管理 / 文件传输，以及刷机组那七个。
            new PageDef("DeviceHome", "主页", () => new DeviceHomePage(), OwnRail: true),
            new PageDef("AndroidTools", "安卓工具", () => new AndroidToolsPage(), OwnRail: true),
            new PageDef("ScreenMirror", "投屏", () => new ScreenMirrorPage(), OwnRail: true),
            new PageDef("AppManager", "应用管理", () => new AppManagerPage(), OwnRail: true),
            new PageDef("FileTransfer", "文件传输", () => new FileTransferPage(), OwnRail: true),
            // 命令行页的「输出」本身就是终端的日志，云端驱动页没有日志区 —— 这两页继续用外壳共用左栏
            new PageDef("CommandConsole", "命令行", () => new CommandConsolePage()),
            new PageDef("CloudDriver", "云端驱动", () => new CloudDriverPage()),
        },
        ["other"] = new[]
        {
            // OwnRail: true = **本页不需要外壳共用左栏**（设备信息 + 日志），内容占满整宽。
            //   设置与关于页不写 PageLog（已确认），而「设备信息 / 日志」对配置和版本信息毫无意义，
            //   留着只是让这两页被压窄、左边空一大块 —— 用户反馈「很不舒服」的第一条就是这个。
            //   注意这条与「页面自带左栏」在实现上是同一个开关：都是把 _rail 收起（折叠后 Auto 列量成 0 宽）。
            new PageDef("Settings", "设置", () => new SettingsPage(), OwnRail: true),
            new PageDef("About", "关于", () => new AboutPage(), OwnRail: true),
        },
    };

    /// <summary>
    /// 启动分组：2026-10-03 从「刷机快捷操作 · Qualcomm」改到「常用工具 · 主页」。
    /// <para>
    /// 原因：原来打开就是一张**空分区表 + 空日志框**（没连设备时），用户第一眼看到的是
    /// 「什么都没有」；而连接状态、设备信息、实时读数、命令行入口全在「主页」里，
    /// 要再切一次菜单才看得到。用户反馈「用起来很不舒服」，根因就在这个启动落点。
    /// </para>
    /// <para>
    /// 参考稿（沐妍工具箱）是直接停 Qualcomm 的 —— 一致性让位于「打开就能用」。
    /// </para>
    /// </summary>
    private const string StartupGroup = "quick";

    /// <summary>启动时选中的页签。</summary>
    private const string StartupPageKey = "DeviceHome";

    private UserControl? _currentPage;
    private readonly Dictionary<string, UserControl> _pageCache = new();
    private readonly Dictionary<string, TabItem> _tabIndex = new();
    /// <summary>外壳共用左栏（设备信息 + 日志）。挂到 MainTabs.Tag 上时显示，置空时整列收起。</summary>
    private readonly DeviceLogRail _rail = new();

    private string _currentGroup = "";
    /// <summary>当前页签的页面定义（决定外壳共用左栏是否让位，见 <see cref="UpdateRailLayout"/>）。</summary>
    private PageDef? _currentDef;
    private string? _signature;
    private string? _vipExpire;
    /// <summary>会员等级（vip / svip / normal），服务端下发；为空时按到期时间推导。</summary>
    private string? _vipLevel;
    /// <summary>当前登录用户名。轮询刷新会员信息时要带着它同步给 MembershipService。</summary>
    private string? _username;
    private static readonly HttpClient _titleHttp = Services.ServerGate.CreateClient(TimeSpan.FromSeconds(10));
    private static string ProfileApiUrl => AppConfig.ServerUrl;
    private DispatcherTimer? _profileTimer;

    public MainWindow()
    {
        InitializeComponent();
        UpdateTitle();

        // 窗口最小化时停掉动态背景：GIF 是全窗口逐帧换图，DispatcherTimer 跑在 UI 线程上，
        // 不停的话最小化之后仍在按帧率重绘整窗（实测是最卡的单点之一）。
        StateChanged += (_, __) => DynamicBg.SetPlaying(WindowState != WindowState.Minimized);

        // 小窗口整体等比缩小（字号/间距/行高一起缩），大窗口恢复原尺寸。阈值附近有死区防抖。
        SizeChanged += (_, _) => { UpdateRailLayout(); UpdateUiScale(); };
        UpdateUiScale();
        _profileTimer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(30) };
        _profileTimer.Tick += async (_, _) => await RefreshSignatureAsync();
        _profileTimer.Start();
        _ = RefreshSignatureAsync();

        SwitchGroup(StartupGroup, StartupPageKey);
        ShellStatus.Changed += SyncShellStatus;
        SyncShellStatus();

        // 窗口变宽/变窄时左栏按门槛自动让位（瞬变路径，不补间）+ 小窗口整体缩放

        ApplyDynamicBackground();
        ApplyCardOpacity();
        ApplyUiTransparency();
        // 主题切换会重建 Theme.BgTint（写死的 alpha），切完必须按用户设置重新覆盖一次
        ThemeManager.ThemeChanged += theme =>
        {
            ApplyUiTransparency();
            // 主题切换会把 Theme.* 全部换成不透明的新画刷（A=255），
            // 这里必须**重新**应用一次卡片不透明度，否则用户调好的「卡片不透明度」
            // 会在切换主题后静默失效——卡片/输入框突然变回实心，正是反馈里的「切换深色主题不协调」。
            ApplyCardOpacity();
            // 日志段落里的 Run 画刷是渲染时定格的旧实例（时间戳压暗版、正文引用当时的
            // Foreground），主题换了它们不跟着变 —— 浅色的黑字留在深色主题里 = 整片看不见。
            PageLog.RecolorAll();
        };
        UpdateVersionLabel();
        Closed += MainWindow_Closed;
        Loaded += MainWindow_Loaded;
    }

    private void MainWindow_Closed(object? sender, EventArgs e)
    {
        // 统一在关闭时收尾：停止 30 秒轮询与动态背景动画，
        // 避免窗口关闭后定时器 / 媒体资源继续存活（进程退出统一在此触发）
        try { _profileTimer?.Stop(); _profileTimer = null; } catch { }
        try { DynamicBg.StopAnimation(); } catch { }
        // 静态事件必须摘：DeviceStateHub / ShellStatus 都是静态类，订阅者不摘会一直握着这个窗口
        try { DeviceStateHub.Changed -= OnDeviceStateChanged; } catch { }
        try { ShellStatus.Changed -= SyncShellStatus; } catch { }
        Application.Current.Shutdown();
    }

    private async void MainWindow_Loaded(object sender, RoutedEventArgs e)
    {
        _ = WindowAnimation.FadeInAsync(this, 280);
        if (_currentPage != null)
            _ = PageTransitionService.PlayEntranceAsync(_currentPage, 320);

        // 左栏「日志」接管全局镜像：所有页面的日志都会在这里出现一份。
        // 🔴 必须在 Loaded 里挂（而不是构造函数）：那时左栏的 RichTextBox 才真正在可视树里，
        //    PageLog 的渲染管线是按日志框引用取历史的，控件没建好就挂 = 白挂。
        try { PageLog.GlobalMirror = _rail.LogBox; } catch { }

        // 左栏「设备信息」：由**各页**探测后汇报（见 DeviceStateHub），外壳不自己轮询 ——
        // 外壳再起一套 adb/fastboot 会和页面正在跑的那条撞车。
        try
        {
            DeviceStateHub.Changed += OnDeviceStateChanged;
            if (DeviceStateHub.Current is { } st) ApplyDeviceState(st);
        }
        catch { }

        // 全局 USB 插拔钩子：设备切模式（ADB/Fastboot/EDL）时主页据此自动重探，
        // 不必再手动点「立即刷新」。幂等，重复调用无副作用。
        try { UsbDeviceNotifier.Initialize(this); } catch { }

        try
        {
            var update = await UpdateService.CheckForUpdateAsync();
            if (update != null)
            {
                if (update.ForceUpdate)
                {
                    // 强制更新：弹出更新窗口；仅在「未完成更新」（用户放弃或更新失败）时退出旧版本，
                    // 若更新已成功应用，UpdateService 内部已完成重启，不应再额外关闭主界面
                    var forceDialog = new UpdateDialog { Owner = this };
                    forceDialog.ShowDialog();
                    if (!forceDialog.UpdateApplied)
                        Application.Current.Shutdown();
                    return;
                }

                var result = MessageBox.Show(
                    $"发现新版本 v{update.Version}\n\n{update.Changelog}\n\n是否立即更新？",
                    "发现新版本",
                    MessageBoxButton.YesNo,
                    MessageBoxImage.Information);

                if (result == MessageBoxResult.Yes)
                {
                    var dialog = new UpdateDialog();
                    dialog.Owner = this;
                    dialog.ShowDialog();
                }
            }
        }
        catch { }
    }

    public void SetUserInfo(string username, string signature = "", string vipExpire = "", string vipLevel = "")
    {
        _signature = signature;
        _vipExpire = vipExpire;
        _vipLevel = vipLevel;
        _username = username;
        // 同步给会员服务：功能分级/各等级额度都从这里判断，别各自解析一次到期时间
        MembershipService.SetUser(username, vipExpire, vipLevel);
        UpdateTitle();
    }

    private void UpdateTitle()
    {
        // 版本号显示成 `Ver:1.0.9`（用户要求，带英文冒号、冒号后不留空格）。
        // ⚠️ 值仍取自 UpdateService.CurrentVersion，**别在标题里手写版本串** —— 否则发版时会漏改这一处。
        var baseTitle = $"KunClient Ver:{UpdateService.CurrentVersion} by 小坤ovo";

        // 会员信息要「一直显示」：拿不到到期日/等级时也要给出明确状态（普通用户），
        // 不能整段消失——用户会以为会员信息丢了。
        baseTitle += $" · {AccountText.VipTitle(_vipLevel, _vipExpire, _signature)}";

        Title = baseTitle;
    }

    private void UpdateVersionLabel()
    {
        // 版本号在标题里已经完整显示（含作者与会员信息），菜单栏右侧只补一个短读数；
        // ⚠️ 值仍取自 UpdateService.CurrentVersion，别在这里手写版本串。
        if (TxtVersion != null)
            TxtVersion.Text = $"v{UpdateService.CurrentVersion}";
    }

    /// <summary>供页面更新外壳读数（设备名 / 模式 / 提示）。实现落在 ShellStatus（底部状态栏 + 左栏）。</summary>
    public void SetStatus(string? device, string? mode, string? hint)
        => ShellStatus.SetDevice(device, mode, hint);

    /// <summary>左栏「设备信息」与状态栏「设备」段：由 DeviceStateHub 广播驱动（谁探测谁汇报）。</summary>
    private void OnDeviceStateChanged(DeviceStateHub.State st)
    {
        if (!Dispatcher.CheckAccess())
        {
            Dispatcher.BeginInvoke(new Action(() => ApplyDeviceState(st)));
            return;
        }
        ApplyDeviceState(st);
    }

    private void ApplyDeviceState(DeviceStateHub.State st)
    {
        try
        {
            if (StatusDeviceText != null) StatusDeviceText.Text = st.Device;
            StatusDot.Background = (Brush)FindResource(IsConnected(st.Mode) ? "Theme.Accent" : "Theme.TextTertiary");
        }
        catch { }
    }

    /// <summary>「未连接 / 未检测 / 未知」都算未连接 —— 状态点只在真连上时才亮。</summary>
    private static bool IsConnected(string? mode)
        => !string.IsNullOrWhiteSpace(mode)
           && !mode.Contains("未连接", StringComparison.Ordinal)
           && !mode.Contains("未检测", StringComparison.Ordinal)
           && !mode.Contains("未知", StringComparison.Ordinal);

    /// <summary>外壳读数 → 底部双进度条 + 分段状态栏。</summary>
    private void SyncShellStatus()
    {
        try
        {
            var percent = ShellStatus.Percent;
            BarTotal.Value = percent;
            // 第二条只在「当前分区进度 ≠ 总进度」时才显示：单分区写入/擦除时两者恒等，
            // 照搬会画成两条同步的进度条（看起来像 bug）。2026-10-02 修复：页面上报 -1 表示
            // 「这条别画」，原先被 Clamp 成 0 → 卡死的 0% 第二条。这里直接整条收起。
            BarPart.Visibility = ShellStatus.PartitionBarVisible ? Visibility.Visible : Visibility.Collapsed;
            BarPart.Value = ShellStatus.PartitionPercent;
            BarPart.ShowText = Math.Abs(ShellStatus.PartitionPercent - percent) > 0.5;

            // 耗时不单独占槽位（参考稿状态栏只有 6 段），并进「任务」段显示
            var elapsed = ShellStatus.Elapsed;
            StatusTaskText.Text = string.IsNullOrEmpty(elapsed)
                ? ShellStatus.Task
                : $"{ShellStatus.Task}  {elapsed}";
            StatusPartitionText.Text = ShellStatus.Partition;
            StatusSpeedText.Text = ShellStatus.Speed;
            StatusTotalText.Text = ShellStatus.TotalSize;
            StatusPercentText.Text = $"{percent:F0}%";
        }
        catch
        {
            // 读数刷新不该影响任何功能路径
        }
    }

    /// <summary>从服务器拉取个性签名，驱动窗口标题实时刷新。</summary>
    private async System.Threading.Tasks.Task RefreshSignatureAsync()
    {
        try
        {
            var token = AuthSession.ResolveToken();
            if (string.IsNullOrEmpty(token)) return;

            using var req = new HttpRequestMessage(HttpMethod.Get, $"{ProfileApiUrl}/api/v1/profile");
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
            using var resp = await _titleHttp.SendAsync(req);

            // 响应体只读一次（后面还要用它解析字段），所以先整体读出来再分支。
            var json = await resp.Content.ReadAsStringAsync();

            // 被管理员强制下线（401 + code=KICKED）：必须在「静默忽略非 2xx」之前判掉 ——
            // 否则这条 401 会和「token 自然过期」一样被无声吞掉，用户继续操作却不知道自己已被踢。
            if (ForceLogout.IsKicked((int)resp.StatusCode, json, out var kickMsg))
            {
                ForceLogout.Trigger(kickMsg);
                return;
            }

            if (!resp.IsSuccessStatusCode) return;

            using var doc = JsonDocument.Parse(json);
            var result = doc.RootElement;
            // 服务端返回结构异常（缺少 success/data/signature 字段）时直接跳过，
            // 避免 GetProperty 抛 KeyNotFoundException 导致每次轮询都走异常分支
            if (!result.TryGetProperty("success", out var okEl) || okEl.ValueKind != JsonValueKind.True)
                return;
            if (!result.TryGetProperty("data", out var dataEl) ||
                !dataEl.TryGetProperty("signature", out var sigEl))
                return;

            var sig = sigEl.ValueKind == JsonValueKind.String ? sigEl.GetString() : null;
            // 同步会员到期时间：管理员在控制台把账号改成「永久」后，客户端标题
            // 也会在下一轮轮询（≤30s）自动刷新，无需重新登录。
            // ⚠️ /api/profile 返回的是**表列名 `vip_expire_at`（蛇形）**，不是 camelCase。
            // 早期这里只读 `vipExpireAt` → 永远取不到 → vipRaw 恒为 null →
            // 「刚登录显示永久，进主窗口后立刻变成未开通」，而且会把 null 灌给
            // MembershipService，导致永久会员丢掉会员身份、被按免费额度限次。
            // 现在兼容两种写法；字段缺失或为 null 时**保持原值**，绝不用 null 覆盖。
            var vipRaw = _vipExpire;
            if (dataEl.TryGetProperty("vip_expire_at", out var veEl) && veEl.ValueKind == JsonValueKind.String)
                vipRaw = veEl.GetString();
            else if (dataEl.TryGetProperty("vipExpireAt", out var veEl2) && veEl2.ValueKind == JsonValueKind.String)
                vipRaw = veEl2.GetString();
            // 会员等级直接用服务端下发的 vipLevel（vipLevelLabel 是「高级会员/特权会员」成品文案，
            // 但等级 key 更稳定；AccountText 会按同一张表翻译成显示名）。
            var lvRaw = dataEl.TryGetProperty("vipLevel", out var lvEl) && lvEl.ValueKind == JsonValueKind.String
                ? lvEl.GetString()
                : null;
            var changed = false;
            var memberChanged = false;
            if (!string.Equals(sig, _signature, StringComparison.Ordinal)) { _signature = sig; changed = true; }
            if (lvRaw != null && !string.Equals(lvRaw, _vipLevel, StringComparison.Ordinal))
            {
                _vipLevel = lvRaw;
                changed = true;
                memberChanged = true;
            }
            if (!string.Equals(vipRaw, _vipExpire, StringComparison.Ordinal))
            {
                _vipExpire = vipRaw;
                changed = true;
                memberChanged = true;
            }
            // 等级或到期时间「任一变」都要同步给会员服务：只认到期时间的话，
            // 管理员把 VIP 调成 SVIP（到期时间不变）时客户端会继续按旧等级算额度。
            if (memberChanged)
                MembershipService.SetUser(_username, _vipExpire, _vipLevel);
            if (changed)
                await Dispatcher.InvokeAsync(UpdateTitle);
        }
        catch { }
    }

    private async void ThemeToggle_Click(object sender, RoutedEventArgs e)
    {
        // 主题切换会把整套 Theme.* 画刷一次性替换掉（几十个键同时跳色）＝用户说的"硬切"。
        // 交给柔洗过渡：先用目标底色把整屏覆盖，在覆盖态里换调色板，再褪去露出新主题。
        try
        {
            await ThemeTransitionService.ToggleAnimatedAsync(ThemeWash, () => { });
        }
        catch
        {
            // 兜底：过渡层出问题也必须完成切换，不能把界面卡在半路
            ThemeManager.Toggle();
            try { ThemeWash.Visibility = Visibility.Collapsed; ThemeWash.Opacity = 0; } catch { }
        }
    }

    private void NavUpdate_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new UpdateDialog();
        dialog.Owner = this;
        dialog.ShowDialog();
    }

    // ═══════════════════════════════════════════════════════════════════
    // 导航：菜单分组（切换整排页签）+ 页签（切换页面）
    // ═══════════════════════════════════════════════════════════════════

    private void MenuGroup_Click(object sender, RoutedEventArgs e)
    {
        if (sender is MenuItem { Tag: string key }) SwitchGroup(key);
    }

    /// <summary>切换分组：重建这一排页签，并选中 <paramref name="pageKey"/>（为空则第一个）。</summary>
    private void SwitchGroup(string key, string? pageKey = null)
    {
        if (!Groups.TryGetValue(key, out var defs)) return;

        if (!string.Equals(_currentGroup, key, StringComparison.Ordinal))
        {
            _currentGroup = key;
            MainTabs.Items.Clear();
            _tabIndex.Clear();
            // ⚠️ 页面实例**不随分组丢弃**（_pageCache 保留）：刷写/备份这类长操作是在页面对象里
            //    持有会话与状态的，切一次菜单就把它扔掉 = 用户切回来看到「未连接」，
            //    而后台其实还在写分区 —— 状态和界面直接对不上。
            //    页签项重建、页面复用，切回来时表格与日志都还在。
        }

        // 菜单高亮：同一时刻只有一个分组是选中态（对应参考稿里高亮的「EDL操作」）
        MenuEdl.IsChecked = key == "edl";
        MenuQuick.IsChecked = key == "quick";
        MenuOther.IsChecked = key == "other";

        foreach (var def in defs) EnsureTab(def);

        var target = pageKey ?? defs[0].Key;
        if (_tabIndex.TryGetValue(target, out var tab)) MainTabs.SelectedItem = tab;
    }

    private void EnsureTab(PageDef def)
    {
        if (_tabIndex.ContainsKey(def.Key)) return;

        var page = GetPage(def);
        var tab = new TabItem
        {
            Header = def.Title,
            Content = page,
            Style = (Style)FindResource("ToolTabItem"),
        };
        _tabIndex[def.Key] = tab;
        MainTabs.Items.Add(tab);
    }

    private UserControl GetPage(PageDef def)
    {
        if (_pageCache.TryGetValue(def.Key, out var cached)) return cached;
        var page = def.Factory();
        _pageCache[def.Key] = page;
        return page;
    }

    private void MainTabs_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        _currentPage = (MainTabs.SelectedItem as TabItem)?.Content as UserControl;
        _currentDef = CurrentDef();
        UpdateRailLayout();

        // 换页签 = 换任务：清掉上一页留下的任务读数（设备读数保留，那是全局状态）。
        // 2026-10-02 修复：有任务**正在跑**时不清 —— 页面是按页签缓存常驻的，后台刷写并不
        // 因换页签停止；此时 Reset 会把进行中任务的进度读数抹成 0，直到下一次进度回调才恢复，
        // 慢阶段会出现「进度条突然清零」的假象。空闲时才重置。
        if (ShellStatus.Percent <= 0 || ShellStatus.Percent >= 100)
            ShellStatus.Reset();

        if (_currentPage is { } page)
            _ = PageTransitionService.PlayEntranceAsync(page, 240);
    }

    /// <summary>当前选中页签对应的页面定义（用于判断它是否自带左栏）。</summary>
    private PageDef? CurrentDef()
    {
        if (!Groups.TryGetValue(_currentGroup, out var defs)) return null;
        foreach (var def in defs)
            if (_tabIndex.TryGetValue(def.Key, out var tab) && ReferenceEquals(tab, MainTabs.SelectedItem))
                return def;
        return null;
    }

    /// <summary>菜单「读取设备信息」：重新进入当前页一次，触发页面重新读取设备信息。</summary>
    private async void ToolbarRefresh_Click(object sender, RoutedEventArgs e)
    {
        if (_currentPage == null) return;
        // 页面都没有独立的「刷新」对外合约，统一走一次入场重放：
        // PageTransitionService 的入场会重新走高亮/状态读取路径，等价于点一次导航。
        await PageTransitionService.PlayEntranceAsync(_currentPage, 240);
    }

    /// <summary>
    /// 供页面内部跳转到另一个页面（例如主页快捷操作里的「命令行」）。
    /// 会先切到该页面所在的分组，再选中对应页签。
    /// </summary>
    public void NavigateTo(string tag)
    {
        foreach (var (group, defs) in Groups)
        {
            foreach (var def in defs)
            {
                if (!string.Equals(def.Key, tag, StringComparison.Ordinal)) continue;
                SwitchGroup(group, tag);
                return;
            }
        }
    }

    /// <summary>
    /// 共用左栏（设备信息 + 日志）的让位规则。
    ///
    /// 两个输入：① 当前分组是否自带左栏（EDL操作 三页都自带）；
    ///          ② 窗口宽度还够不够 —— 左栏要吃掉 324px，窄到一定程度页面会被压出问题。
    /// 🔴 左栏挂在 MainTabs.Tag 上，**首次挂上后不再摘**，之后只切 _rail.Visibility。
    ///    折叠时 Auto 列量到的 DesiredSize 就是 0（与置空 Tag 等价，railprobe 实测 col0=0），
    ///    所以这里既不会多占一条，又省掉每次切页签的整列卸载/重挂。
    /// 🔴 这是 SizeChanged 驱动的**瞬变路径**：不补间（每帧都会进来），直接落值。
    /// </summary>
    private const double RailWidth = 324;
    /// <summary>带左栏时页面区的最小可用宽（实测下限：834 通过 / 670 出问题，取 830 留 4px 余量）。</summary>
    private const double PageMinWidthWithRail = 830;
    private bool? _railShown;

    // ═══════════════ 小窗口整体等比缩小（2026-10-03） ═══════════════
    //  用户反馈「窗口 ui 分辨率过小、字体偏大」，数出 12 处观感问题。
    //  逐处调 FontSize 必然漏、且每处都要单独回归；这里在**根 Grid 上挂 LayoutTransform**，
    //  字号 / 行距 / 内边距 / 表格行高一起等比缩 —— 一处生效、全局一致。
    //  WPF 在变换之后才光栅化文字，缩放后的字仍是矢量清晰的（不像位图缩放会糊）。

    private double _uiScale = 1.0;
    private double _lastScaleW = -1, _lastScaleH = -1;

    /// <summary>
    /// 按客户区尺寸选缩放档位并落到 <c>UiScale</c>。
    /// <para>
    /// 🔴 阈值附近必须留**死区**（20px）：否则在临界点拖动窗口会看到字号反复跳动，比不缩还难受。
    /// 🔴 这是 SizeChanged 驱动的**瞬变路径**（每帧都可能进来），所以只做比较 + 赋值，不补间。
    /// </para>
    /// </summary>
    private void UpdateUiScale()
    {
        double w = ActualWidth > 0 ? ActualWidth : Width;
        double h = ActualHeight > 0 ? ActualHeight : Height;
        if (double.IsNaN(w) || double.IsNaN(h) || w <= 0 || h <= 0) return;

        // 死区：尺寸变化不足 20px 就沿用上一次的判定
        if (Math.Abs(w - _lastScaleW) < 20 && Math.Abs(h - _lastScaleH) < 20) return;
        _lastScaleW = w;
        _lastScaleH = h;

        // 窗口最小尺寸是 1000×640（XAML 的 MinWidth/MinHeight），
        // 所以最小档就是"刚好贴着最小尺寸"这一档。
        var s = (w >= 1160 && h >= 740) ? 1.00
              : (w >= 1060) ? 0.93
              : 0.87;
        if (Math.Abs(s - _uiScale) < 0.001) return;

        _uiScale = s;
        UiScale.ScaleX = s;
        UiScale.ScaleY = s;
    }

    private void UpdateRailLayout()
    {
        try
        {
            double w = ActualWidth;
            if (double.IsNaN(w) || w <= 0) w = double.IsNaN(Width) ? 1280 : Width;   // 构造期尚未测量

            // 当前页自带左栏（DeviceLogRail 同款「信息 + 日志」列）时，外壳这一列整块让位；
            // 判据来自 PageDef.OwnRail —— 页面改版后自带左列就必须在这里标 true，
            // 否则会出现两条左栏、两个日志框。
            bool pageWantsRail = _currentPage != null && _currentDef is { OwnRail: false };
            bool show = pageWantsRail && (w - RailWidth) >= PageMinWidthWithRail;
            if (_railShown == show) return;
            _railShown = show;

            // 🚀 左栏**只挂一次 MainTabs.Tag，之后只切显隐**（原来是 show ? _rail : null，
            //    每次切页签都把整列从可视树摘掉再挂回来 → DeviceLogRail 走一遍 Unloaded/Loaded
            //    （静态事件摘了又挂）、左栏那 2000 行日志框整篇重排，正是「切页签卡一下」的一部分。
            //    ⚠️ 旧注释写「不要改成只设 Visibility —— 那样列还占宽度」，实测不成立：
            //    Auto 列量的是子元素 DesiredSize，折叠元素 DesiredSize=0，
            //    见 .workbuddy\tmp\railprobe（Content=null→0 / Visible→316 / Collapsed→0 / presenter 折叠→0）。
            //    折叠后子树完全不参与 Measure，视觉与摘掉逐像素一致。
            if (!ReferenceEquals(MainTabs.Tag, _rail)) MainTabs.Tag = _rail;
            _rail.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        }
        catch { }
    }

    public void ApplyDynamicBackground()
    {
        var settings = AppSettings.Current;
        // 配置里的路径可能因文件被改名/移动而失效，先做一次自愈解析（并在设置界面提示）
        var path = settings.ResolveBackgroundPath();
        var hasBackground = !string.IsNullOrEmpty(path) && System.IO.File.Exists(path);

        // 没有壁纸时玻璃读不出来：面板与窗底都是近白，上沿高光/下沿暗影全对白，
        // 于是整页看着就是一块死白 —— 用户反馈的「液态玻璃效果没有实现」就是这条。
        // 交给 ThemeManager 给描边与面板底色加一档对比（值没变时内部直接返回，不会抖）。
        ThemeManager.SetGlassContrastBoost(!hasBackground);

        // 🔴 2026-10-08（iOS 液态玻璃）：没有壁纸时**不再收起** DynamicBg。
        //    它那一层里画着「环境底色」（DynamicBackground.BuildAmbientBloom：四团柔和色晕），
        //    正是玻璃唯一的衬底 —— 收起来 = 窗底纯白，半透明面板叠上去什么都读不出来。
        //    这就是「液态玻璃效果没实现」的最后一块：玻璃的账不是画在面板上的，是算在**背后那层**的。
        DynamicBg.Visibility = Visibility.Visible;

        // 控件尚未进入可视树时（MainWindow 构造阶段）只决定可见性，
        // 真正的媒体加载交给 DynamicBackground.OnLoaded。否则 Source 会被连续设置两次，
        // MediaElement 在媒体尚未打开时重启加载管线会偶发 MediaFailed，背景整块消失。
        if (!DynamicBg.IsLoaded)
            return;

        if (hasBackground)
        {
            DynamicBg.LoadMedia(path);
            DynamicBg.ApplyAnimation(settings.BackgroundAnimation, settings.BackgroundOpacity);
        }
        else
        {
            DynamicBg.StopAnimation();
            DynamicBg.ApplyAnimation("None", 0);
        }
    }

    /// <summary>
    /// 按设置改写「页面根背景」Theme.BgTint 的 alpha —— 这就是全局透明度的唯一落点。
    /// 所有页面根节点都用 <c>Background="{DynamicResource Theme.BgTint}"</c>，
    /// 所以改这一支画刷即可让整个界面的透明度同时生效。
    /// <para>语义：UiTransparency 0 = 界面完全不透明（看不到背景），1 = 完全透明（背景全露）。</para>
    /// </summary>
    public void ApplyUiTransparency()
    {
        var transparency = Math.Clamp(AppSettings.Current.UiTransparency, 0.0, 1.0);
        if (Application.Current.Resources["Theme.Bg"] is not SolidColorBrush bg) return;

        // 底色跟随当前主题的 Theme.Bg，只改 alpha
        var alpha = (byte)Math.Round((1.0 - transparency) * 255);
        var tint = new SolidColorBrush(Color.FromArgb(alpha, bg.Color.R, bg.Color.G, bg.Color.B));
        tint.Freeze();
        Application.Current.Resources["Theme.BgTint"] = tint;
    }

    /// <summary>按设置调整卡片/面板不透明度（基于首次捕获的原始颜色，避免反复叠加降低透明度）</summary>
    private static readonly Dictionary<string, Color> _baseSurfaceColors = new();

    /// <summary>
    /// 参与「卡片不透明度」的底色刷。必须把日志框用的 Theme.SurfaceAlt 一起算进来，
    /// 否则用户把卡片调透明后，日志面板还是一块不透明的深色——正是反馈里说的「不和谐」。
    /// <para>
    /// 同理必须包含 <c>Theme.InputBg</c>：TextBox / PasswordBox / ComboBox / CheckBox
    /// 的底色都用它（见 ToolboxStyles.xaml），漏掉它就会出现「卡片都透了、选择框还是一片死白」
    /// 的割裂感——即反馈里的「选择框不能一起透明」。表格表头 / 页签条 / 状态栏底色
    /// （PanelHeadBg / TabStripBg / StatusBarBg）也必须一起算：它们是外壳大面积底色，
    /// 漏掉就等于「面板透了、表头和状态栏还是实心」。
    /// </para>
    /// 只改背景（这些刷都只作用于 Background/Fill），字体颜色不变。
    /// </summary>
    private static readonly string[] _surfaceKeys =
    {
        "Theme.Surface", "Theme.SurfaceAlt", "Theme.CardBg", "Theme.SurfaceElevated",
        "Theme.InputBg", "Theme.PanelHeadBg", "Theme.TabStripBg", "Theme.StatusBarBg"
    };

    public void ApplyCardOpacity()
    {
        // 下限放到 0：用户可以真的把卡片调到全透明（只留背景），不再被写死的 0.2 挡住
        var opacity = Math.Clamp(AppSettings.Current.CardOpacity, 0.0, 1.0);
        foreach (var key in _surfaceKeys)
        {
            if (Application.Current.Resources[key] is not SolidColorBrush brush) continue;

            // 首次记录原始颜色（未应用透明度前），之后始终从原始颜色计算。
            // 「主题换没换」按 **RGB** 认，不能再看 alpha —— 玻璃化之后基准色本身就是
            // 半透明的（A 永远不是 255），拿 alpha 判新旧会一直误判、基准色永远不刷新。
            // 主题切换会整支换掉画刷，RGB 随之改变，这时才刷新基准色。
            if (!_baseSurfaceColors.TryGetValue(key, out var baseColor)
                || baseColor.R != brush.Color.R || baseColor.G != brush.Color.G || baseColor.B != brush.Color.B)
            {
                _baseSurfaceColors[key] = brush.Color;
                baseColor = brush.Color;
            }

            // 在**玻璃基准 alpha** 上再乘用户滑条：滑条 = 1 → 令牌原样（玻璃还在），
            // = 0 → 真的全透明。旧实现直接写 opacity*255，会把玻璃 alpha 一笔冲掉，
            // 用户只要没动过滑条（默认 0.86）玻璃就永远显不出来。
            var alpha = (byte)Math.Round(Math.Clamp(opacity, 0.0, 1.0) * baseColor.A);
            Application.Current.Resources[key] =
                new SolidColorBrush(Color.FromArgb(alpha, baseColor.R, baseColor.G, baseColor.B));
        }
    }
}
