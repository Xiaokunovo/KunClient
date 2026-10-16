using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public partial class FileTransferPage : BasePage
    {
        private readonly PlatformToolsManager _tools;

        public FileTransferPage()
        {
            InitializeComponent();
            _tools = new PlatformToolsManager(msg => LogInfo($"[工具] {msg}"));

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

        private void BtnBrowsePush_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog();
            if (dialog.ShowDialog() == true)
            {
                TxtPushLocal.Text = dialog.FileName;
            }
        }

        private async void BtnPushFile_Click(object sender, RoutedEventArgs e)
        {
            var local = TxtPushLocal.Text?.Trim();
            var remote = TxtPushRemote.Text?.Trim();
            if (string.IsNullOrEmpty(local) || string.IsNullOrEmpty(remote))
            {
                ShowError("请选择文件和目标路径");
                return;
            }
            if (!File.Exists(local))
            {
                ShowError("本地文件不存在");
                return;
            }
            if (!Services.MembershipService.GuardAndConsume("文件传输", "file.transfer")) return;
            await RunAdbCommandAsync($"push \"{local}\" \"{remote}\"", $"推送: {Path.GetFileName(local)} -> {remote}");
        }

        private async void BtnPullFile_Click(object sender, RoutedEventArgs e)
        {
            var remote = TxtPullRemote.Text?.Trim();
            if (string.IsNullOrEmpty(remote))
            {
                ShowError("请输入设备文件路径");
                return;
            }
            var dialog = new SaveFileDialog();
            if (dialog.ShowDialog() == true)
            {
                if (!Services.MembershipService.GuardAndConsume("文件传输", "file.transfer")) return;
                await RunAdbCommandAsync($"pull \"{remote}\" \"{dialog.FileName}\"", $"拉取: {remote} -> {dialog.FileName}");
            }
        }

        private void BtnBrowseBatchApk_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFolderDialog();
            if (dialog.ShowDialog() == true)
            {
                TxtBatchApkDir.Text = dialog.FolderName;
            }
        }

        private async void BtnBatchInstall_Click(object sender, RoutedEventArgs e)
        {
            var dir = TxtBatchApkDir.Text?.Trim();
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
            {
                ShowError("请选择有效的APK目录");
                return;
            }

            var apkFiles = Directory.GetFiles(dir, "*.apk");
            if (apkFiles.Length == 0)
            {
                ShowError("目录下没有 APK 文件");
                return;
            }

            if (!Services.MembershipService.GuardAndConsume("文件传输", "file.transfer")) return;
            LogInfo($"[传输] 找到 {apkFiles.Length} 个APK文件");
            var succeeded = 0;

            for (var i = 0; i < apkFiles.Length; i++)
            {
                var name = Path.GetFileName(apkFiles[i]);
                SetProgress(true, $"安装中 ({i + 1}/{apkFiles.Length}): {name}");
                var output = await RunAdbCommandAsync($"install -r \"{apkFiles[i]}\"", $"安装: {name}", showProgress: false);

                if (output.Contains("Success", StringComparison.OrdinalIgnoreCase))
                    succeeded++;
                else
                    LogWarning($"[传输] {name} 安装可能失败");
            }

            SetProgress(false, $"安装结束：成功 {succeeded} / {apkFiles.Length}");
            if (succeeded == apkFiles.Length)
                ShowSuccess($"全部 {apkFiles.Length} 个 APK 安装完成");
            else
                ShowError($"安装结束：成功 {succeeded} / {apkFiles.Length}");
        }

        private async void BtnBrowseDevice_Click(object sender, RoutedEventArgs e)
        {
            await RefreshDeviceDirectoryAsync();
        }

        private async Task RefreshDeviceDirectoryAsync()
        {
            var path = TxtDevicePath.Text?.Trim();
            if (string.IsNullOrEmpty(path))
                path = "/sdcard/";

            var output = await RunAdbCommandAsync($"shell ls -la \"{path}\"", $"浏览目录: {path}");

            // 🔴 一次构造完再整体换 ItemsSource：原来 Clear() + 逐行 Items.Add = 每行一次
            //    集合变更通知 + 每行 FindResource 一次（`ls -la` 几百行时逐行插容器、逐行重排）。
            var rows = new System.Collections.Generic.List<ListBoxItem>();
            if (!string.IsNullOrWhiteSpace(output))
            {
                var fg = (System.Windows.Media.Brush)FindResource("Theme.TextPrimary");
                foreach (var line in output.Split('\n'))
                {
                    var text = line.Trim();
                    if (text.Length == 0)
                        continue;

                    rows.Add(new ListBoxItem
                    {
                        Content = text,
                        Foreground = fg
                    });
                }
            }
            LstDeviceFiles.ItemsSource = rows;
        }

        private async void BtnParentDir_Click(object sender, RoutedEventArgs e)
        {
            var path = TxtDevicePath.Text?.Trim() ?? "/sdcard/";
            var parent = Path.GetDirectoryName(path)?.Replace("\\", "/");
            TxtDevicePath.Text = string.IsNullOrEmpty(parent) ? "/" : parent;
            await RefreshDeviceDirectoryAsync();
        }

        private void DropZone_DragEnter(object sender, DragEventArgs e)
        {
            e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
        }

        private async void DropZone_Drop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop))
                return;

            var files = (string[])e.Data.GetData(DataFormats.FileDrop)!;
            var remote = TxtPushRemote.Text?.Trim() ?? "/sdcard/";

            // 2026-10-02 修复：拖放推送原先**不做额度校验** —— 按钮推送走
            // GuardAndConsume("文件传输")，拖 N 个文件却一次都不扣，同一操作两条口径。
            // 统一在推送前做一次额度守卫（与按钮同口径，一批只扣一次）。
            if (!Services.MembershipService.GuardAndConsume("文件传输", "file.transfer")) return;

            for (var i = 0; i < files.Length; i++)
            {
                var name = Path.GetFileName(files[i]);
                SetProgress(true, $"推送中 ({i + 1}/{files.Length}): {name}");
                await RunAdbCommandAsync($"push \"{files[i]}\" \"{remote}\"", $"推送: {name}", showProgress: false);
            }

            SetProgress(false, $"已推送 {files.Length} 个文件到 {remote}");
        }

        private void SetProgress(bool busy, string text)
        {
            ProgressBar.IsIndeterminate = busy;
            if (!busy)
                ProgressBar.Value = 100;
            TxtProgress.Text = text;
        }

        private async Task<string> RunAdbCommandAsync(string arguments, string description, bool showProgress = true)
        {
            // 日志「三明治」（参考稿日志区的形状）：
            //   · PageLog.Section 起一个区段标题，同时把外壳状态栏的「任务」段刷成这个名字；
            //   · PageLog.Footer 在结尾补一行右对齐的「完成, 耗时: x.xx秒」。
            // 原来的 LogInfo($"[ADB] {description}") 由 Section 取代 —— 同一句话不打两遍。
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, description);
            try
            {
                if (!await _tools.EnsureToolsExistAsync())
                {
                    ShowError("adb 不可用，请检查网络后重试");
                    TxtProgress.Text = "adb 不可用";
                    return "";
                }

                if (showProgress)
                    SetProgress(true, description);

                var (success, output, error) = await _tools.ExecuteAdbAsync(arguments, 600000);

                if (showProgress)
                    SetProgress(false, success ? $"{description} 完成" : $"{description} 失败");

                if (!string.IsNullOrWhiteSpace(output))
                    LogInfo(output.Trim());
                if (!string.IsNullOrWhiteSpace(error))
                    LogWarning(error.Trim());

                if (!success)
                    ShowError($"{description} 失败");

                return output;
            }
            catch (Exception ex)
            {
                LogError($"[ADB错误] {ex.Message}");
                TxtProgress.Text = "执行失败";
                return "";
            }
            finally
            {
                // 成败都要收尾：区段标题开着不关，日志区看起来像卡在半路
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }
    }
}
