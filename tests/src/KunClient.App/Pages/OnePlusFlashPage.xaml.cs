using System;
using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public partial class OnePlusFlashPage : BasePage
    {
        private BackgroundDownloader? _downloader;

        public OnePlusFlashPage()
        {
            InitializeComponent();
            Loaded += OnLoaded;
            Unloaded += (_, _) => { _downloader?.Cancel(); _downloader?.Dispose(); _downloader = null; };

            // 页内左栏「设备信息」读数：本页在 MainWindow 页表里标了 OwnRail:true，
            // 外壳共用左栏（Controls/DeviceLogRail）对本页整列让位，所以读数由页面自己显示。
            // 数据来源与外壳左栏完全一致 —— 设备/模式 ← DeviceStateHub（谁探测谁汇报，
            // 本页自己不探测，显示最近一次广播）；任务/进度/状态 ← ShellStatus
            // （本页各操作的 PageLog.Section 已顺带把任务名上报，下载/逐镜像刷写还报百分比）。
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

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            _downloader?.Dispose();
            _downloader = new BackgroundDownloader();
            _downloader.ProgressChanged += OnDownloadProgress;
        }

        private void OnDownloadProgress(DownloadProgress p)
        {
            // BeginInvoke：BackgroundDownloader 已补 ConfigureAwait(false)（D8），
            // 回调在线程池上，Invoke 会同步等 UI 线程，UI 稍忙后台就被拖住。
            Dispatcher.BeginInvoke(() =>
            {
                TxtProgress.Text = p.Detail;
                if (p.Percent >= 0)
                {
                    ProgressBar.Value = p.Percent;
                    // 页面进度顺手报给外壳底部进度条（参考稿里那两条属于窗口外壳）
                    ShellStatus.Report(percent: p.Percent);
                }
            });
        }

        private void BtnBrowseLocal_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "OPLUS刷写包|*.ofp;*.ops;*.ozip;*.zip|Payload文件|*.bin|所有文件|*.*" };
            if (dialog.ShowDialog() == true)
            {
                TxtLocalFile.Text = dialog.FileName;
            }
        }

        private async void BtnDownloadCloud_Click(object sender, RoutedEventArgs e)
        {
            var url = TxtCloudUrl.Text?.Trim();
            if (string.IsNullOrEmpty(url) || !url.StartsWith("http"))
            {
                ShowError("请输入有效的下载链接");
                return;
            }

            if (_downloader?.IsDownloading == true)
            {
                _downloader?.Cancel();
                TxtProgress.Text = "下载已取消";
                return;
            }

            var saveDialog = new SaveFileDialog { Title = "保存刷写包", Filter = "刷写包|*.ofp;*.ops;*.ozip;*.zip;*.bin" };
            if (saveDialog.ShowDialog() != true) return;

            // 日志三明治：区段标题（Section 同时把外壳「任务」段刷成同名）→ 明细 → 收尾统计。
            // 计时/成败旗标放在 try 外面，catch 里也要能收尾。
            var sw = Stopwatch.StartNew();
            var ok = false;
            try
            {
                PageLog.Section(LogTarget, "下载刷写包");
                ProgressBar.Value = 0;
                TxtProgress.Text = "准备下载...";

                var success = await _downloader!.DownloadFileAsync(new DownloadTask
                {
                    Url = url,
                    SavePath = saveDialog.FileName,
                    Description = "下载刷写包",
                    MaxRetries = 3,
                    Timeout = TimeSpan.FromMinutes(30)
                });

                if (success)
                {
                    ok = true;
                    TxtLocalFile.Text = saveDialog.FileName;
                    LogSuccess($"[欧加线刷] 下载完成: {saveDialog.FileName}");
                    TxtProgress.Text = "下载完成";
                    ShellStatus.EndTask();
                }
                else
                {
                    LogError("[欧加线刷] 下载失败");
                    TxtProgress.Text = "下载失败";
                    ShellStatus.Report(task: "下载失败");
                }
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[欧加线刷] 下载失败: {ex.Message}");
                TxtProgress.Text = "下载失败";
                ShellStatus.Report(task: "下载失败");
                PageLog.Footer(LogTarget, $"失败, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnStartFlash_Click(object sender, RoutedEventArgs e)
        {
            var file = TxtLocalFile.Text?.Trim();
            if (string.IsNullOrEmpty(file) || !File.Exists(file))
            {
                ShowError("请选择有效的刷写包文件");
                return;
            }

            var mode = RbFullPackage.IsChecked == true ? "完整包" : "售后";
            if (!Services.MembershipService.EnsureAllowed("OnePlus 一键刷写", "oneplus.flash")) return;
            if (MessageBox.Show($"确认以{mode}模式刷写？\n文件: {file}", "确认刷写",
                MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;
            if (!Services.MembershipService.Consume("oneplus.flash")) return;

            // 刷写类操作：Section 把外壳状态栏「任务」段设成刷写模式名，进度镜像到外壳底部进度条，
            // 收尾统一走 finally（成功/失败/中途 return 都只收一次尾）。
            var sw = Stopwatch.StartNew();
            var ok = false;
            try
            {
                PageLog.Section(LogTarget, $"线刷刷写（{mode}模式）");
                ProgressBar.Value = 0;
                TxtProgress.Text = "刷写中...";

                var ext = Path.GetExtension(file).ToLowerInvariant();
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();

                if (ext is ".ofp" or ".ops")
                {
                    LogInfo($"[欧加线刷] 使用edl刷写 {Path.GetFileName(file)}");
                    var (success, output, error) = await tools.ExecuteAdbAsync($"edl \"{file}\"");
                    if (success) { ok = true; LogSuccess("[欧加线刷] 刷写完成"); TxtProgress.Text = "刷写完成"; ProgressBar.Value = 100; }
                    else { LogError($"[欧加线刷] 刷写失败: {error}"); TxtProgress.Text = "刷写失败"; }
                }
                else if (ext is ".ozip" or ".zip")
                {
                    LogInfo("[欧加线刷] 解压刷写包...");
                    var extractDir = Path.Combine(Path.GetTempPath(), "KunClient_oneplus_" + Guid.NewGuid().ToString("N")[..8]);
                    Directory.CreateDirectory(extractDir);

                    // 2026-10-02 修复：解压是**本机文件操作**，原先却发给 adb 执行
                    // （`adb unzip ...` 不是 adb 的任何命令，原版 platform-tools 直接
                    // 「unknown command」退出 → .zip/.ozip 路径永远「解压失败」）。
                    // 改用 .NET 自带 ZipFile 本地解压（ScreenMirrorPage 同款做法）；
                    // 加密的 .ozip 会在这里抛异常，如实报「解压失败 + 原因」。
                    try
                    {
                        System.IO.Compression.ZipFile.ExtractToDirectory(file, extractDir, overwriteFiles: true);
                    }
                    catch (Exception ex)
                    {
                        LogError($"[欧加线刷] 解压失败: {ex.Message}");
                        TxtProgress.Text = "解压失败";
                        return;
                    }

                    var payloadBin = Path.Combine(extractDir, "payload.bin");
                    if (File.Exists(payloadBin))
                    {
                        LogInfo("[欧加线刷] 检测到payload.bin，使用fastboot刷写...");
                        var flashResult = await FlashPayloadAsync(tools, payloadBin);
                        ok = flashResult;
                        if (flashResult) { TxtProgress.Text = "刷写完成"; ProgressBar.Value = 100; }
                        else { TxtProgress.Text = "刷写失败"; }
                    }
                    else
                    {
                        var imgs = Directory.GetFiles(extractDir, "*.img");
                        if (imgs.Length > 0)
                        {
                            LogInfo($"[欧加线刷] 找到 {imgs.Length} 个镜像，逐个刷写...");
                            for (int i = 0; i < imgs.Length; i++)
                            {
                                var imgName = Path.GetFileNameWithoutExtension(imgs[i]);
                                await tools.ExecuteFastbootAsync($"flash {imgName} \"{imgs[i]}\"");
                                ProgressBar.Value = (double)(i + 1) / imgs.Length * 100;
                                ShellStatus.Report(percent: ProgressBar.Value);
                            }
                            ok = true;
                            TxtProgress.Text = "刷写完成";
                            LogSuccess("[欧加线刷] 刷写完成");
                        }
                        else
                        {
                            LogError("[欧加线刷] 未找到可刷写的镜像文件");
                            TxtProgress.Text = "刷写失败";
                        }
                    }
                }
                else if (ext == ".bin")
                {
                    LogInfo("[欧加线刷] 使用fastboot刷写payload.bin...");
                    var flashResult = await FlashPayloadAsync(tools, file);
                    ok = flashResult;
                    if (flashResult) { TxtProgress.Text = "刷写完成"; ProgressBar.Value = 100; }
                    else { TxtProgress.Text = "刷写失败"; }
                }
            }
            catch (Exception ex)
            {
                TxtProgress.Text = "刷写出错";
                LogError($"[欧加线刷] 刷写出错: {ex.Message}");
            }
            finally
            {
                // 收尾口径照 EdlFlashPage：成功才 EndTask（外壳进度条停 100%），
                // 失败只把任务名改成「刷写失败」，不把进度条倒满（否则用户以为刷完了）。
                if (ok) ShellStatus.EndTask();
                else ShellStatus.Report(task: "刷写失败");
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void BtnStopFlash_Click(object sender, RoutedEventArgs e)
        {
            _downloader?.Cancel();
            ProgressBar.Value = 0;
            TxtProgress.Text = "已停止";
            LogInfo("[欧加线刷] 刷写已停止");
        }

        private void BtnFastbootD_Click(object sender, RoutedEventArgs e)
        {
            RunAdbCommand("reboot fastboot", "重启到FastbootD模式");
        }

        private void BtnFormatData_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.EnsureAllowed("Fastboot 擦除 / 格式化", "fastboot.erase")) return;
            if (MessageBox.Show("格式化数据会清除所有用户数据，确定？", "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
            {
                if (!Services.MembershipService.Consume("fastboot.erase")) return;
                RunFastbootCommand("-w", "格式化数据");
            }
        }

        private void BtnEraseFrp_Click(object sender, RoutedEventArgs e)
        {
            // 合规门槛：FRP（出厂重置保护）是厂商提供的设备防盗机制，清除它会移除设备的防盗保护。
            // 为避免使用本工具处置来历不明 / 遗失 / 被盗设备，执行前必须显式确认设备归属，
            // 并把「确认 / 取消」都记进日志，便于事后追溯。
            const string riskText =
                "FRP（Factory Reset Protection，出厂重置保护）是设备厂商提供的防盗机制：设备被重置后，" +
                "仍会要求验证此前的机主账号，用以阻止他人使用丢失或被盗的设备。\n\n" +
                "「清除 FRP」会移除这层保护。因此它只能用于：\n" +
                "  • 设备为本人合法所有，只是忘记了原绑定的账号；\n" +
                "  • 已获得设备所有者的明确授权（例如维修接单）。\n\n" +
                "严禁用于来历不明、遗失或被盗的设备。";

            // 会员额度：FRP 是高风险专项，普通等级每天额度很小（默认 1 次）
            if (!Services.MembershipService.EnsureAllowed("清除出厂重置保护（FRP）", "oneplus.frp")) return;

            var dialog = new KunFlashTool.App.Dialogs.ComplianceConfirmDialog("清除出厂重置保护（FRP）", riskText);
            dialog.Owner = Window.GetWindow(this);
            if (dialog.ShowDialog() != true)
            {
                LogWarning("[合规] 未确认设备归属，已取消「清除 FRP」");
                return;
            }

            if (!Services.MembershipService.Consume("oneplus.frp")) return;
            LogWarning($"[合规] 已确认设备归属，执行 fastboot erase frp（{DateTime.Now:yyyy-MM-dd HH:mm:ss}）");
            RunFastbootCommand("erase frp", "擦除FRP");
        }

        private async void BtnExtractPayload_Click(object sender, RoutedEventArgs e)
        {
            var file = TxtLocalFile.Text?.Trim();
            if (string.IsNullOrEmpty(file) || !file.EndsWith(".bin"))
            {
                ShowError("请选择payload.bin文件");
                return;
            }

            if (!Services.MembershipService.EnsureAllowed("Payload 分区提取", "payload.extract")) return;

            var dialog = new OpenFolderDialog { Title = "选择提取目录" };
            if (dialog.ShowDialog() != true) return;
            if (!Services.MembershipService.Consume("payload.extract")) return;

            try
            {
                var sw = Stopwatch.StartNew();
                PageLog.Section(LogTarget, "提取 Payload");
                LogInfo($"[欧加线刷] 提取Payload: {file}");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var extractDir = Path.Combine(dialog.FolderName, "payload_extracted");
                Directory.CreateDirectory(extractDir);
                var (success, output, error) = await tools.ExecuteAdbAsync($"payload unpack -o \"{extractDir}\" -i \"{file}\"");
                if (success)
                {
                    LogSuccess($"[欧加线刷] 提取完成: {extractDir}");
                    TxtProgress.Text = "提取完成";
                }
                else
                {
                    LogError($"[欧加线刷] 提取失败: {error}");
                    TxtProgress.Text = "提取失败";
                }
                PageLog.Footer(LogTarget, $"{(success ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[欧加线刷] 提取错误: {ex.Message}");
            }
        }

        private async Task<bool> FlashPayloadAsync(PlatformToolsManager tools, string payloadPath)
        {
            try
            {
                var payloadDir = Path.GetDirectoryName(payloadPath) ?? "";
                var flashAllBat = Path.Combine(payloadDir, "flash_all.bat");
                var flashAllSh = Path.Combine(payloadDir, "flash_all.sh");

                if (File.Exists(flashAllBat))
                {
                    LogInfo("[欧加线刷] 执行 flash_all.bat...");
                    var process = new Process
                    {
                        StartInfo = new ProcessStartInfo
                        {
                            FileName = flashAllBat,
                            WorkingDirectory = payloadDir,
                            UseShellExecute = false,
                            RedirectStandardOutput = true,
                            RedirectStandardError = true,
                            CreateNoWindow = true
                        }
                    };
                    process.Start();
                    var output = await process.StandardOutput.ReadToEndAsync();
                    var error = await process.StandardError.ReadToEndAsync();
                    await process.WaitForExitAsync();
                    if (!string.IsNullOrEmpty(output)) LogInfo(output.Trim());
                    if (!string.IsNullOrEmpty(error)) LogWarning(error.Trim());
                    // 2026-10-02 修复：原先不看 ExitCode，flash_all.bat 中途报错（签名校验失败/
                    // 镜像缺失/反回滚拦截）也照样报「执行完成」并让外壳进度条走到 100%。
                    // 脚本退出码非 0 = 刷写失败，必须如实报错并把失败传给调用方。
                    if (process.ExitCode != 0)
                    {
                        LogError($"[欧加线刷] flash_all.bat 失败，退出码 {process.ExitCode}（详见上方脚本输出）");
                        return false;
                    }
                    LogSuccess("[欧加线刷] flash_all.bat 执行完成");
                    return true;
                }

                LogInfo("[欧加线刷] 逐个刷写分区...");
                var rawprogramFiles = Directory.GetFiles(payloadDir, "rawprogram*.xml");
                if (rawprogramFiles.Length > 0)
                {
                    foreach (var xml in rawprogramFiles)
                    {
                        LogInfo($"[欧加线刷] 刷写: {Path.GetFileName(xml)}");
                        await tools.ExecuteFastbootAsync($"flash rawprogram \"{xml}\"");
                    }
                    return true;
                }

                var imgs = Directory.GetFiles(payloadDir, "*.img");
                for (int i = 0; i < imgs.Length; i++)
                {
                    var name = Path.GetFileNameWithoutExtension(imgs[i]);
                    LogInfo($"[欧加线刷] flash {name}...");
                    await tools.ExecuteFastbootAsync($"flash {name} \"{imgs[i]}\"");
                    ProgressBar.Value = (double)(i + 1) / imgs.Length * 100;
                    // 逐个镜像刷写时把进度镜像给外壳（本方法没有自己的进度条更新点，就这一处）
                    ShellStatus.Report(partition: name, percent: ProgressBar.Value);
                }
                return true;
            }
            catch (Exception ex)
            {
                LogError($"[欧加线刷] 刷写错误: {ex.Message}");
                return false;
            }
        }

        private async void RunAdbCommand(string arguments, string description)
        {
            // 这些按钮（重启到 FastbootD 等）都是独立的一键操作，
            // 同样给「区段标题 → 明细 → 收尾统计」三明治；Section 顺带把外壳任务名设成 description。
            var sw = Stopwatch.StartNew();
            var ok = false;
            try
            {
                PageLog.Section(LogTarget, description);
                LogInfo($"[ADB] {description}: adb {arguments}");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var (success, output, error) = await tools.ExecuteAdbAsync(arguments);
                ok = success;
                if (success && !string.IsNullOrEmpty(output)) LogInfo(output.Trim());
                if (!string.IsNullOrEmpty(error)) LogWarning($"[ADB错误] {error.Trim()}");
            }
            catch (Exception ex)
            {
                LogError($"[ADB错误] {ex.Message}");
            }
            finally
            {
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void RunFastbootCommand(string arguments, string description)
        {
            var sw = Stopwatch.StartNew();
            var ok = false;
            try
            {
                PageLog.Section(LogTarget, description);
                LogInfo($"[Fastboot] {description}: fastboot {arguments}");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var (success, output, error) = await tools.ExecuteFastbootAsync(arguments);
                ok = success;
                if (success && !string.IsNullOrEmpty(output)) LogInfo(output.Trim());
                if (!string.IsNullOrEmpty(error)) LogWarning($"[Fastboot错误] {error.Trim()}");
            }
            catch (Exception ex)
            {
                LogError($"[Fastboot错误] {ex.Message}");
            }
            finally
            {
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }
    }
}
