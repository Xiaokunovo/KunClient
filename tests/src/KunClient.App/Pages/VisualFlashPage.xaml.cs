using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public class VisualPartitionInfo
    {
        public bool IsSelected { get; set; }
        public string Name { get; set; } = "";
        public string Size { get; set; } = "";
        public string Type { get; set; } = "";
    }

    public partial class VisualFlashPage : BasePage
    {
        public VisualFlashPage()
        {
            InitializeComponent();

            // 页内左栏「设备信息」读数：本页在 MainWindow 页表里标了 OwnRail:true，
            // 外壳共用左栏（Controls/DeviceLogRail）对本页整列让位，所以读数由页面自己显示。
            // 数据来源与外壳左栏完全一致 —— 设备/模式 ← DeviceStateHub（本页读分区表成功后
            // 会汇报序列号，见 BtnReadPartTable_Click）；任务/进度/状态 ← ShellStatus
            // （各操作的 PageLog.Section 已顺带把任务名上报，写入分区还会报百分比）。
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

        /// <summary>
        /// 平台工具下载的**真实**进度回调（<b>运行在后台线程</b>，必须自己切回 UI 线程）。
        /// <para>
        /// 服务端这个端点不给 <c>Content-Length</c> 时 total 会是 -1 ⇒ 退化成不确定态，
        /// 而不是编一个假百分比出来。
        /// </para>
        /// </summary>
        private void ReportToolsProgress(long got, long total)
        {
            var pct = total > 0 ? (double)got / total * 100 : -1;
            Dispatcher.BeginInvoke(new Action(() =>
            {
                if (pct >= 0)
                {
                    ProgressBar.IsIndeterminate = false;
                    ProgressBar.Value = pct;
                    TxtProgress.Text = $"正在下载平台工具 {pct:F0}%";
                }
                else
                {
                    ProgressBar.IsIndeterminate = true;
                    TxtProgress.Text = "正在下载平台工具…";
                }
                ShellStatus.Report(task: "加载平台工具", percent: pct, partitionPercent: -1);
            }));
        }

        /// <summary>刷写阶段的界面状态：确定进度给百分比，不确定态走 indeterminate。</summary>
        private void SetFlashUi(bool indeterminate, string? text = null)
        {
            ProgressBar.IsIndeterminate = indeterminate;
            if (indeterminate) ProgressBar.Value = 0;
            if (text != null) TxtProgress.Text = text;
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

        private async void BtnReadPartTable_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.GuardAndConsume("读取分区表", "edl.readtable")) return;
            // 日志「三明治」：Section（区段标题，同时把外壳状态栏「任务」段刷成同名）→ 明细 → Footer 收尾统计。
            // 计时从点下按钮算起（含工具准备），用户看到的就是这次点击的真实耗时。
            var sw = Stopwatch.StartNew();
            try
            {
                PageLog.Section(LogTarget, "读取分区表");
                LogInfo("[可视刷写] 读取分区表...");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();

                var partitions = new List<VisualPartitionInfo>();
                var (success, output, _) = await tools.ExecuteFastbootAsync("getvar all");
                if (success && !string.IsNullOrEmpty(output))
                {
                    var lines = output.Split('\n');

                    // 设备探测结论顺带广播给 DeviceStateHub（页内左栏「设备信息」与外壳都吃这份广播，
                    // 谁探测谁汇报 —— 本页是 fastboot getvar all 的第一个成功结果）：
                    // 输出里有一行 `(bootloader) serialno: xxx`，剥掉前缀取序列号。
                    string? serial = null;
                    foreach (var line in lines)
                    {
                        var t = line.Trim();
                        if (t.StartsWith("serialno:", StringComparison.OrdinalIgnoreCase))
                            serial = t["serialno:".Length..].Trim();
                    }
                    if (!string.IsNullOrEmpty(serial))
                        DeviceStateHub.Report(serial, "Fastboot 模式");

                    // 2026-10-02 修复：fastboot getvar all 的真实分区行是
                    //   `partition-size:boot: 0x4000000` / `partition-type:boot: ext4`
                    // 原先找的 `partition:` 前缀 / 含 "Partition" 的行 **0 命中** → 表格永远落到
                    // adb by-name 兜底（fastboot 模式下不可用，等于可视刷写在 fastboot 下永远
                    // 「没找到分区」）。照 FastbootFlashPage 的口径解析 partition-size。
                    foreach (var raw in lines)
                    {
                        var m = System.Text.RegularExpressions.Regex.Match(
                            raw.Trim(),
                            @"partition-size:\s*([A-Za-z0-9_\-\.]+)\s*:\s*(0x[0-9A-Fa-f]+|\d+)",
                            System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                        if (!m.Success) continue;
                        var name = m.Groups[1].Value;
                        long bytes = 0;
                        try
                        {
                            var text = m.Groups[2].Value;
                            bytes = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                                ? Convert.ToInt64(text[2..], 16)
                                : long.Parse(text);
                        }
                        catch
                        {
                            // 单个分区的大小解析失败不影响其它分区（Size 显示 0 B）
                        }
                        partitions.Add(new VisualPartitionInfo
                        {
                            Name = name,
                            Size = FormatBytes(bytes),
                            Type = GetPartitionType(name)
                        });
                    }
                }

                if (partitions.Count == 0)
                {
                    var (adbOk, adbOut, _) = await tools.ExecuteAdbAsync("shell ls /dev/block/by-name/");
                    if (adbOk && !string.IsNullOrEmpty(adbOut))
                    {
                        var names = adbOut.Split(new[] { '\r', '\n', ' ' }, StringSplitOptions.RemoveEmptyEntries);
                        foreach (var name in names)
                        {
                            if (!string.IsNullOrWhiteSpace(name) && name.Length > 1)
                            {
                                partitions.Add(new VisualPartitionInfo
                                {
                                    Name = name.Trim(),
                                    Size = "-",
                                    Type = GetPartitionType(name.Trim())
                                });
                            }
                        }
                    }
                }

                if (partitions.Count > 0)
                {
                    DgPartitions.ItemsSource = partitions;
                    LogSuccess($"[可视刷写] 找到 {partitions.Count} 个分区");
                }
                else
                {
                    LogWarning("[可视刷写] 未找到分区信息");
                }
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] 读取分区表失败: {ex.Message}");
                PageLog.Footer(LogTarget, $"失败, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void BtnRefreshPartTable_Click(object sender, RoutedEventArgs e) => BtnReadPartTable_Click(sender, e);

        private async void BtnBackupGpt_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.EnsureAllowed("分区备份 / 导出镜像", "edl.backup")) return;
            var dialog = new SaveFileDialog { Title = "保存GPT备份", Filter = "GPT文件|*.bin;*.gpt" };
            if (dialog.ShowDialog() != true) return;
            if (!Services.MembershipService.Consume("edl.backup")) return;

            var sw = Stopwatch.StartNew();
            try
            {
                PageLog.Section(LogTarget, "备份 GPT");
                LogInfo("[可视刷写] 备份GPT...");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();

                var (s1, o1, e1) = await tools.ExecuteFastbootAsync("getvar all");
                var gptInfo = new StringBuilder();
                gptInfo.AppendLine("# GPT Backup from KunClient");
                gptInfo.AppendLine($"# Date: {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
                gptInfo.AppendLine("# Device Info:");
                if (!string.IsNullOrEmpty(o1)) gptInfo.AppendLine(o1);

                await File.WriteAllTextAsync(dialog.FileName, gptInfo.ToString());

                var (s2, o2, _) = await tools.ExecuteAdbAsync("shell su -c 'dd if=/dev/block/by-name/gpt of=/sdcard/gpt_backup.bin'");
                if (s2)
                {
                    await tools.ExecuteAdbAsync("pull /sdcard/gpt_backup.bin .");
                    LogSuccess("[可视刷写] GPT备份完成（含原始数据）");
                }
                else
                {
                    LogSuccess("[可视刷写] GPT信息已保存");
                }
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] GPT备份失败: {ex.Message}");
                PageLog.Footer(LogTarget, $"失败, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnRestoreGpt_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.EnsureAllowed("分区表（GPT）写入 / 修复", "edl.gpt")) return;
            var dialog = new OpenFileDialog { Title = "选择GPT备份", Filter = "GPT文件|*.bin;*.gpt" };
            if (dialog.ShowDialog() != true) return;

            // ⚠️ 计时器必须建在 try 外面：catch 里也要读它的 Elapsed 补收尾统计，
            //    建在 try 内会直接编译不过（CS0103: 当前上下文中不存在名称“sw”）。
            var sw = Stopwatch.StartNew();
            try
            {
                if (MessageBox.Show("恢复GPT可能导致设备无法启动，确定？", "确认",
                    MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes) return;

                if (!Services.MembershipService.Consume("edl.gpt")) return;
                PageLog.Section(LogTarget, "恢复 GPT");
                LogInfo("[可视刷写] 恢复GPT...");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();

                await tools.ExecuteAdbAsync($"push \"{dialog.FileName}\" /sdcard/gpt_restore.bin");
                var (success, _, error) = await tools.ExecuteAdbAsync("shell su -c 'dd if=/sdcard/gpt_restore.bin of=/dev/block/by-name/gpt'");
                if (success)
                {
                    LogSuccess("[可视刷写] GPT恢复完成");
                    ShowSuccess("GPT恢复完成，建议重启设备");
                    PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
                }
                else
                {
                    LogError($"[可视刷写] GPT恢复失败: {error}");
                    PageLog.Footer(LogTarget, $"失败, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
                }
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] GPT恢复错误: {ex.Message}");
                PageLog.Footer(LogTarget, $"失败, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnGenerateRawprogram_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new SaveFileDialog { Title = "保存rawprogram.xml", Filter = "XML文件|*.xml" };
            if (dialog.ShowDialog() != true) return;

            var sw = Stopwatch.StartNew();
            try
            {
                PageLog.Section(LogTarget, "生成 rawprogram.xml");
                LogInfo("[可视刷写] 生成rawprogram.xml...");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();

                var (success, output, _) = await tools.ExecuteFastbootAsync("getvar all");
                var xml = new StringBuilder();
                xml.AppendLine("<?xml version=\"1.0\" ?>");
                xml.AppendLine("<data>");
                xml.AppendLine("  <!-- Auto-generated by KunClient -->");

                if (success && !string.IsNullOrEmpty(output))
                {
                    var lines = output.Split('\n');
                    foreach (var line in lines)
                    {
                        var trimmed = line.Trim();
                        if (trimmed.StartsWith("partition:"))
                        {
                            var name = trimmed["partition:".Length..].Trim();
                            if (!string.IsNullOrEmpty(name))
                            {
                                var entry = "  <program SECTOR_SIZE_IN_BYTES=\"512\" file=\"" + name + ".img\" label=\"" + name + "\" num_partition_sectors=\"0\" physical_partition_number=\"0\" size_in_bytes=\"0\" sparse=\"false\" start_byte_hex=\"0x0\" start_sector=\"0\" />";
                                xml.AppendLine(entry);
                            }
                        }
                    }
                }

                xml.AppendLine("</data>");
                await File.WriteAllTextAsync(dialog.FileName, xml.ToString());
                LogSuccess($"[可视刷写] rawprogram.xml已生成: {dialog.FileName}");
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] 生成rawprogram失败: {ex.Message}");
                PageLog.Footer(LogTarget, $"失败, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void BtnBrowseImage_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "镜像文件|*.img;*.bin;*.mbn" };
            if (dialog.ShowDialog() == true)
            {
                TxtImageFile.Text = dialog.FileName;
            }
        }

        private async void BtnWriteSelected_Click(object sender, RoutedEventArgs e)
        {
            var image = TxtImageFile.Text?.Trim();
            var partition = TxtTargetPartition.Text?.Trim();
            if (string.IsNullOrEmpty(image) || string.IsNullOrEmpty(partition))
            {
                ShowError("请选择镜像和目标分区");
                return;
            }

            if (!Services.MembershipService.GuardAndConsume("写入指定分区", "edl.write")) return;
            // 刷写类操作：Section 会把外壳状态栏「任务」段设成「写入分区」，
            // 进度与收尾再回给外壳（ShellStatus），页面不再自己画底部进度条。
            var sw = Stopwatch.StartNew();
            var ok = false;
            try
            {
                PageLog.Section(LogTarget, "写入分区");
                LogInfo($"[可视刷写] 写入 {partition}...");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                // 🔴 工具加载接**真实字节进度**。原来这里是裸的 EnsureToolsExistAsync()：
                //    它内部一次性 GetStringAsync 读完整包，界面上这段时间进度条一直是 0%，
                //    成功后又瞬间跳 100% —— 看着像"卡住"，其实压根没有进度可报。
                if (!await tools.EnsureToolsExistAsync(ReportToolsProgress))
                {
                    LogError("[可视刷写] 平台工具不可用，已终止");
                    ShellStatus.Report(task: "加载平台工具失败");
                    return;
                }

                // fastboot 是子进程，**不吐字节级进度**。这时把进度条切到不确定态，
                // 而不是把它留在 0% —— 后者会被当成"卡死/假进度"。
                SetFlashUi(indeterminate: true, text: "正在刷写…（fastboot 不提供字节级进度）");
                var (success, output, error) = await tools.ExecuteFastbootAsync($"flash {partition} \"{image}\"");
                SetFlashUi(indeterminate: false);
                if (success)
                {
                    ok = true;
                    LogSuccess($"[可视刷写] 写入完成: {partition}");
                    ProgressBar.Value = 100;
                    TxtProgress.Text = "进度: 100%";
                    // 单分区写入没有逐字节进度可报，收尾时把「分区 + 100%」补给外壳
                    ShellStatus.Report(task: "写入分区", partition: partition, percent: 100, partitionPercent: 100);
                }
                else
                {
                    LogError($"[可视刷写] 写入失败: {error}");
                    ShellStatus.Report(task: "写入分区失败");
                }
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] 写入错误: {ex.Message}");
                ShellStatus.Report(task: "写入分区失败");
            }
            finally
            {
                // 收尾：外壳进度条停在 100%（任务名保留，用户还要看「刚才做了什么」）
                ShellStatus.EndTask();
                PageLog.Footer(LogTarget, $"{(ok ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void CtxReadPartition_Click(object sender, RoutedEventArgs e)
        {
            if (DgPartitions.SelectedItem is not VisualPartitionInfo part) return;

            if (!Services.MembershipService.EnsureAllowed("分区备份 / 导出镜像", "edl.backup")) return;
            var dialog = new SaveFileDialog { Title = "保存分区镜像", Filter = "镜像文件|*.img" };
            if (dialog.ShowDialog() != true) return;
            if (!Services.MembershipService.Consume("edl.backup")) return;

            try
            {
                // 右键菜单也是「一键式」操作：同样给区段标题 + 收尾统计
                var sw = Stopwatch.StartNew();
                PageLog.Section(LogTarget, "读取分区");
                LogInfo($"[可视刷写] 读取分区: {part.Name}");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var (success, _, error) = await tools.ExecuteFastbootAsync($"get_staged \"{dialog.FileName}\"");
                if (success) LogSuccess($"[可视刷写] 读取完成: {part.Name}");
                else LogError($"[可视刷写] 读取失败: {error}");
                PageLog.Footer(LogTarget, $"{(success ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] 读取错误: {ex.Message}");
            }
        }

        private async void CtxWritePartition_Click(object sender, RoutedEventArgs e)
        {
            if (DgPartitions.SelectedItem is not VisualPartitionInfo part) return;

            if (!Services.MembershipService.EnsureAllowed("写入指定分区", "edl.write")) return;
            var dialog = new OpenFileDialog { Title = "选择镜像文件", Filter = "镜像文件|*.img;*.bin" };
            if (dialog.ShowDialog() != true) return;
            if (!Services.MembershipService.Consume("edl.write")) return;

            try
            {
                // 写入类操作：区段标题同时把外壳「任务」段刷成「写入分区」，收尾再报一次结果
                var sw = Stopwatch.StartNew();
                PageLog.Section(LogTarget, "写入分区");
                LogInfo($"[可视刷写] 写入分区: {part.Name}");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var (success, output, error) = await tools.ExecuteFastbootAsync($"flash {part.Name} \"{dialog.FileName}\"");
                if (success)
                {
                    LogSuccess($"[可视刷写] 写入完成: {part.Name}");
                    ShellStatus.Report(task: "写入分区", partition: part.Name, percent: 100, partitionPercent: 100);
                }
                else
                {
                    LogError($"[可视刷写] 写入失败: {error}");
                    ShellStatus.Report(task: "写入分区失败");
                }
                PageLog.Footer(LogTarget, $"{(success ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] 写入错误: {ex.Message}");
                ShellStatus.Report(task: "写入分区失败");
            }
            finally
            {
                ShellStatus.EndTask();
            }
        }

        private async void CtxErasePartition_Click(object sender, RoutedEventArgs e)
        {
            if (DgPartitions.SelectedItem is not VisualPartitionInfo part) return;

            if (!Services.MembershipService.GuardAndConsume("擦除 / 格式化分区", "edl.erase")) return;
            try
            {
                var sw = Stopwatch.StartNew();
                PageLog.Section(LogTarget, "擦除分区");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var (success, _, error) = await tools.ExecuteFastbootAsync($"erase {part.Name}");
                if (success) LogSuccess($"[可视刷写] 擦除完成: {part.Name}");
                else LogError($"[可视刷写] 擦除失败: {error}");
                ShellStatus.Report(task: success ? "擦除分区" : "擦除分区失败", partition: part.Name);
                PageLog.Footer(LogTarget, $"{(success ? "完成" : "失败")}, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
            catch (Exception ex)
            {
                LogError($"[可视刷写] 擦除错误: {ex.Message}");
            }
        }

        private static string GetPartitionType(string name)
        {
            var lower = name.ToLowerInvariant();
            if (lower is "boot" or "init_boot" or "recovery" or "dtbo") return "Boot";
            if (lower is "system" or "vendor" or "product" or "system_ext" or "odm") return "System";
            if (lower is "modem" or "radio") return "Modem";
            if (lower is "userdata" or "metadata") return "Data";
            if (lower is "xbl" or "abl" or "tz" or "hyp" or "devcfg" or "rpm") return "Bootloader";
            if (lower is "gpt" or "gpt_backup0" or "gpt_backup1") return "GPT";
            return "Other";
        }

        /// <summary>getvar 的分区大小是字节数 —— 按人类可读单位显示（与 FastbootFlashPage 同一口径）。</summary>
        private static string FormatBytes(long bytes)
        {
            if (bytes <= 0) return "-";
            const double KB = 1024, MB = KB * 1024, GB = MB * 1024;
            return bytes >= GB ? $"{bytes / GB:F1} GB"
                 : bytes >= MB ? $"{bytes / MB:F1} MB"
                 : bytes >= KB ? $"{bytes / KB:F1} KB"
                 : $"{bytes} B";
        }
    }
}
