using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public partial class AppManagerPage : BasePage
    {
        private string _selectedPackage = "";
        // 2026-10-02 修复（配套异步化）：搜索框的防抖计时器 —— 原先每敲一个字符就同步跑一轮
        // `adb shell pm list packages`（1~3 秒，UI 线程直接冻结），连打 6 个字符 = 6 次连环冻结。
        private System.Windows.Threading.DispatcherTimer? _searchDebounce;

        public AppManagerPage()
        {
            InitializeComponent();

            // 页内左栏「设备信息」读数：本页在 MainWindow 页表里标了 OwnRail:true，
            // 外壳共用左栏（Controls/DeviceLogRail）对本页整列让位，所以读数由页面自己显示。
            // 数据来源与外壳左栏完全一致 —— 设备/模式 ← DeviceStateHub（谁探测谁汇报，
            // 本页自己不探测，显示最近一次广播）；任务/进度/状态 ← ShellStatus
            // （各操作的 PageLog.Section 已顺带把任务名上报）。
            // 静态事件必须在 Unloaded 摘除：页面销毁后不摘会一直被回调（照 DeviceLogRail 的写法）。
            Loaded += (_, _) =>
            {
                ShellStatus.Changed += SyncRailStatus;
                DeviceStateHub.Changed += SyncRailDevice;
                SyncRailStatus();
                if (DeviceStateHub.Current is { } st) SyncRailDevice(st);
            };
            Unloaded += (_, _) =>
            {
                ShellStatus.Changed -= SyncRailStatus;
                DeviceStateHub.Changed -= SyncRailDevice;
                _searchDebounce?.Stop();
            };
        }

        /// <summary>DeviceStateHub 广播 → 左栏「设备信息」的 序列号 / 设备模式 两格。</summary>
        private void SyncRailDevice(DeviceStateHub.State st)
        {
            try
            {
                if (!Dispatcher.CheckAccess())
                {
                    Dispatcher.BeginInvoke(new Action(() => SyncRailDevice(st)));
                    return;
                }
                TxtRailDevice.Text = st.Device;
                TxtRailMode.Text = st.Mode;
            }
            catch
            {
                // 读数刷新失败不影响任何功能路径：控件卸载后回调仍可能到（事件晚一步），忽略
            }
        }

        /// <summary>ShellStatus → 左栏「设备信息」的 任务 / 进度 / 状态 三格。</summary>
        private void SyncRailStatus()
        {
            try
            {
                TxtRailTask.Text = ShellStatus.Task;
                TxtRailPercent.Text = $"{ShellStatus.Percent:F0}%";
                TxtRailHint.Text = ShellStatus.Hint;
            }
            catch
            {
                // 同上：读数刷新失败不影响功能
            }
        }

        private void TxtSearch_TextChanged(object sender, TextChangedEventArgs e)
        {
            // 2026-10-02 修复：300ms 防抖 —— 停止输入半秒后才真正查询，与异步化配合，
            // 彻底消除「每键一卡」。计时器只建一次、Tick 只挂一次（不能每键重挂）。
            if (_searchDebounce == null)
            {
                _searchDebounce = new System.Windows.Threading.DispatcherTimer
                {
                    Interval = TimeSpan.FromMilliseconds(300)
                };
                _searchDebounce.Tick += SearchDebounced_Tick;
            }
            _searchDebounce.Stop();
            _searchDebounce.Start();
        }

        private async void SearchDebounced_Tick(object? sender, EventArgs e)
        {
            _searchDebounce?.Stop();
            await LoadAppsAsync();
        }

        private void BtnSearch_Click(object sender, RoutedEventArgs e) => _ = LoadAppsAsync();

        private async Task LoadAppsAsync()
        {
            var showSystem = ChkShowSystem.IsChecked == true;
            var filter = showSystem ? "" : "-3";
            var output = await RunAdbCommandAsync($"shell pm list packages {filter}");

            // 🔴 一次性换 ItemsSource，不再 Clear() + 逐个 Items.Add：
            //    包列表动辄两三百条，逐条 Add = 两三百次集合变更通知，DataGrid 每次都插一行容器
            //    并当场重排（O(n²)），即使开了行虚拟化也照样卡；合成一个 List 只发一次通知。
            var rows = new System.Collections.Generic.List<string>();
            if (!string.IsNullOrEmpty(output))
            {
                var search = TxtSearch.Text?.Trim().ToLower();   // 提到循环外（原来每个包都算一次）
                foreach (var line in output.Split('\n'))
                {
                    if (!line.StartsWith("package:")) continue;
                    var pkg = line.Substring(8).Trim();
                    if (string.IsNullOrEmpty(search) || pkg.ToLower().Contains(search))
                    {
                        // 列表控件由 ListBox 换成 DataGrid（经典版式要求表格，见 AppManagerPage.xaml）：
                        // 行数据直接是**包名字符串**，XAML 里「包名」列用 Binding="{Binding}" 显示；
                        // 不再包一层 ListBoxItem，也就不用再 FindResource 取正文色（配色交给 ThemedDataGrid）。
                        rows.Add(pkg);
                    }
                }
            }
            LstApps.ItemsSource = rows;
        }

        private async void LstApps_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            // DataGrid 的行数据就是包名字符串（XAML 侧「包名」列 Binding="{Binding}" 直接显示它）
            if (LstApps.SelectedItem is string pkg)
            {
                _selectedPackage = pkg;
                TxtPackageName.Text = _selectedPackage;
                var version = await RunAdbCommandAsync($"shell dumpsys package {_selectedPackage} | grep versionName");
                TxtVersion.Text = version?.Trim().Split('=') is { Length: > 1 } parts ? parts[1] : "-";
            }
        }

        private async void BtnFreezeApp_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrEmpty(_selectedPackage))
            {
                if (!Services.MembershipService.GuardAndConsume("应用管理", "app.manage")) return;
                await RunAdbCommandAsync($"shell pm disable-user --user 0 {_selectedPackage}", $"冻结: {_selectedPackage}");
            }
        }

        private async void BtnUnfreezeApp_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrEmpty(_selectedPackage))
            {
                if (!Services.MembershipService.GuardAndConsume("应用管理", "app.manage")) return;
                await RunAdbCommandAsync($"shell pm enable {_selectedPackage}", $"解冻: {_selectedPackage}");
            }
        }

        private async void BtnExtractApk_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrEmpty(_selectedPackage))
            {
                if (!Services.MembershipService.GuardAndConsume("应用管理", "app.manage")) return;
                var output = await RunAdbCommandAsync($"shell pm path {_selectedPackage}");
                if (!string.IsNullOrEmpty(output))
                {
                    var path = output.Split('\n')[0].Replace("package:", "").Trim();
                    await RunAdbCommandAsync($"pull \"{path}\" .", $"提取: {_selectedPackage}");
                }
            }
        }

        private async void BtnClearData_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrEmpty(_selectedPackage))
            {
                if (!Services.MembershipService.EnsureAllowed("应用管理", "app.manage")) return;
                if (MessageBox.Show($"确定清除 {_selectedPackage} 的数据？", "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
                {
                    if (!Services.MembershipService.Consume("app.manage")) return;
                    await RunAdbCommandAsync($"shell pm clear {_selectedPackage}", $"清除数据: {_selectedPackage}");
                }
            }
        }

        private async void BtnUninstallApp_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrEmpty(_selectedPackage))
            {
                if (!Services.MembershipService.EnsureAllowed("应用管理", "app.manage")) return;
                if (MessageBox.Show($"确定卸载 {_selectedPackage}？", "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
                {
                    if (!Services.MembershipService.Consume("app.manage")) return;
                    await RunAdbCommandAsync($"uninstall {_selectedPackage}", $"卸载: {_selectedPackage}");
                }
            }
        }

        private async void BtnBackupApk_Click(object sender, RoutedEventArgs e)
        {
            if (!string.IsNullOrEmpty(_selectedPackage))
            {
                var output = await RunAdbCommandAsync($"shell pm path {_selectedPackage}");
                if (!string.IsNullOrEmpty(output))
                {
                    var path = output.Split('\n')[0].Replace("package:", "").Trim();
                    var dialog = new SaveFileDialog { FileName = $"{_selectedPackage}.apk" };
                    if (dialog.ShowDialog() == true)
                    {
                        await RunAdbCommandAsync($"pull \"{path}\" \"{dialog.FileName}\"", $"备份: {_selectedPackage}");
                    }
                }
            }
        }

        private async Task<string> RunAdbCommandAsync(string arguments, string description = "")
        {
            // 日志「三明治」（参考稿日志区的形状；与 FileTransferPage.RunAdbCommandAsync 同一做法）：
            // 带描述的操作（冻结 / 卸载 / 浏览目录…）先起一个区段标题 —— PageLog.Section 同时把
            // 外壳状态栏的「任务」段刷成这个名字；结尾在 finally 里补右对齐的耗时收尾行，
            // 成功 / 失败 / 中途 return 都不会漏。纯加载（LoadApps 的过滤查询没有描述）不打区段，
            // 避免搜索框每敲一个字符就刷一条标题。
            //
            // 2026-10-02 修复：整个方法异步化 —— 原先是同步 Process + ReadToEnd，在 UI 线程上
            // 直接跑（列表查询 1~3 秒 = 界面冻结 1~3 秒）；stderr 只 Redirect 不读（管道写满即死锁）；
            // 没有超时（挂死的 adb 永远占住）。两路并行异步读 + 30 秒强杀兜底。
            var withSection = !string.IsNullOrEmpty(description);
            var sw = Stopwatch.StartNew();
            if (withSection) PageLog.Section(LogTarget, description);
            try
            {
                if (withSection) LogInfo($"[ADB] {description}: adb {arguments}");
                using var process = new Process
                {
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = "adb",
                        Arguments = arguments,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        UseShellExecute = false,
                        CreateNoWindow = true
                    }
                };
                process.Start();
                var stdoutTask = process.StandardOutput.ReadToEndAsync();
                var _ = process.StandardError.ReadToEndAsync();   // 持续排空 stderr，防管道死锁
                var timeout = System.Threading.Tasks.Task.Run(async () =>
                {
                    await process.WaitForExitAsync();
                    return true;
                });
                var finished = await System.Threading.Tasks.Task.WhenAny(timeout, System.Threading.Tasks.Task.Delay(30000));
                if (finished != timeout)
                {
                    try { process.Kill(true); } catch { }
                    LogError("[ADB错误] 命令 30 秒无响应，已强制结束");
                }
                return await stdoutTask;
            }
            catch (Exception ex)
            {
                LogError($"[ADB错误] {ex.Message}");
                return "";
            }
            finally
            {
                // 成败都要收尾：区段标题开着不关，日志区看起来像卡在半路
                if (withSection) PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }
    }
}
