using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.Win32;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.Core.Services;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public partial class AndroidToolsPage : BasePage
    {
        public AndroidToolsPage()
        {
            InitializeComponent();

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

        private void BtnBrowseSuper_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "Super镜像|*.img;*.bin" };
            if (dialog.ShowDialog() == true) TxtSuperInput.Text = dialog.FileName;
        }

        private async void BtnMergeSuper_Click(object sender, RoutedEventArgs e)
        {
            var file = TxtSuperInput.Text?.Trim();
            if (string.IsNullOrEmpty(file) || !File.Exists(file)) { ShowError("请选择Super镜像"); return; }

            if (!Services.MembershipService.EnsureAllowed("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（PageLog.Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, "合并 Super");
            try
            {
                LogInfo($"[工具] 合并Super: {Path.GetFileName(file)}");

                var folderDialog = new OpenFolderDialog { Title = "选择Super镜像所在文件夹" };
                if (folderDialog.ShowDialog() != true)
                {
                    ShowError("请选择Super镜像所在文件夹");
                    return;
                }

                if (!Services.MembershipService.Consume("android.tools")) return;
                var imagesDir = folderDialog.FolderName;
                var service = new SuperMergeService(
                    msg => LogInfo($"[Super] {msg}"),
                    progress => LogInfo($"[Super] 进度: {progress}%"));

                var jsonPath = service.FindSuperDefJson(imagesDir);
                if (jsonPath == null)
                {
                    ShowError("未找到super_def.*.json配置文件");
                    return;
                }

                var config = service.ParseSuperDefJson(jsonPath);
                if (config == null)
                {
                    ShowError("JSON配置解析失败");
                    return;
                }

                LogInfo($"[Super] 分区数: {config.Partitions.Count}");
                var success = await service.MergeSuperAsync(imagesDir, config, imagesDir);

                if (success)
                {
                    LogSuccess($"[工具] Super合并完成!");
                    ShowSuccess("Super合并完成!");
                }
                else
                {
                    LogError("[工具] Super合并失败");
                }
            }
            catch (Exception ex)
            {
                LogError($"[工具] Super合并错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行（成败都收尾，区段标题不会一直开着）
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void BtnBrowseOfp_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "OPLUS刷写包|*.ofp;*.ops" };
            if (dialog.ShowDialog() == true) TxtOfpInput.Text = dialog.FileName;
        }

        private async void BtnUnpackOfp_Click(object sender, RoutedEventArgs e)
        {
            var file = TxtOfpInput.Text?.Trim();
            if (string.IsNullOrEmpty(file) || !File.Exists(file)) { ShowError("请选择OFP文件"); return; }

            if (!Services.MembershipService.GuardAndConsume("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, "解包 OFP / OPS");
            try
            {
                LogInfo($"[工具] 解包OFP: {Path.GetFileName(file)}");
                var ext = Path.GetExtension(file).ToLowerInvariant();

                if (ext == ".ops")
                {
                    LogInfo("[工具] OPS文件，使用OpsDecryptor...");
                    var opsService = new OpsDecryptor(msg => LogInfo($"[OPS] {msg}"));
                    var result = await Task.Run(() => opsService.Decrypt(file));
                    if (result != null)
                    {
                        LogSuccess($"[工具] OPS解包完成: {result}");
                        ShowSuccess($"解包完成: {result}");
                    }
                    else
                    {
                        LogError("[工具] OPS解包失败，密钥不匹配");
                    }
                }
                else if (ext == ".ofp")
                {
                    LogInfo("[工具] OFP文件，使用OfpDecryptor...");
                    var ofpService = new OfpDecryptor(msg => LogInfo($"[OFP] {msg}"));
                    var result = await Task.Run(() => ofpService.Decrypt(file));
                    if (result != null)
                    {
                        LogSuccess($"[工具] OFP解包完成: {result}");
                        ShowSuccess($"解包完成: {result}");
                    }
                    else
                    {
                        LogError("[工具] OFP解包失败，密钥不匹配或格式未知");
                    }
                }
                else
                {
                    ShowError($"不支持的文件格式: {ext}");
                }
            }
            catch (Exception ex)
            {
                LogError($"[工具] 解包错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnUnpackOps_Click(object sender, RoutedEventArgs e)
        {
            var file = TxtOfpInput.Text?.Trim();
            if (string.IsNullOrEmpty(file) || !File.Exists(file)) { ShowError("请选择OPS文件"); return; }

            if (!Services.MembershipService.GuardAndConsume("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, "解包 OPS");
            try
            {
                LogInfo($"[工具] 解包OPS: {Path.GetFileName(file)}");
                var opsService = new OpsDecryptor(msg => LogInfo($"[OPS] {msg}"));
                var result = await Task.Run(() => opsService.Decrypt(file));
                if (result != null)
                {
                    LogSuccess($"[工具] OPS解包完成: {result}");
                    ShowSuccess($"解包完成: {result}");
                }
                else
                {
                    LogError("[工具] OPS解包失败，密钥不匹配");
                }
            }
            catch (Exception ex)
            {
                LogError($"[工具] OPS解包错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void BtnBrowsePayload_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "Payload文件|*.bin;payload.bin" };
            if (dialog.ShowDialog() == true) TxtPayloadInput.Text = dialog.FileName;
        }

        private async void BtnConvertPayload_Click(object sender, RoutedEventArgs e)
        {
            var file = TxtPayloadInput.Text?.Trim();
            if (string.IsNullOrEmpty(file) || !File.Exists(file)) { ShowError("请选择payload.bin"); return; }

            if (!Services.MembershipService.GuardAndConsume("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, "Payload 转 Fastboot");
            try
            {
                LogInfo($"[工具] Payload转Fastboot: {Path.GetFileName(file)}");
                var outputDir = Path.Combine(Path.GetDirectoryName(file) ?? "", "fastboot_scripts");
                Directory.CreateDirectory(outputDir);

                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();

                // Use payload_dumper if available, otherwise generate flash script
                var (success, output, error) = await tools.ExecuteAdbAsync($"shell payload_dumper --out \"{outputDir}\" \"{file}\"");
                if (success && !string.IsNullOrEmpty(output))
                {
                    LogSuccess($"[工具] Payload转换完成: {outputDir}");
                }
                else
                {
                    // Generate a flash_all.sh script
                    var scriptPath = Path.Combine(outputDir, "flash_all.sh");
                    var sb = new StringBuilder();
                    sb.AppendLine("#!/bin/bash");
                    sb.AppendLine("# Auto-generated flash script from payload.bin");
                    sb.AppendLine("set -e");
                    sb.AppendLine("");

                    using var fs = new FileStream(file, FileMode.Open, FileAccess.Read);
                    var buffer = new byte[4096];
                    fs.ReadExactly(buffer, 0, buffer.Length);
                    var content = Encoding.ASCII.GetString(buffer);

                    var knownParts = new[] { "boot", "system", "vendor", "product", "system_ext", "dtbo", "recovery", "init_boot", "modem" };
                    foreach (var part in knownParts)
                    {
                        if (content.Contains(part))
                        {
                            sb.AppendLine($"fastboot flash {part} {part}.img");
                        }
                    }
                    sb.AppendLine("fastboot reboot");

                    await File.WriteAllTextAsync(scriptPath, sb.ToString());
                    LogSuccess($"[工具] 已生成刷写脚本: {scriptPath}");
                }
            }
            catch (Exception ex)
            {
                LogError($"[工具] Payload转换错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private void BtnBrowseTwrpImage_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "镜像文件|*.img" };
            if (dialog.ShowDialog() == true)
            {
                TxtTwrpPartition.Text = Path.GetFileNameWithoutExtension(dialog.FileName);
                LogInfo($"[TWRP] 选择镜像: {dialog.FileName}");
            }
        }

        private async void BtnTwrpRead_Click(object sender, RoutedEventArgs e)
        {
            var part = TxtTwrpPartition.Text?.Trim();
            if (string.IsNullOrEmpty(part)) { ShowError("请输入分区名"); return; }

            if (!Services.MembershipService.GuardAndConsume("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, $"读取分区 (TWRP): {part}");
            try
            {
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var (success, output, error) = await tools.ExecuteAdbAsync($"shell su -c 'dd if=/dev/block/by-name/{part} of=/sdcard/{part}.img'");
                if (success)
                {
                    await tools.ExecuteAdbAsync($"pull /sdcard/{part}.img .");
                    LogSuccess($"[TWRP] 读取完成: {part}.img");
                }
                else
                {
                    LogError($"[TWRP] 读取失败: {error}");
                }
            }
            catch (Exception ex)
            {
                LogError($"[TWRP] 读取错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnTwrpWrite_Click(object sender, RoutedEventArgs e)
        {
            var part = TxtTwrpPartition.Text?.Trim();
            if (string.IsNullOrEmpty(part)) { ShowError("请输入分区名"); return; }

            var dialog = new OpenFileDialog { Filter = "镜像文件|*.img" };
            if (dialog.ShowDialog() != true) return;

            if (!Services.MembershipService.GuardAndConsume("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, $"写入分区 (TWRP): {part}");
            try
            {
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                LogInfo($"[TWRP] 推送 {Path.GetFileName(dialog.FileName)}...");
                await tools.ExecuteAdbAsync($"push \"{dialog.FileName}\" /sdcard/{part}.img");
                LogInfo($"[TWRP] 写入分区 {part}...");
                var (success, _, error) = await tools.ExecuteAdbAsync($"shell su -c 'dd if=/sdcard/{part}.img of=/dev/block/by-name/{part}'");
                if (success)
                {
                    LogSuccess($"[TWRP] 写入完成: {part}");
                }
                else
                {
                    LogError($"[TWRP] 写入失败: {error}");
                }
            }
            catch (Exception ex)
            {
                LogError($"[TWRP] 写入错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnTwrpErase_Click(object sender, RoutedEventArgs e)
        {
            var part = TxtTwrpPartition.Text?.Trim();
            if (string.IsNullOrEmpty(part)) { ShowError("请输入分区名"); return; }

            if (!Services.MembershipService.GuardAndConsume("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, $"擦除分区 (TWRP): {part}");
            try
            {
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();
                var (success, _, error) = await tools.ExecuteAdbAsync($"shell su -c 'dd if=/dev/zero of=/dev/block/by-name/{part} bs=1 count=1'");
                if (success) LogSuccess($"[TWRP] 擦除完成: {part}");
                else LogError($"[TWRP] 擦除失败: {error}");
            }
            catch (Exception ex)
            {
                LogError($"[TWRP] 擦除错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }

        private async void BtnGenerateOcdt_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.EnsureAllowed("Android 工具", "android.tools")) return;

            // 日志「三明治」上半：区段标题（Section 同时把外壳状态栏的「任务」段刷成该名字）
            var sw = Stopwatch.StartNew();
            PageLog.Section(LogTarget, "生成 OCDT");
            try
            {
                LogInfo("[工具] 生成OCDT...");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                await tools.EnsureToolsExistAsync();

                // Read device info to generate OCDT
                var (success, output, error) = await tools.ExecuteAdbAsync("shell cat /proc/device-tree/model");
                if (success && !string.IsNullOrEmpty(output))
                {
                    var model = output.Trim();
                    var ocdtContent = $"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<DeviceInfo>\n  <Model>{model}</Model>\n  <Tool>KunClient</Tool>\n</DeviceInfo>";
                    var dialog = new SaveFileDialog { Title = "保存OCDT", Filter = "OCDT文件|*.xml" };
                    if (dialog.ShowDialog() == true)
                    {
                        if (!Services.MembershipService.Consume("android.tools")) return;
                        await File.WriteAllTextAsync(dialog.FileName, ocdtContent);
                        LogSuccess($"[工具] OCDT已生成: {dialog.FileName}");
                    }
                }
                else
                {
                    LogError($"[工具] 获取设备信息失败: {error}");
                }
            }
            catch (Exception ex)
            {
                LogError($"[工具] OCDT生成错误: {ex.Message}");
            }
            finally
            {
                // 日志「三明治」下半：右对齐收尾行
                PageLog.Footer(LogTarget, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            }
        }
    }
}
