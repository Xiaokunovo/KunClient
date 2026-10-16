using System;
using System.Diagnostics;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public partial class DeviceHomePage : BasePage
    {
        private readonly DispatcherTimer _autoRefreshTimer = new();
        private bool _isAutoRefreshEnabled;
        private string _currentMode = "none"; // none, adb, fastboot, recovery, edl

        // 设备切模式（ADB/Fastboot/EDL）时，Windows 会重新枚举 USB 接口并广播 WM_DEVICECHANGE。
        // 主页订阅 UsbDeviceNotifier，去抖后自动重探 —— 不再需要手动点「立即刷新」。
        private readonly DispatcherTimer _usbDebounce = new();
        // 探测是整串串行 adb/fastboot 调用，耗时以秒计；用闸门防并发重入把 UI 打花。
        private bool _detectRunning;
        private bool _detectPending;

        public DeviceHomePage()
        {
            InitializeComponent();
            Loaded += OnLoaded;
            // 2026-10-02 修复：Tick 处理器只在构造时挂**一次**。原先每开一次自动刷新都
            // `Tick +=` 叠一个新处理器 —— 开关 N 次后每个 5 秒 tick 会并发跑 N 份设备探测
            // （每份都是整串 adb/fastboot 阻塞调用），互相踩 _currentMode 还会把外壳状态栏
            // 打成抽风。开关只 Start/Stop 即可。
            // 2026-10-10 修复：Unloaded 改名 UnloadedHandler —— 除了停自动刷新，还要把
            // USB 插拔 / 窗口激活 / 可见性三路订阅摘掉，否则页面缓存常驻会一直空转接收事件。
            Unloaded += UnloadedHandler;
            _autoRefreshTimer.Tick += async (_, _) => await DetectDeviceModeAsync();

            // USB 插拔一次物理变化会连发多条 PnP 事件，去抖到 600ms 只触发一轮探测。
            _usbDebounce.Interval = TimeSpan.FromMilliseconds(600);
            _usbDebounce.Tick += (_, _) =>
            {
                _usbDebounce.Stop();
                _ = DetectDeviceModeAsync();
            };

            // 切回主页页签（页面缓存常驻，不重建）→ IsVisible 变 true，立即重探一次。
            IsVisibleChanged += (_, _) =>
            {
                if (IsVisible) _ = DetectDeviceModeAsync();
            };
        }

        private void UnloadedHandler(object sender, RoutedEventArgs e)
        {
            _autoRefreshTimer.Stop();
            _usbDebounce.Stop();
            UsbDeviceNotifier.DeviceChanged -= OnUsbDeviceChanged;
            if (Application.Current.MainWindow != null)
                Application.Current.MainWindow.Activated -= OnMainWindowActivated;
        }

        private async void OnLoaded(object sender, RoutedEventArgs e)
        {
            // 订阅 USB 插拔（设备切模式）与窗口重新激活（设备重启后切回本程序）。
            // 两者都只在主页可见时才真正去重探（见 OnUsbDeviceChanged 的 IsVisible 闸门）。
            UsbDeviceNotifier.DeviceChanged += OnUsbDeviceChanged;
            if (Application.Current.MainWindow != null)
                Application.Current.MainWindow.Activated += OnMainWindowActivated;

            await DetectDeviceModeAsync();
        }

        private void OnUsbDeviceChanged(object? sender, EventArgs e)
        {
            if (!IsVisible) return;
            _usbDebounce.Stop();
            _usbDebounce.Start();
        }

        private void OnMainWindowActivated(object? sender, EventArgs e)
        {
            if (!IsVisible) return;
            _ = DetectDeviceModeAsync();
        }

        private async Task DetectDeviceModeAsync()
        {
            // 重入闸门：USB 插拔 / 窗口激活 / 可见性变化 / 手动刷新可能几乎同时打到，
            // 已有一轮在跑就把需求记成 pending，跑完再补一轮，避免两套串行 adb 调用互踩。
            if (_detectRunning) { _detectPending = true; return; }
            _detectRunning = true;
            try
            {
            await Task.Run(() =>
            {
                // Check ADB
                // 合规：不再读取 ADB 序列号（设备唯一标识）。这里只需判断「是否有设备在线」，
                // 直接看 adb devices 输出里是否存在状态为 device 的行即可，无需取序列号。
                var adbDevices = RunCommand("adb", "devices");
                if (adbDevices.Contains("device") && !adbDevices.Contains("offline"))
                {
                    var hasOnlineDevice = adbDevices
                        .Split('\n')
                        .Skip(1)
                        .Any(line => line.Trim().EndsWith("\tdevice"));
                    if (hasOnlineDevice)
                    {
                        var state = RunCommand("adb", "shell getprop ro.build.type").Trim();
                        if (state == "userdebug" || state == "eng" || state.Contains("recovery"))
                        {
                            Dispatcher.Invoke(() => _currentMode = "recovery");
                        }
                        else
                        {
                            Dispatcher.Invoke(() => _currentMode = "adb");
                        }
                        return;
                    }
                }

                // Check Fastboot
                var fastbootDevices = RunCommand("fastboot", "devices");
                if (fastbootDevices.Contains("fastboot"))
                {
                    Dispatcher.Invoke(() => _currentMode = "fastboot");
                    return;
                }

                // Check EDL (9008)
                var edlDevices = RunCommand("adb", "devices -l");
                if (edlDevices.Contains("9008") || edlDevices.Contains("Qualcomm"))
                {
                    Dispatcher.Invoke(() => _currentMode = "edl");
                    return;
                }

                Dispatcher.Invoke(() => _currentMode = "none");
            });

            Dispatcher.Invoke(() =>
            {
                switch (_currentMode)
                {
                    case "adb":
                        TxtDeviceMode.Text = "ADB 模式";
                        TxtDeviceMode.Foreground = (System.Windows.Media.Brush)FindResource("Theme.LogSuccess");
                        TxtConnectionType.Text = "USB 调试";
                        TxtConnectionStatus.Text = "已连接 (ADB)";
                        EllipseConnection.Background = (System.Windows.Media.Brush)FindResource("Theme.LogSuccess");
                        break;
                    case "fastboot":
                        TxtDeviceMode.Text = "Fastboot 模式";
                        TxtDeviceMode.Foreground = (System.Windows.Media.Brush)FindResource("Theme.LogWarning");
                        TxtConnectionType.Text = "Fastboot";
                        TxtConnectionStatus.Text = "已连接 (Fastboot)";
                        EllipseConnection.Background = (System.Windows.Media.Brush)FindResource("Theme.LogWarning");
                        break;
                    case "recovery":
                        TxtDeviceMode.Text = "Recovery 模式";
                        TxtDeviceMode.Foreground = (System.Windows.Media.Brush)FindResource("Theme.LogWarning");
                        TxtConnectionType.Text = "ADB (Recovery)";
                        TxtConnectionStatus.Text = "已连接 (Recovery)";
                        EllipseConnection.Background = (System.Windows.Media.Brush)FindResource("Theme.LogWarning");
                        break;
                    case "edl":
                        TxtDeviceMode.Text = "EDL 模式 (9008)";
                        TxtDeviceMode.Foreground = (System.Windows.Media.Brush)FindResource("Theme.LogError");
                        TxtConnectionType.Text = "EDL";
                        TxtConnectionStatus.Text = "已连接 (EDL)";
                        EllipseConnection.Background = (System.Windows.Media.Brush)FindResource("Theme.LogError");
                        break;
                    default:
                        TxtDeviceMode.Text = "未连接";
                        TxtDeviceMode.Foreground = (System.Windows.Media.Brush)FindResource("Theme.TextSecondary");
                        TxtConnectionType.Text = "-";
                        TxtConnectionStatus.Text = "未连接";
                        EllipseConnection.Background = (System.Windows.Media.Brush)FindResource("Theme.TextSecondary");
                        break;
                }
            });

            if (_currentMode == "adb" || _currentMode == "recovery")
                await RefreshDeviceInfoAdbAsync();
            else if (_currentMode == "fastboot")
                await RefreshDeviceInfoFastbootAsync();
            else if (_currentMode == "edl")
                await RefreshDeviceInfoEdlAsync();
            else
                ClearDeviceInfo();

            PushDeviceState();
            }
            finally
            {
                _detectRunning = false;
                if (_detectPending) { _detectPending = false; _ = DetectDeviceModeAsync(); }
            }
        }

        /// <summary>
        /// 把本页探测结果广播给外壳左栏「设备信息」卡（本页 5s 一轮，顺手带出去）。
        /// 外壳因此**不用**再起一套 adb/fastboot 探测 —— 两台探测撞同一台设备会互相打断。
        /// </summary>
        private void PushDeviceState()
        {
            var model = (TxtModel?.Text ?? "").Trim();
            if (model is "" or "-" or "—") model = "";
            bool none = _currentMode.Length == 0 || _currentMode == "none";

            // 2026-10-02 修复：adb/fastboot 探测**看不见** 9008 设备。EDL 会话已在广播
            // 「EDL 模式 (9008)」时，本页「没探到」绝不能反过来把它覆盖成「未连接」——
            // 否则插着线刷机时，外壳读数每 5 秒被抽成一次「未连接」再弹回来。只有本页
            // 真的探到设备、或当前广播不是 EDL 会话时，才允许覆盖。
            if (none && DeviceStateHub.Current is { } cur &&
                cur.Mode?.Contains("EDL", StringComparison.Ordinal) == true)
                return;

            Services.DeviceStateHub.Report(
                none ? "未连接设备" : (model.Length > 0 ? model : "未知设备"),
                none ? "未连接" : _currentMode switch
                {
                    "edl"      => "EDL 模式 (9008)",
                    "recovery" => "Recovery 模式",
                    "fastboot" => "Fastboot 模式",
                    _          => "ADB 模式"
                });
        }

        private async Task RefreshDeviceInfoAdbAsync()
        {
            try
            {
                // 合规：不再采集 ADB 序列号与 Android ID（两者都是设备唯一标识）。
                var (model, androidVersion, build, brand, product, kernel, securityPatch, baseband,
                     battery, storage, rootCheck, bootloader, usbDebug) = await Task.Run(() =>
                {
                    var m = RunAdbCommand("shell getprop ro.product.model")?.Trim() ?? "";
                    var av = RunAdbCommand("shell getprop ro.build.version.release")?.Trim() ?? "";
                    var b = RunAdbCommand("shell getprop ro.build.display.id")?.Trim() ?? "";
                    var br = RunAdbCommand("shell getprop ro.product.brand")?.Trim() ?? "";
                    var p = RunAdbCommand("shell getprop ro.product.name")?.Trim() ?? "";
                    var k = RunAdbCommand("shell cat /proc/version")?.Trim() ?? "";
                    var sp = RunAdbCommand("shell getprop ro.build.version.security_patch")?.Trim() ?? "";
                    var bb = RunAdbCommand("shell getprop gsm.version.baseband")?.Trim() ?? "";
                    var bat = RunAdbCommand("shell dumpsys battery")?.Trim() ?? "";
                    var st = RunAdbCommand("shell df /data --output=size,used,avail")?.Trim() ?? "";
                    var rc = RunAdbCommand("shell su -c id") ?? "";
                    var bl = RunAdbCommand("shell getprop ro.boot.verifiedbootstate") ?? "";
                    var ud = RunAdbCommand("shell settings get global adb_enabled") ?? "";
                    return (m, av, b, br, p, k, sp, bb, bat, st, rc, bl, ud);
                });

                TxtModel.Text = model;
                TxtAndroidVersion.Text = androidVersion;
                TxtBuild.Text = build;
                TxtBrand.Text = brand;
                TxtProduct.Text = product;
                TxtKernel.Text = kernel;
                TxtSecurityPatch.Text = securityPatch;
                TxtBaseband.Text = baseband;

                // 新增的 6 项（芯片平台 / 运行内存 / 分辨率 / 设备代号 / 启动槽位 / A-B 分区）
                // 走 3 次调用而不是 6 次：`getprop` 一次dump出全部属性，本地解析即可
                // （原来 13 项就是 13 次 adb 往返，再每项加一次会明显拖慢刷新）。
                var extra = await Task.Run(() => (
                    Props: RunAdbCommand("shell getprop") ?? "",
                    Wm: RunAdbCommand("shell wm size") ?? "",
                    Mem: RunAdbCommand("shell grep MemTotal /proc/meminfo") ?? ""));

                TxtPlatform.Text = OrDash(GetProp(extra.Props, "ro.board.platform"));
                TxtDeviceCodename.Text = OrDash(GetProp(extra.Props, "ro.product.device"));
                TxtRam.Text = ParseRamMb(extra.Mem);
                TxtResolution.Text = ParseResolution(extra.Wm);
                var density = GetProp(extra.Props, "ro.sf.lcd_density").Trim();
                TxtDensity.Text = density.Length > 0 ? density + " dpi" : "-";

                var slot = GetProp(extra.Props, "ro.boot.slot_suffix").Trim();
                TxtSlot.Text = slot.Equals("_a", StringComparison.OrdinalIgnoreCase) ? "A 槽"
                    : slot.Equals("_b", StringComparison.OrdinalIgnoreCase) ? "B 槽"
                    : "不适用（非 A/B）";
                TxtAbUpdate.Text = GetProp(extra.Props, "ro.build.ab_update").Trim() == "true" ? "支持" : "不支持";

                // 一次 dumpsys battery 里同时抠出电量与温度，别为温度再跑一次 adb。
                // ⚠️ 原来这里抠到 level 就 break —— 加了温度之后必须去掉 break，否则温度永远取不到。
                // 另外用 StartsWith("level:") 而不是 Contains("level")：后者会误命中 battery_level / level_step 之类。
                if (!string.IsNullOrEmpty(battery))
                {
                    foreach (var raw in battery.Split('\n'))
                    {
                        var line = raw.Trim();
                        if (line.StartsWith("level:", StringComparison.OrdinalIgnoreCase))
                        {
                            TxtBattery.Text = line.Substring("level:".Length).Trim() + "%";
                        }
                        else if (line.StartsWith("temperature:", StringComparison.OrdinalIgnoreCase))
                        {
                            // dumpsys 的温度单位是 0.1℃
                            if (int.TryParse(line.Substring("temperature:".Length).Trim(), out var t))
                                TxtBatteryTemp.Text = (t / 10.0).ToString("0.0") + " °C";
                        }
                    }
                }

                TxtStorage.Text = string.IsNullOrEmpty(storage) ? "未知" : storage;

                TxtRootStatus.Text = rootCheck.Contains("uid=0") ? "已获取 Root" : "未 Root";
                TxtRootStatus.Foreground = rootCheck.Contains("uid=0")
                    ? (System.Windows.Media.Brush)FindResource("Theme.LogSuccess")
                    : (System.Windows.Media.Brush)FindResource("Theme.TextSecondary");

                TxtBootloader.Text = bootloader.Trim() == "green" ? "已锁定" : "已解锁";
                TxtBootloader.Foreground = bootloader.Trim() == "green"
                    ? (System.Windows.Media.Brush)FindResource("Theme.LogSuccess")
                    : (System.Windows.Media.Brush)FindResource("Theme.LogWarning");

                TxtUsbDebugging.Text = usbDebug.Trim() == "1" ? "已开启" : "已关闭";
                TxtUsbDebugging.Foreground = usbDebug.Trim() == "1"
                    ? (System.Windows.Media.Brush)FindResource("Theme.LogSuccess")
                    : (System.Windows.Media.Brush)FindResource("Theme.LogError");

                LogSuccess($"设备信息刷新完成: {TxtModel.Text}");
            }
            catch (Exception ex)
            {
                LogError($"刷新设备信息失败: {ex.Message}");
            }
        }

        private async Task RefreshDeviceInfoFastbootAsync()
        {
            try
            {
                await Task.Run(() =>
                {
                    // 合规：不再读取 getvar serialno（设备唯一标识）
                    var product = RunFastbootCommand("getvar product")?.Trim() ?? "";
                    var unlocked = RunFastbootCommand("getvar unlocked")?.Trim() ?? "";

                    Dispatcher.Invoke(() =>
                    {
                        TxtProduct.Text = product;
                        TxtModel.Text = "-";
                        TxtAndroidVersion.Text = "-";
                        TxtBuild.Text = "-";
                        TxtBrand.Text = "-";
                        TxtKernel.Text = "-";
                        TxtSecurityPatch.Text = "-";
                        TxtBaseband.Text = "-";
                        TxtBattery.Text = "-";
                        TxtStorage.Text = "-";
                        // fastboot 下拿不到运行时信息（getprop/wm/meminfo 都要进系统），这几项留空
                        TxtPlatform.Text = "-";
                        TxtRam.Text = "-";
                        TxtResolution.Text = "-";
                        TxtDeviceCodename.Text = "-";
                        TxtSlot.Text = "-";
                        TxtAbUpdate.Text = "-";
                        TxtDensity.Text = "-";
                        TxtBatteryTemp.Text = "-";

                        TxtRootStatus.Text = "-";
                        TxtRootStatus.Foreground = (System.Windows.Media.Brush)FindResource("Theme.TextSecondary");

                        TxtBootloader.Text = unlocked.Contains("yes") ? "已解锁" : "已锁定";
                        TxtBootloader.Foreground = unlocked.Contains("yes")
                            ? (System.Windows.Media.Brush)FindResource("Theme.LogWarning")
                            : (System.Windows.Media.Brush)FindResource("Theme.LogSuccess");

                        TxtUsbDebugging.Text = "-";
                        TxtUsbDebugging.Foreground = (System.Windows.Media.Brush)FindResource("Theme.TextSecondary");
                    });

                    LogSuccess("Fastboot 设备信息刷新完成");
                });
            }
            catch (Exception ex)
            {
                LogError($"刷新 Fastboot 设备信息失败: {ex.Message}");
            }
        }

        private async Task RefreshDeviceInfoEdlAsync()
        {
            try
            {
                await Task.Run(() =>
                {
                    Dispatcher.Invoke(() =>
                    {
                        TxtModel.Text = "-";
                        TxtAndroidVersion.Text = "-";
                        TxtBuild.Text = "-";
                        TxtBrand.Text = "-";
                        TxtProduct.Text = "-";
                        TxtKernel.Text = "-";
                        TxtSecurityPatch.Text = "-";
                        TxtBaseband.Text = "-";
                        TxtBattery.Text = "-";
                        TxtStorage.Text = "-";
                        TxtRootStatus.Text = "-";
                        TxtBootloader.Text = "-";
                        TxtUsbDebugging.Text = "-";
                    });

                    LogSuccess("EDL 设备信息刷新完成");
                });
            }
            catch (Exception ex)
            {
                LogError($"刷新 EDL 设备信息失败: {ex.Message}");
            }
        }

        private void ClearDeviceInfo()
        {
            TxtModel.Text = "-";
            TxtAndroidVersion.Text = "-";
            TxtBuild.Text = "-";
            TxtBrand.Text = "-";
            TxtProduct.Text = "-";
            TxtKernel.Text = "-";
            TxtSecurityPatch.Text = "-";
            TxtBaseband.Text = "-";
            TxtBattery.Text = "-";
            TxtStorage.Text = "-";
            TxtRootStatus.Text = "-";
            TxtBootloader.Text = "-";
            TxtUsbDebugging.Text = "-";
            TxtPlatform.Text = "-";
            TxtRam.Text = "-";
            TxtResolution.Text = "-";
            TxtDeviceCodename.Text = "-";
            TxtSlot.Text = "-";
            TxtAbUpdate.Text = "-";
            TxtDensity.Text = "-";
            TxtBatteryTemp.Text = "-";
        }

        // ADB Quick Actions
        private void BtnRebootSystem_Click(object sender, RoutedEventArgs e) => RunReboot("reboot");
        private void BtnRebootRecovery_Click(object sender, RoutedEventArgs e) => RunReboot("reboot recovery");
        private void BtnRebootBootloader_Click(object sender, RoutedEventArgs e) => RunReboot("reboot bootloader");
        private void BtnRebootEdl_Click(object sender, RoutedEventArgs e) => RunReboot("reboot edl");
        private void BtnRebootFastbootD_Click(object sender, RoutedEventArgs e) => RunReboot("reboot fastboot");

        // Fastboot Quick Actions
        private void BtnFastbootReboot_Click(object sender, RoutedEventArgs e) => RunFastboot("reboot");
        private void BtnFastbootRebootRecovery_Click(object sender, RoutedEventArgs e) => RunFastboot("reboot recovery");
        private void BtnFastbootRebootBootloader_Click(object sender, RoutedEventArgs e) => RunFastboot("reboot-bootloader");
        private void BtnFastbootOemUnlock_Click(object sender, RoutedEventArgs e) => RunFastboot("oem unlock");
        // 合规：原「获取序列号」按钮（fastboot getvar serialno）已替换为
        // 「查看引导状态」——只读设备级状态，不涉及任何唯一标识。
        private void BtnFastbootGetvarUnlocked_Click(object sender, RoutedEventArgs e)
        {
            var unlocked = (RunFastbootCommand("getvar unlocked") ?? "").Trim();
            LogInfo($"[Fastboot] Bootloader 状态: {(unlocked.Contains("yes") ? "已解锁" : "已锁定")}");
        }

        /// <summary>
        /// 免解 BL、免拆机进入 EDL(9008)：只用**公开文档**里的两条命令。
        ///
        /// 关于「读 token 算码」：小米在 9008 上加了授权 —— 设备发 token，主机用厂商密钥签名回写，
        /// 验签通过才放行。早年所谓「免授权」是重放一个固定签名（token 未请求时内存为默认值 →
        /// 签名也固定），属于已修复的鉴权漏洞。要靠它进 EDL，要么利用该漏洞、要么持有厂商签名密钥，
        /// 两条都是在绕过设备鉴权，本工具**不实现**。
        /// 所以这里只能做到：命令有效就进，无效就如实告诉用户改走深度刷机线或测试点。
        /// </summary>
        private async void BtnEnterEdl_Click(object sender, RoutedEventArgs e)
        {
            await Task.Run(() =>
            {
                if (_currentMode == "adb" || _currentMode == "recovery")
                {
                    RunAdbCommand("reboot edl");
                    LogInfo("已发送: adb reboot edl");
                    Dispatcher.Invoke(() => ShowSuccess("已发送 adb reboot edl，请查看设备管理器是否出现 9008 端口"));
                    return;
                }

                if (_currentMode == "fastboot")
                {
                    // 不同机型认的命令不一样：先试 oem edl，再退 reboot edl
                    RunFastbootCommand("oem edl");
                    LogInfo("已发送: fastboot oem edl");
                    RunFastbootCommand("reboot edl");
                    LogInfo("已发送: fastboot reboot edl");
                    Dispatcher.Invoke(() => ShowSuccess(
                        "已依次尝试 fastboot oem edl / reboot edl。若设备管理器没有出现 9008 端口，说明该机型已封锁命令，需深度刷机线或短接测试点。"));
                    return;
                }

                LogWarning("未检测到在线设备，无法进入 EDL");
                Dispatcher.Invoke(() => ShowError("没有检测到 ADB / Fastboot 设备，请先连接手机"));
            });
        }

        private void RunReboot(string command)
        {
            // 日志「三明治」：一次重启就是一次操作，RunAdbCommand 同步返回即结束
            // （这里的探测循环 DetectDeviceModeAsync 不加：自动刷新 5 秒一轮，加了会把日志刷满）。
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, $"重启设备 (adb {command})");
            RunAdbCommand(command);
            LogInfo($"执行: adb {command}");
            ShowSuccess($"已发送: adb {command}");
            PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
        }

        private void RunFastboot(string command)
        {
            // 日志「三明治」：同上，Fastboot 侧的重启 / 解锁操作
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, $"Fastboot 操作 (fastboot {command})");
            RunFastbootCommand(command);
            LogInfo($"执行: fastboot {command}");
            ShowSuccess($"已发送: fastboot {command}");
            PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
        }

        // 系统快捷操作
        private void BtnDeviceManager_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                // devmgmt.msc 是 mmc 管理单元，必须走 Shell 执行（用 cmd 起不来）
                Process.Start(new ProcessStartInfo("devmgmt.msc") { UseShellExecute = true });
                LogInfo("已打开设备管理器");
                ShowSuccess("已打开设备管理器");
            }
            catch (Exception ex)
            {
                LogError($"打开设备管理器失败: {ex.Message}");
                ShowError("打开设备管理器失败：" + ex.Message);
            }
        }

        private void BtnOpenConsole_Click(object sender, RoutedEventArgs e)
        {
            if (Application.Current.MainWindow is MainWindow window)
                window.NavigateTo("CommandConsole");
        }

        private async void BtnRefresh_Click(object sender, RoutedEventArgs e) => await DetectDeviceModeAsync();

        private void BtnAutoRefresh_Click(object sender, RoutedEventArgs e)
        {
            if (_isAutoRefreshEnabled)
            {
                _autoRefreshTimer.Stop();
                BtnAutoRefresh.Content = "开启自动刷新";
                _isAutoRefreshEnabled = false;
                LogInfo("自动刷新已关闭");
            }
            else
            {
                _autoRefreshTimer.Interval = TimeSpan.FromSeconds(5);
                _autoRefreshTimer.Start();
                BtnAutoRefresh.Content = "关闭自动刷新";
                _isAutoRefreshEnabled = true;
                LogInfo("自动刷新已开启 (5秒间隔)");
            }
        }

        private static string RunCommand(string fileName, string arguments)
        {
            try
            {
                using var process = new Process
                {
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = fileName,
                        Arguments = arguments,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        UseShellExecute = false,
                        CreateNoWindow = true
                    }
                };
                process.Start();
                // 2026-10-02 修复：原先只同步读 stdout、stderr 只 Redirect 不读 —— 子进程往
                // stderr 写满管道缓冲（约 4KB）就与 ReadToEnd 互等死锁；且没有超时，一条挂死的
                // adb「waiting for device」会永远占住探测槽（自动刷新还会越叠越多）。改为两路
                // 并行异步读 + 15 秒强杀兜底。
                var stdoutTask = process.StandardOutput.ReadToEndAsync();
                var _ = process.StandardError.ReadToEndAsync();   // 持续排空 stderr，防管道死锁
                if (!process.WaitForExit(15000))
                {
                    try { process.Kill(true); } catch { }
                    process.WaitForExit(3000);
                }
                return stdoutTask.GetAwaiter().GetResult() ?? "";
            }
            catch
            {
                return "";
            }
        }

        private static string OrDash(string s) => string.IsNullOrWhiteSpace(s) ? "-" : s.Trim();

        /// <summary>从 <c>adb shell getprop</c> 的整份输出里按 key 取值（行格式：<c>[key]: [value]</c>）。</summary>
        private static string GetProp(string getpropOutput, string key)
        {
            if (string.IsNullOrEmpty(getpropOutput)) return "";
            foreach (var raw in getpropOutput.Split('\n'))
            {
                var line = raw.Trim();
                if (!line.StartsWith("[" + key + "]", StringComparison.Ordinal)) continue;
                int sep = line.IndexOf(": [", StringComparison.Ordinal);
                if (sep < 0) continue;
                var v = line.Substring(sep + 3).Trim();
                return v.EndsWith("]", StringComparison.Ordinal) ? v.Substring(0, v.Length - 1) : v;
            }
            return "";
        }

        /// <summary><c>MemTotal:        7657472 kB</c> → <c>7478 MB</c>（取 kB 数 / 1024）。</summary>
        private static string ParseRamMb(string memInfo)
        {
            if (string.IsNullOrEmpty(memInfo)) return "-";
            foreach (var part in memInfo.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries))
            {
                if (long.TryParse(part, out var kb) && kb > 1024)
                    return (kb / 1024) + " MB";
            }
            return "-";
        }

        /// <summary><c>wm size</c> 输出 <c>Physical size: 1080x2400</c>（还可能有 Override 行）→ 取 1080x2400。</summary>
        private static string ParseResolution(string wmSize)
        {
            if (string.IsNullOrEmpty(wmSize)) return "-";
            var m = System.Text.RegularExpressions.Regex.Match(wmSize, @"(\d{3,5})\s*x\s*(\d{3,5})");
            return m.Success ? m.Groups[1].Value + " × " + m.Groups[2].Value : "-";
        }

        private static string RunAdbCommand(string arguments) => RunCommand("adb", arguments);
        private static string RunFastbootCommand(string arguments) => RunCommand("fastboot", arguments);
    }
}
