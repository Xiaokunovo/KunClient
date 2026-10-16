using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using KunFlashTool.App.Services;
using KunFlashTool.Core.Configuration;
using KunFlashTool.Core.PlatformTools;

namespace KunFlashTool.App.Pages
{
    public partial class ScreenMirrorPage : BasePage
    {
        private static string ServerUrl => KunFlashTool.App.Services.AppConfig.ServerUrl;
        private Process? _scrcpyProcess;
        private string _scrcpyPath = "";
        private bool _isDownloading;

        /// <summary>
        /// scrcpy 安装目录：<c>%LOCALAPPDATA%\KunClientState\cache\scrcpy</c>（**跨会话保留**）。
        /// <para>🔴 2026-10-08：老位置是 <c>%LOCALAPPDATA%\KunClient\scrcpy</c>，而 <c>App.OnExit</c>
        /// 会把那个目录整目录删掉 —— 所以「用户级目录」这个说法当时是错的，**每次启动都要重下
        /// 11.3MB**。现在走 <see cref="AppStatePaths.ResolveCache"/>：老位置已有的会一次性搬过来。</para>
        /// </summary>
        private static string ScrcpyDir => AppStatePaths.ResolveCache("scrcpy", "scrcpy", "scrcpy.exe");

        private readonly PlatformToolsManager _tools;
        private string _lastMode = "usb";

        /// <summary>
        /// scrcpy 逐行输出 → 日志框的批量投递。
        /// 原来是**每行一次同步 Dispatcher.Invoke**（stdout / stderr 各一行一次）：
        /// 读线程阻塞等 UI 线程，scrcpy 启动时会连吐一串信息行，界面一忙整条管道跟着卡。
        /// 现在只入队，UI 侧一次回调把一批交给 PageLog（PageLog 自己再攒一次渲染）。
        /// </summary>
        private readonly LogLineQueue<(string Text, bool Error)> _scrcpyOut;

        public ScreenMirrorPage()
        {
            InitializeComponent();
            _scrcpyOut = new LogLineQueue<(string Text, bool Error)>(Dispatcher, lines =>
            {
                foreach (var (text, error) in lines)
                {
                    if (error) LogWarning($"[scrcpy] {text}");
                    else LogInfo($"[scrcpy] {text}");
                }
            });
            _tools = new PlatformToolsManager(msg => LogInfo($"[工具] {msg}"));
            Loaded += OnLoaded;
            Unloaded += OnUnloaded;

            // 页内左栏「设备信息」读数：本页在 MainWindow 页表里标了 OwnRail:true，
            // 外壳共用左栏（Controls/DeviceLogRail）对本页整列让位，所以读数由页面自己显示。
            // 数据来源与外壳左栏完全一致 —— 设备/模式 ← DeviceStateHub（谁探测谁汇报，
            // 本页自己不探测，显示最近一次广播）；任务/进度/状态 ← ShellStatus
            // （本页各操作的 PageLog.Section 已顺带把任务名上报）。
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

        private void OnUnloaded(object sender, RoutedEventArgs e) => StopScrcpy();

        private async void OnLoaded(object sender, RoutedEventArgs e)
        {
            await EnsureScrcpyAsync();
        }

        private async Task EnsureScrcpyAsync()
        {
            try
            {
                // 迁移（从 %TEMP% / %LOCALAPPDATA%\KunClient 的老 scrcpy 目录搬过来）已经在
                // ScrcpyDir → AppStatePaths.ResolveCache 里做掉了，这里只看结果。
                var found = FindScrcpy();
                if (found != null)
                {
                    _scrcpyPath = found;
                    LogSuccess($"[投屏] scrcpy 已就绪: {_scrcpyPath}");
                    return;
                }

                LogInfo("[投屏] 未检测到 scrcpy，开始自动下载...");
                await DownloadScrcpyAsync();
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 初始化失败: {ex.Message}");
            }
        }

        private static string? FindScrcpy()
        {
            if (!Directory.Exists(ScrcpyDir))
                return null;

            var found = Directory.GetFiles(ScrcpyDir, "scrcpy.exe", SearchOption.AllDirectories);
            return found.Length > 0 ? found[0] : null;
        }

        private async Task DownloadScrcpyAsync()
        {
            if (_isDownloading) return;
            _isDownloading = true;

            // 日志「三明治」：下载 scrcpy 也是一次「一键式」操作（进度行夹在区段标题与收尾行之间）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, "下载 scrcpy");

            try
            {
                var scrcpyDir = ScrcpyDir;
                Directory.CreateDirectory(scrcpyDir);
                var zipPath = Path.Combine(scrcpyDir, "scrcpy.zip");

                var downloader = new BackgroundDownloader();
                downloader.ProgressChanged += (p) =>
                {
                    // BeginInvoke：D8 之后回调落在线程池，Invoke 会同步等 UI 线程。
                    Dispatcher.BeginInvoke(() =>
                    {
                        var downloaded = (double)p.BytesReceived / 1024 / 1024;
                        var total = (double)p.TotalBytes / 1024 / 1024;
                        LogInfo($"[投屏] 下载中... {p.Percent:F1}% ({downloaded:F1}MB/{total:F1}MB)");
                    });
                };

                LogInfo("[投屏] 正在从服务器下载 scrcpy...");
                // E11：优先二进制端点（-33% 字节），失败回落 base64。
                await downloader.DownloadPackageFromServerAsync(
                    ServerUrl, "/api/v1/packages/scrcpy", zipPath, "正在下载 scrcpy...");

                if (File.Exists(zipPath))
                {
                    LogInfo("[投屏] 解压 scrcpy...");
                    ZipFile.ExtractToDirectory(zipPath, scrcpyDir, true);
                    File.Delete(zipPath);

                    var found = Directory.GetFiles(scrcpyDir, "scrcpy.exe", SearchOption.AllDirectories);
                    if (found.Length > 0)
                    {
                        _scrcpyPath = found[0];
                        LogSuccess($"[投屏] scrcpy 下载完成: {_scrcpyPath}");
                        ShowNotify("scrcpy 已就绪", false);
                    }
                    else
                    {
                        LogError("[投屏] scrcpy.exe 未在压缩包中找到");
                        ShowNotify("scrcpy 下载失败：文件异常", true);
                    }
                }
                else
                {
                    LogError("[投屏] 下载 scrcpy 失败");
                    ShowNotify("scrcpy 下载失败", true);
                }
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 下载失败: {ex.Message}");
                ShowNotify($"下载失败: {ex.Message}", true);
            }
            finally
            {
                _isDownloading = false;
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private string BuildArguments(string mode, string? crop = null, string? recordPath = null, bool fullscreen = false)
        {
            var args = "";

            var resolution = (CmbResolution.SelectedItem as System.Windows.Controls.ComboBoxItem)?.Content?.ToString();
            if (resolution != null && resolution != "原始分辨率")
            {
                var parts = resolution.Split('x');
                if (parts.Length == 2)
                    args += $" --max-size {parts[0]}";
            }

            var bitrate = (CmbBitrate.SelectedItem as System.Windows.Controls.ComboBoxItem)?.Content?.ToString();
            if (!string.IsNullOrEmpty(bitrate))
                args += $" --video-bit-rate {bitrate}";   // scrcpy 2.0 起 --bit-rate 已改名（4.1 用它直接报 unknown option）

            var fps = (CmbFps.SelectedItem as System.Windows.Controls.ComboBoxItem)?.Content?.ToString();
            if (!string.IsNullOrEmpty(fps))
                args += $" --max-fps {fps}";

            if (ChkAudio.IsChecked == true)
                args += " --audio-codec=aac";
            else
                args += " --no-audio";

            if (ChkStayAwake.IsChecked == true)
                args += " --stay-awake";

            if (!string.IsNullOrEmpty(crop))
                args += $" --crop={crop}";

            if (!string.IsNullOrEmpty(recordPath))
                args += $" --record \"{recordPath}\"";
            else if (fullscreen)
                args += " --fullscreen";

            if (mode == "usb")
            {
                if (ChkOtori.IsChecked == true)
                    args += " --otg";
                else if (string.IsNullOrEmpty(recordPath))
                    args += " --turn-screen-off";
            }
            else if (mode == "wifi")
            {
                if (ChkOtori.IsChecked == true)
                    LogWarning("[投屏] OTG 控制仅支持 USB 连接，已忽略该选项");

                var ip = TxtDeviceIp.Text?.Trim();
                if (!string.IsNullOrEmpty(ip) && ip != "192.168.")
                {
                    // scrcpy 2.0 起不再接受「位置参数 = IP:端口」（4.1 会报 Unexpected additional argument），
                    // 必须用 -s/--serial；且用户可能自己带了端口，避免拼成 1.2.3.4:5555:5555。
                    var host = ip.Contains(':') ? ip : ip + ":5555";
                    args += $" -s {host}";
                }
            }

            return args;
        }

        /// <summary>勾选「裁剪屏幕」时，按设备实际分辨率与状态栏/导航栏高度计算 --crop 参数。</summary>
        private async Task<string?> BuildCropArgumentAsync()
        {
            if (ChkCrop.IsChecked != true)
                return null;

            var (sizeOk, sizeOutput, _) = await _tools.ExecuteAdbAsync("shell wm size", 15000);
            if (!sizeOk)
            {
                LogWarning($"[投屏] 读取设备分辨率失败，已忽略裁剪: {sizeOutput.Trim()}");
                return null;
            }

            var sizeMatches = Regex.Matches(sizeOutput, @"(\d{3,5})x(\d{3,5})");
            if (sizeMatches.Count == 0)
            {
                LogWarning("[投屏] 未能解析设备分辨率，已忽略裁剪");
                return null;
            }

            var size = sizeMatches[sizeMatches.Count - 1];
            if (!int.TryParse(size.Groups[1].Value, out var width) ||
                !int.TryParse(size.Groups[2].Value, out var height) ||
                width <= 0 || height <= 0)
            {
                LogWarning("[投屏] 设备分辨率数值异常，已忽略裁剪");
                return null;
            }

            var top = 0;
            var bottom = 0;
            var (insetOk, insetOutput, _) = await _tools.ExecuteAdbAsync("shell dumpsys window", 30000);

            if (insetOk)
            {
                var insetMatch = Regex.Match(insetOutput, @"stable[Ii]?nsets?=?\[?Rect\(?(\d+),\s*(\d+)\s*-\s*(\d+),\s*(\d+)\)");
                if (!insetMatch.Success)
                    insetMatch = Regex.Match(insetOutput, @"mStableInsets=Rect\((\d+),\s*(\d+)\s*-\s*(\d+),\s*(\d+)\)");

                if (insetMatch.Success)
                {
                    int.TryParse(insetMatch.Groups[2].Value, out top);
                    int.TryParse(insetMatch.Groups[4].Value, out bottom);
                }
            }

            if (top <= 0 && bottom <= 0)
            {
                LogWarning("[投屏] 未能识别状态栏 / 导航栏高度，已忽略裁剪");
                return null;
            }

            var cropHeight = height - top - bottom;
            if (cropHeight <= 0 || cropHeight > height)
            {
                LogWarning("[投屏] 计算出的裁剪区域无效，已忽略裁剪");
                return null;
            }

            var crop = $"{width}:{cropHeight}:0:{top}";
            LogInfo($"[投屏] 裁剪区域: {crop}（状态栏 {top}px，导航栏 {bottom}px）");
            return crop;
        }

        private async Task StartScrcpyAsync(string mode, string? recordPath = null, bool fullscreen = false)
        {
            if (string.IsNullOrEmpty(_scrcpyPath) || !File.Exists(_scrcpyPath))
            {
                ShowError("scrcpy 未找到，请先点击「重新下载 scrcpy」");
                return;
            }

            // 日志「三明治」上半：区段标题（PageLog.Section 同时把外壳状态栏的「任务」段刷成这个名字）。
            // 「一次操作」= 启动一次投屏会话，录制 / 切全屏同样走这里。
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, string.IsNullOrEmpty(recordPath)
                ? (mode == "wifi" ? "无线投屏" : "USB 投屏")
                : "录制屏幕");

            try
            {
                _lastMode = mode;

                if (IsMirroring)
                {
                    LogInfo("[投屏] 已存在投屏会话，正在重启以应用新参数...");
                    StopScrcpy();
                    await Task.Delay(400);
                }

                var crop = await BuildCropArgumentAsync();
                var args = BuildArguments(mode, crop, recordPath, fullscreen);
                LogInfo($"[投屏] 启动 scrcpy {args}");

                var process = new Process
                {
                    EnableRaisingEvents = true,
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = _scrcpyPath,
                        Arguments = args,
                        UseShellExecute = false,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        CreateNoWindow = true
                    }
                };

                // 🔴 2026-10-08：给 scrcpy 指路 adb。scrcpy 4.1 **没有** --adb 参数
                //    （实测 `--help` 里查无此项），它认的是 **ADB 环境变量**
                //    （帮助「Environment variables: ADB — Path to adb executable」）。
                //    不设的话它只在 PATH 和**自己所在目录**里找 adb —— 而服务端下发的
                //    scrcpy.zip 已经剔掉了那份和 platform-tools **完全重复**的 adb.exe
                //    （压缩后 3.89MB，占整包 37%；剔除后 10.78MB → 6.78MB），
                //    所以在自己目录里是找不到的，必须显式指到我们自己的 platform-tools。
                //    实测：ADB 指向不存在的路径 → 「Command not found … define its fullpath
                //    in the ADB environment variable」；指向真实 adb → 正常
                //    「daemon started successfully」。用 OS 的环境变量而不是改 PATH，
                //    是为了不污染子进程之外的任何东西。
                //
                //    ⚠️ 顺序约束：**必须先发新版客户端、再换服务端那个瘦包**。
                //    老客户端不设 ADB、又拿到没有本地 adb 的 scrcpy.zip，投屏会直接起不来。
                var adbPath = _tools.GetAdbPath();
                if (!File.Exists(adbPath))
                {
                    LogInfo("[投屏] platform-tools 未就位，先准备 adb…");
                    await _tools.EnsureToolsExistAsync();
                    adbPath = _tools.GetAdbPath();
                }

                if (File.Exists(adbPath))
                {
                    process.StartInfo.EnvironmentVariables["ADB"] = adbPath;
                }
                else
                {
                    // 兜底不静默：真找不到就明说，否则现象是「投屏启动后立刻退出、日志只有一句
                    // Command not found」，排查时最难定位。
                    LogWarning($"[投屏] 未找到 adb.exe（{adbPath}），scrcpy 将退回 PATH 查找 adb");
                }

                process.OutputDataReceived += (s, e) =>
                {
                    if (!string.IsNullOrEmpty(e.Data))
                        _scrcpyOut.Enqueue((e.Data, Error: false));
                };
                process.ErrorDataReceived += (s, e) =>
                {
                    if (!string.IsNullOrEmpty(e.Data))
                        _scrcpyOut.Enqueue((e.Data, Error: true));
                };
                process.Exited += (s, e) =>
                {
                    Dispatcher.Invoke(() =>
                    {
                        // 退出提示必须排在最后一行输出之后
                        _scrcpyOut.FlushNow();
                        LogInfo("[投屏] scrcpy 已退出");
                        if (ReferenceEquals(_scrcpyProcess, process))
                            _scrcpyProcess = null;
                    });
                };

                // 2026-10-02 修复：必须在 Start() **成功后**才登记字段 —— 原先 Start() 一抛
                // （scrcpy.exe 被隔离/损坏），字段里就留下一个从未启动的 Process，
                // 之后每次 IsMirroring 访问 HasExited 都抛 InvalidOperationException：
                // 本页从此永远无法投屏（页面缓存常驻），「停止投屏」还会以 async-void
                // 未捕获异常直接把应用打崩。失败时字段保持 null，状态干净。
                process.Start();
                _scrcpyProcess = process;
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();

                LogSuccess("[投屏] scrcpy 已启动");
                ShowSuccess(string.IsNullOrEmpty(recordPath) ? "投屏已启动" : $"正在录制到 {Path.GetFileName(recordPath)}");
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 启动失败: {ex.Message}");
                ShowError($"启动失败: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private bool IsMirroring => _scrcpyProcess != null && !_scrcpyProcess.HasExited;

        private void StopScrcpy()
        {
            try
            {
                var process = _scrcpyProcess;
                if (process == null)
                    return;

                _scrcpyProcess = null;

                if (!process.HasExited)
                {
                    process.Kill();
                    _scrcpyOut.FlushNow();   // 退出前的回包先落地，「已停止」才不会插到它们前面
                    LogInfo("[投屏] 已停止");
                }

                process.Dispose();
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 停止失败: {ex.Message}");
            }
        }

        // 只在「新开一次投屏」时扣额度；录制 / 切全屏是对已在跑的会话复用，
        // 所以校验放在这两个按钮上，不放进 StartScrcpyAsync（否则切个全屏也吃一次额度）。
        private async void BtnStartUsb_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.GuardAndConsume("屏幕镜像", "mirror.scrcpy")) return;
            await StartScrcpyAsync("usb");
        }

        private async void BtnStartWifi_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.GuardAndConsume("屏幕镜像", "mirror.scrcpy")) return;
            await StartScrcpyAsync("wifi");
        }
        private void BtnStopMirror_Click(object sender, RoutedEventArgs e) => StopScrcpy();
        private async void BtnDownloadScrcpy_Click(object sender, RoutedEventArgs e) => await DownloadScrcpyAsync();

        private async void BtnConnectWifi_Click(object sender, RoutedEventArgs e)
        {
            var ip = TxtDeviceIp.Text?.Trim();
            if (string.IsNullOrEmpty(ip) || ip == "192.168.")
            {
                ShowError("请输入设备IP地址");
                return;
            }

            // 日志「三明治」：一次连接 = 一个区段；原来的 LogInfo 换成 Section（同一句话不打两遍）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, $"连接设备 {ip}:5555");
            try
            {
                if (!await _tools.EnsureToolsExistAsync())
                {
                    ShowError("adb 不可用，请检查网络后重试");
                    return;
                }

                var (ok, output, error) = await _tools.ExecuteAdbAsync($"connect {ip}:5555", 30000);
                var text = (string.IsNullOrWhiteSpace(output) ? error : output).Trim();

                if (ok && text.Contains("connected", StringComparison.OrdinalIgnoreCase))
                {
                    LogSuccess($"[投屏] 已连接 {ip}:5555");
                    ShowSuccess($"已连接 {ip}");
                }
                else
                {
                    LogWarning($"[投屏] 连接失败: {text}");
                    ShowError($"连接失败: {text}");
                }
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 连接失败: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnScreenshot_Click(object sender, RoutedEventArgs e)
        {
            // 日志「三明治」：截图 = screencap + pull 两条 adb 合成一次操作
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, "截图");
            try
            {
                if (!await _tools.EnsureToolsExistAsync())
                {
                    ShowError("adb 不可用，请检查网络后重试");
                    return;
                }

                var savePath = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.Desktop),
                    $"screenshot_{DateTime.Now:yyyyMMdd_HHmmss}.png");

                var (ok, _, error) = await _tools.ExecuteAdbAsync("shell screencap -p /sdcard/kun_screenshot.png", 30000);
                if (!ok)
                {
                    LogError($"[投屏] 截图失败: {error}");
                    ShowError($"截图失败: {error}");
                    return;
                }

                var (pullOk, _, pullError) = await _tools.ExecuteAdbAsync($"pull /sdcard/kun_screenshot.png \"{savePath}\"", 60000);
                await _tools.ExecuteAdbAsync("shell rm -f /sdcard/kun_screenshot.png", 15000);

                if (!pullOk || !File.Exists(savePath))
                {
                    LogError($"[投屏] 截图导出失败: {pullError}");
                    ShowError("截图导出失败");
                    return;
                }

                LogSuccess($"[投屏] 截图已保存: {savePath}");
                ShowSuccess("截图已保存到桌面");
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 截图失败: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnRecord_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.SaveFileDialog
            {
                Title = "保存录屏文件",
                FileName = $"scrcpy_{DateTime.Now:yyyyMMdd_HHmmss}.mp4",
                Filter = "MP4 视频|*.mp4|MKV 视频|*.mkv"
            };

            if (dialog.ShowDialog() != true)
                return;

            LogInfo($"[投屏] 开始录制到 {dialog.FileName}");
            await StartScrcpyAsync(_lastMode, dialog.FileName);
        }

        private async void BtnFullscreen_Click(object sender, RoutedEventArgs e) => await StartScrcpyAsync(_lastMode, fullscreen: true);

        private async void BtnTurnOff_Click(object sender, RoutedEventArgs e)
        {
            if (!IsMirroring)
            {
                ShowNotify("投屏未启动：USB 投屏启动时会自动关闭设备屏幕", false);
                LogInfo("[投屏] 息屏投屏：USB 模式启动 scrcpy 时会自动执行 --turn-screen-off");
                return;
            }

            try
            {
                if (!await _tools.EnsureToolsExistAsync())
                {
                    ShowError("adb 不可用，请检查网络后重试");
                    return;
                }

                var (ok, _, error) = await _tools.ExecuteAdbAsync("shell input keyevent 26", 15000);
                if (ok)
                {
                    LogSuccess("[投屏] 已关闭设备屏幕");
                    ShowSuccess("已关闭设备屏幕");
                }
                else
                {
                    LogError($"[投屏] 息屏失败: {error}");
                    ShowError($"息屏失败: {error}");
                }
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 息屏失败: {ex.Message}");
            }
        }

        private async void BtnBack_Click(object sender, RoutedEventArgs e) => await RunAdbKeyeventAsync(4);
        private async void BtnHome_Click(object sender, RoutedEventArgs e) => await RunAdbKeyeventAsync(3);
        private async void BtnRecent_Click(object sender, RoutedEventArgs e) => await RunAdbKeyeventAsync(187);

        private async Task RunAdbKeyeventAsync(int keycode)
        {
            try
            {
                if (!await _tools.EnsureToolsExistAsync())
                {
                    ShowError("adb 不可用，请检查网络后重试");
                    return;
                }

                var (ok, _, error) = await _tools.ExecuteAdbAsync($"shell input keyevent {keycode}", 15000);
                if (!ok)
                {
                    LogWarning($"[投屏] 按键发送失败: {error}");
                    ShowError("adb 命令执行失败");
                }
            }
            catch (Exception ex)
            {
                LogError($"[投屏] 按键发送失败: {ex.Message}");
            }
        }

        private void ShowNotify(string message, bool isError)
        {
            NotifyBar.Visibility = Visibility.Visible;
            NotifyIcon.Text = isError ? "\u274C" : "\u2705";
            NotifyText.Text = message;
            // 配色改走主题令牌（原来自行拼的 "#FEF2F2/#DC2626" 等 Tailwind 色值已删）：
            // 写死色值 = 深色主题下留浅色补丁。语义与 Notify.Show 同一口径 ——
            // 失败 = Danger 文字 + DangerBg 底，成功 = Success 文字 + SuccessBg 底。
            var bgKey = isError ? "Theme.DangerBg" : "Theme.SuccessBg";
            var fgKey = isError ? "Theme.Danger" : "Theme.Success";
            if (Application.Current?.TryFindResource(bgKey) is System.Windows.Media.Brush bgColor)
                NotifyBar.Background = bgColor;
            if (Application.Current?.TryFindResource(fgKey) is System.Windows.Media.Brush fgColor)
            {
                NotifyIcon.Foreground = fgColor;
                NotifyText.Foreground = fgColor;
            }
        }
    }
}
