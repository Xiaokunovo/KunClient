using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    /// <summary>
    /// 小米线刷（类 MiFlash）：
    /// 选择解压后的官方线刷包目录（或直接选 .tgz 自动解压）→ 检测 flash_all 系列脚本 →
    /// 按三档语义（全部清除 / 保留用户数据 / 全部清除并回锁）执行对应 bat，脚本输出实时进日志，
    /// 进度按脚本内 fastboot 命令数估算推进。与 MiFlash 一致，实际刷写逻辑完全交给官方脚本（含防回滚/机型校验）。
    /// </summary>
    public partial class XiaomiFastbootPage : BasePage
    {
        /// <summary>刷写方式 → 官方脚本映射（与 MiFlash 三档一致）。</summary>
        private static readonly (string Title, string File, bool WipesData, bool LocksBl)[] ScriptOptions =
        {
            ("全部清除", "flash_all.bat", true, false),
            ("保留用户数据", "flash_all_except_storage.bat", false, false),
            ("全部清除并回锁", "flash_all_lock.bat", true, true),
        };

        private readonly PlatformToolsManager _tools;

        private string _romDir = "";
        private int _estSteps;
        private int _doneSteps;
        private bool _failedSeen;
        private bool _busy;
        private CancellationTokenSource? _cts;

        public XiaomiFastbootPage()
        {
            InitializeComponent();
            // 字段初始化器里不能引用实例方法（BasePage.LogInfo），所以放构造函数；此时 TxtLog 名字已注册，日志能正常落框
            _tools = new PlatformToolsManager(msg => LogInfo(msg));
        }

        // ─────────────────────────── 线刷包 ───────────────────────────

        private void BtnBrowseFolder_Click(object sender, RoutedEventArgs e)
        {
            if (_busy) return;
            var dialog = new OpenFolderDialog { Title = "选择线刷包目录（根目录需含 flash_all.bat）" };
            if (dialog.ShowDialog() != true) return;
            TxtRomPath.Text = dialog.FolderName;
        }

        private async void BtnBrowseTgz_Click(object sender, RoutedEventArgs e)
        {
            if (_busy) return;
            var dialog = new OpenFileDialog { Title = "选择小米官方线刷包（.tgz）", Filter = "小米线刷包|*.tgz;*.tar.gz" };
            if (dialog.ShowDialog() != true) return;

            var archive = dialog.FileName;
            // a.tgz → a；a.tar.gz → a.tar（再去一次尾巴）
            var destName = Path.GetFileNameWithoutExtension(archive);
            if (destName.EndsWith(".tar", StringComparison.OrdinalIgnoreCase))
                destName = destName[..^4];
            var dest = Path.Combine(Path.GetDirectoryName(archive) ?? ".", destName);

            if (Directory.Exists(dest) && Directory.EnumerateFileSystemEntries(dest).Any())
            {
                ShowError($"解压目录已存在且非空，请先手动清理或重命名：\n{dest}");
                return;
            }

            SetBusy(true);
            PrgFlash.IsIndeterminate = true;
            TxtStage.Text = "正在解压线刷包…";
            // 日志三明治：Section 把外壳「任务」段设成「解压线刷包」，收尾在 finally 里统一补。
            var sw = System.Diagnostics.Stopwatch.StartNew();
            var ok = false;
            PageLog.Section(LogTarget, "解压线刷包");
            LogInfo($"[小米线刷] 解压 {archive}");
            LogInfo($"[小米线刷] 目标目录 {dest}");
            try
            {
                var (extractOk, error) = await _tools.ExtractTarGzAsync(archive, dest,
                    s => Dispatcher.InvokeAsync(() => LogInfo($"[tar] {s}")), CancellationToken.None);
                if (!extractOk)
                {
                    LogError($"[小米线刷] 解压失败: {error}");
                    ShowError($"解压失败: {error}");
                    return;
                }

                ok = true;
                LogSuccess("[小米线刷] 解压完成");
                // 官方包有的带一层根目录，有的直接铺在根上：根上没有 flash_all.bat 且只有唯一子目录时进入子目录
                var root = dest;
                if (!File.Exists(Path.Combine(root, "flash_all.bat")))
                {
                    var dirs = Directory.GetDirectories(root);
                    if (dirs.Length == 1 && !Directory.EnumerateFiles(root).Any())
                        root = dirs[0];
                }
                TxtRomPath.Text = root;
                ShowSuccess("解压完成，已加载线刷包");
            }
            catch (Exception ex)
            {
                LogError($"[小米线刷] 解压异常: {ex.Message}");
                ShowError($"解压异常: {ex.Message}");
            }
            finally
            {
                PrgFlash.IsIndeterminate = false;
                SetBusy(false);
                SetProgress(0);
                TxtStage.Text = "空闲";
                // 收尾：成功 → 外壳进度条停 100%；失败只改任务名（照 EdlFlashPage 的收尾口径）。
                if (ok) ShellStatus.EndTask();
                else ShellStatus.Report(task: "解压失败");
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void TxtRomPath_TextChanged(object sender, TextChangedEventArgs e) => DetectRom();

        private void BtnRescan_Click(object sender, RoutedEventArgs e) => DetectRom();

        private void DetectRom()
        {
            _romDir = TxtRomPath.Text?.Trim() ?? "";
            if (string.IsNullOrEmpty(_romDir))
            {
                TxtRomInfo.Text = "请选择解压后的线刷包目录（根目录需含 flash_all.bat），或直接选择官方 .tgz 线刷包自动解压";
                return;
            }
            if (!Directory.Exists(_romDir))
            {
                TxtRomInfo.Text = "目录不存在，请重新选择";
                return;
            }

            var found = ScriptOptions.Count(o => File.Exists(Path.Combine(_romDir, o.File)));
            var scriptSummary = string.Join(" / ", ScriptOptions.Select(o =>
                File.Exists(Path.Combine(_romDir, o.File)) ? $"{o.File} 有" : $"{o.File} 缺"));

            var imagesDir = Path.Combine(_romDir, "images");
            var imageCount = Directory.Exists(imagesDir)
                ? Directory.GetFiles(imagesDir, "*.img").Length
                  + Directory.GetFiles(imagesDir, "*.mbn").Length
                  + Directory.GetFiles(imagesDir, "*.bin").Length
                : -1;

            var parts = found > 0
                ? $"刷写脚本 {found}/{ScriptOptions.Length} 可用（{scriptSummary}）"
                : "未找到 flash_all 系列脚本，所选目录可能不是线刷包";
            parts += imageCount >= 0
                ? $" ｜ images 镜像 {imageCount} 个"
                : " ｜ 未找到 images 目录";

            TxtRomInfo.Text = parts;
            LogInfo($"[小米线刷] ROM 检测: {parts}");
        }

        // ─────────────────────────── 设备 ───────────────────────────

        private async void BtnDetectDevice_Click(object sender, RoutedEventArgs e)
        {
            if (_busy) return;
            if (!await _tools.EnsureToolsExistAsync())
            {
                ShowError("fastboot 工具不可用，请检查网络后重试");
                return;
            }

            BtnDetectDevice.IsEnabled = false;
            // 日志三明治：Section 把外壳「任务」段设成「检测设备」，收尾在 finally 里统一补。
            var sw = System.Diagnostics.Stopwatch.StartNew();
            var detected = false;
            try
            {
                PageLog.Section(LogTarget, "检测设备");
                var (ok, output, error) = await _tools.ExecuteFastbootAsync("devices", 15000);
                var text = (output + "\n" + error);
                var serial = text.Split('\n')
                    .Select(l => l.Trim())
                    .FirstOrDefault(l => l.EndsWith("fastboot", StringComparison.Ordinal))
                    ?.Split(' ')[0];

                if (string.IsNullOrEmpty(serial))
                {
                    TxtDeviceInfo.Text = "未检测到 Fastboot 设备 · 请确认设备已进入 Fastboot 模式并安装驱动";
                    LogWarning("[小米线刷] 未检测到 Fastboot 设备");
                    return;
                }

                var product = await GetVarAsync("product");
                var anti = await GetVarAsync("anti");
                var unlocked = await GetVarAsync("unlocked");
                var slot = await GetVarAsync("current-slot");
                var unlockedText = unlocked == "yes" ? "已解锁" : unlocked == "no" ? "未解锁" : "未知";

                TxtDeviceInfo.Text = $"序列号 {serial} ｜ 机型 {OrDash(product)} ｜ 反回滚 {OrDash(anti)} ｜ " +
                                     $"Bootloader {unlockedText} ｜ 槽位 {OrDash(slot)}";
                LogSuccess($"[小米线刷] 设备已连接: {serial}（{OrDash(product)}）");
                detected = true;
                // 探测结论广播给 DeviceStateHub：外壳底部状态栏与各页左栏「设备信息」都吃这份广播，
                // 外壳不必再起一套 adb/fastboot 探测，否则两条命令会撞同一台设备互相打断
                //（见 DeviceStateHub 的注释）；本页左栏自己的读数则直接落在 TxtDeviceInfo。
                DeviceStateHub.Report(serial, "Fastboot 模式",
                    new DeviceStateHub.Row("机型", OrDash(product)),
                    new DeviceStateHub.Row("Bootloader", unlockedText),
                    new DeviceStateHub.Row("槽位", OrDash(slot)));
            }
            catch (Exception ex)
            {
                LogError($"[小米线刷] 设备检测异常: {ex.Message}");
                ShowError($"设备检测异常: {ex.Message}");
            }
            finally
            {
                BtnDetectDevice.IsEnabled = true;
                PageLog.Footer(LogTarget, $"{(detected ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async Task<string> GetVarAsync(string key)
        {
            var (ok, output, error) = await _tools.ExecuteFastbootAsync($"getvar {key}", 10000);
            var match = Regex.Match(output + "\n" + error, $@"(?im)^\s*(?:\(bootloader\)\s*)?{Regex.Escape(key)}:\s*(.+?)\s*$");
            return match.Success ? match.Groups[1].Value : "";
        }

        private static string OrDash(string value) => string.IsNullOrWhiteSpace(value) || value == "(null)" ? "-" : value;

        // ─────────────────────────── 刷写 ───────────────────────────

        private async void BtnStart_Click(object sender, RoutedEventArgs e)
        {
            if (_busy) return;
            var option = ScriptOptions[Math.Max(0, CmbScript.SelectedIndex)];
            var scriptPath = Path.Combine(_romDir, option.File);

            if (string.IsNullOrEmpty(_romDir) || !Directory.Exists(_romDir))
            {
                ShowError("请先选择线刷包目录");
                return;
            }
            if (!File.Exists(scriptPath))
            {
                ShowError($"刷写脚本不存在: {option.File}\n请确认所选目录是官方线刷包，或换一种刷写方式");
                return;
            }

            // 二次确认必须在扣额度之前
            var confirmText = option.LocksBl
                ? $"「{option.Title}」将清除全部用户数据并回锁 Bootloader。\n回锁后若刷写中断可能变砖，且数据无法恢复。确定继续？"
                : option.WipesData
                    ? $"「{option.Title}」将清除全部用户数据，确定继续？"
                    : $"「{option.Title}」保留用户数据刷写。若系统版本变化仍可能出现异常，建议提前备份。确定继续？";
            if (MessageBox.Show(confirmText, "确认刷写", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
                return;

            if (!Services.MembershipService.GuardAndConsume("小米线刷", "fastboot.flash")) return;

            if (!await _tools.EnsureToolsExistAsync())
            {
                ShowError("fastboot 工具不可用，请检查网络后重试");
                return;
            }

            var (devOk, devOut, devErr) = await _tools.ExecuteFastbootAsync("devices", 15000);
            if (!devOk || !(devOut + devErr).Contains("fastboot"))
            {
                ShowError("未检测到 Fastboot 设备，请确认设备已进入 Fastboot 模式并安装驱动");
                return;
            }

            _estSteps = CountFlashSteps(scriptPath);
            if (_estSteps <= 0)
            {
                LogWarning("[小米线刷] 未从脚本解析到 fastboot 刷写命令，进度将按完成事件推进");
                _estSteps = 1;
            }
            _doneSteps = 0;
            _failedSeen = false;
            _busy = true;
            SetBusy(true);
            SetProgress(0);
            TxtStage.Text = "准备刷写…";

            _cts = new CancellationTokenSource();
            // 刷写类操作：Section 把外壳状态栏「任务」段设成刷写方式名；
            // 进度已在 SetProgress 里镜像给外壳；成败与耗时在 finally 统一收尾。
            var sw = System.Diagnostics.Stopwatch.StartNew();
            var ok = false;
            // 「用户是否点了停止」要在 finally 里用，所以提到 try 外面记一份
            // （try 里的 cancelled 是 ExecuteScriptStreamingAsync 解构出来的，出了 try 就看不见了）
            var userStopped = false;
            PageLog.Section(LogTarget, $"线刷刷写（{option.Title}）");
            LogInfo($"[小米线刷] 开始刷写: {option.Title}（{option.File}）");
            LogInfo($"[小米线刷] ROM 目录: {_romDir}");
            LogInfo($"[小米线刷] 预计刷写步骤 {_estSteps} 步");

            try
            {
                var (exitCode, cancelled) = await _tools.ExecuteScriptStreamingAsync(scriptPath, _romDir, OnScriptLine, _cts.Token);
                userStopped = cancelled;
                if (cancelled)
                {
                    LogWarning("[小米线刷] 已手动停止，设备可能处于半刷写状态，建议重新完整刷写");
                    ShowError("刷写已停止");
                    TxtStage.Text = "已停止";
                }
                else if (exitCode == 0 && !_failedSeen)
                {
                    ok = true;
                    SetProgress(100);
                    TxtStage.Text = "刷写完成";
                    LogSuccess("[小米线刷] 线刷完成");
                    ShowSuccess("线刷完成，设备将自动重启");
                }
                else
                {
                    TxtStage.Text = "刷写失败";
                    var reason = _failedSeen ? "脚本输出含 FAILED" : $"退出码 {exitCode}";
                    LogError($"[小米线刷] 线刷失败（{reason}），请检查日志中的红字与镜像/机型是否匹配");
                    ShowError($"线刷失败（{reason}）");
                }
            }
            catch (Exception ex)
            {
                TxtStage.Text = "刷写异常";
                LogError($"[小米线刷] 刷写异常: {ex.Message}");
                ShowError($"刷写异常: {ex.Message}");
            }
            finally
            {
                _busy = false;
                _cts?.Dispose();
                _cts = null;
                SetBusy(false);
                // 收尾：成功 → 外壳进度条停 100%；停止/失败只改任务名（照 EdlFlashPage 的收尾口径）。
                if (ok) ShellStatus.EndTask();
                else ShellStatus.Report(task: userStopped ? "已停止" : "刷写失败");
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void BtnStop_Click(object sender, RoutedEventArgs e)
        {
            if (!_busy) return;
            LogWarning("[小米线刷] 正在停止刷写进程…");
            TxtStage.Text = "正在停止…";
            _cts?.Cancel();
        }

        /// <summary>脚本输出行（线程池线程触发）→ 调度回 UI 线程分派日志/进度。</summary>
        private void OnScriptLine(string line)
        {
            Dispatcher.InvokeAsync(() =>
            {
                if (line.Contains("FAILED", StringComparison.OrdinalIgnoreCase))
                {
                    _failedSeen = true;
                    LogError(line);
                    TxtStage.Text = "出现失败步骤";
                    return;
                }
                if (line.Contains("Finished. Total time", StringComparison.Ordinal))
                {
                    _doneSteps++;
                    SetProgress(Math.Min(100, _doneSteps * 100 / _estSteps));
                    LogInfo(line);
                }
                else if (line.Contains("Sending '"))
                {
                    TxtStage.Text = line.Trim();
                    LogInfo(line);
                }
                else if (line.Contains("Writing '"))
                {
                    TxtStage.Text = line.Trim();
                    LogInfo(line);
                }
                else if (line.StartsWith("(bootloader)", StringComparison.OrdinalIgnoreCase))
                {
                    LogInfo(line);
                }
                else
                {
                    LogInfo(line);
                }
            });
        }

        private void SetBusy(bool busy)
        {
            BtnStart.IsEnabled = !busy;
            BtnStop.IsEnabled = busy;
            BtnRescan.IsEnabled = !busy;
            BtnBrowseFolder.IsEnabled = !busy;
            BtnBrowseTgz.IsEnabled = !busy;
            BtnDetectDevice.IsEnabled = !busy;
            CmbScript.IsEnabled = !busy;
        }

        private void SetProgress(int percent)
        {
            PrgFlash.Value = percent;
            TxtPct.Text = $"{percent}%";
            // 页面自己的进度条照旧，同时镜像给外壳底部双进度条（参考稿里那两条属于窗口外壳，
            // 页面不再自己画底部状态栏）。
            ShellStatus.Report(percent: percent);
        }

        /// <summary>统计脚本里的 fastboot flash 命令行数，作为进度分母（估算值，够用）。</summary>
        private static int CountFlashSteps(string scriptPath)
        {
            try
            {
                var text = File.ReadAllText(scriptPath);
                var count = 0;
                foreach (var raw in text.Split('\n'))
                {
                    var line = raw.TrimStart();
                    if (line.StartsWith("::") || line.StartsWith("rem ", StringComparison.OrdinalIgnoreCase)) continue;
                    if (line.IndexOf("fastboot", StringComparison.OrdinalIgnoreCase) >= 0 &&
                        line.IndexOf(" flash ", StringComparison.OrdinalIgnoreCase) >= 0)
                        count++;
                }
                return count;
            }
            catch
            {
                return 0;
            }
        }
    }
}
