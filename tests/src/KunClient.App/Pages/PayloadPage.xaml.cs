using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using Microsoft.Win32;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public class PayloadPartitionInfo
    {
        public bool IsSelected { get; set; }
        public string Name { get; set; } = "";
        public string Size { get; set; } = "";
        public long Offset { get; set; }
        public long DataLength { get; set; }

        /// <summary>表格「#」列的序号（解析时按顺序填，从 1 开始）。</summary>
        public int Index { get; set; }

        /// <summary>
        /// 表格「偏移地址」列的显示文本：十六进制。
        /// 备用解析模式（ParseAsRawPayload）只有名称、没有偏移，这时显示 "-"（参考稿该列的占位）。
        /// </summary>
        public string OffsetText => Offset > 0 ? $"0x{Offset:X}" : "-";
    }

    public partial class PayloadPage : BasePage
    {
        private List<PayloadPartitionInfo> _partitions = new();
        private string _payloadPath = "";

        /// <summary>搜索过滤防抖：手停 220ms 才真正过滤（与 EDL 页同款）。</summary>
        private System.Windows.Threading.DispatcherTimer? _filterDebounce;

        private const uint MAGIC = 0x00444150; // "PAYLOAD" magic (big-endian)

        public PayloadPage()
        {
            InitializeComponent();
        }

        private void BtnBrowsePayload_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = "Payload文件|*.bin;payload.bin|所有文件|*.*" };
            if (dialog.ShowDialog() == true)
            {
                TxtPayloadFile.Text = dialog.FileName;
            }
        }

        /// <summary>选提取目录（参考稿「分区管理」顶行那个文件夹按钮）。</summary>
        private void BtnPickSaveDir_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFolderDialog { Title = "选择提取目录" };
            if (dialog.ShowDialog() != true) return;
            TxtSaveDir.Text = dialog.FolderName;
            LogInfo($"[Payload] 提取目录: {dialog.FolderName}");
        }

        // ─────────────────────────────────────────────────────────────
        //  解析（「解析固件」按钮与左侧「解析」按钮共用）
        // ─────────────────────────────────────────────────────────────

        private void BtnParsePayload_Click(object sender, RoutedEventArgs e) => ParsePayloadCore();

        /// <summary>
        /// 左侧「解析」：把「URL」框当作解析目标的来源。
        ///
        /// <para>
        /// 现有代码没有给 Payload 页接下载器（BackgroundDownloader 只服务于 EDL / 欧加两页），
        /// 所以这里不自己造一套下载：地址是本机路径就直接填进「本地」框，是 http(s) 就明确说明
        /// 要先下载到本地，两种都不成立时退回「本地」框里已经选好的 payload.bin ——
        /// 也就是原有的本地解析流程。
        /// </para>
        /// </summary>
        private void BtnParseUrl_Click(object sender, RoutedEventArgs e)
        {
            var url = TxtFirmwareUrl.Text?.Trim() ?? "";
            if (url.StartsWith("http://", StringComparison.OrdinalIgnoreCase) ||
                url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
            {
                PageLog.Section(TxtLog, "解析固件");
                var skipWatch = Stopwatch.StartNew();
                LogWarning("[Payload] 在线固件地址暂不支持直接下载，请先下载到本地，再用「本地」右侧的文件夹按钮选择 payload.bin");
                PageLog.Footer(TxtLog, $"已跳过, 耗时: {skipWatch.Elapsed.TotalSeconds:F2}秒");
                ShowError("暂不支持在线下载，请选择本地 payload.bin");
                return;
            }

            if (!string.IsNullOrEmpty(url) && File.Exists(url)) TxtPayloadFile.Text = url;
            ParsePayloadCore();
        }

        /// <summary>
        /// 解析入口：参考稿的日志三明治（Section 开头 / Footer 收尾）+ 外壳状态栏读数。
        /// 文件无效的提示放在 Section 之前 —— 没开始做的事不该在日志里开一段。
        /// </summary>
        private void ParsePayloadCore()
        {
            var file = TxtPayloadFile.Text?.Trim();
            if (string.IsNullOrEmpty(file) || !File.Exists(file))
            {
                ShowError("请选择有效的payload.bin文件");
                return;
            }

            PageLog.Section(TxtLog, "解析固件");
            // PageLog.Section 内部已经把外壳「任务」段设成同一个名字，这里再显式写一遍，
            // 保证任务名一定是「解析固件」（参考稿状态栏第一段就是这个）。
            ShellStatus.BeginTask("解析固件");
            var sw = Stopwatch.StartNew();
            try
            {
                ParsePayloadFile(file);
            }
            finally
            {
                PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
                ShellStatus.Report(percent: 100);
                ShellStatus.EndTask();
            }
        }

        /// <summary>解析 payload.bin 的正文（原 BtnParsePayload_Click 的实现，流程未改）。</summary>
        private void ParsePayloadFile(string file)
        {
            try
            {
                _payloadPath = file;
                _partitions.Clear();

                using var fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read);
                using var br = new BinaryReader(fs);

                // payload.bin format: 8-byte magic, 8-byte header_size, 8-byte entry_count
                var magic = br.ReadUInt32();
                if (magic != MAGIC)
                {
                    // Try big-endian
                    var bytes = BitConverter.GetBytes(magic);
                    Array.Reverse(bytes);
                    magic = BitConverter.ToUInt32(bytes, 0);
                }

                if (magic != MAGIC)
                {
                    // Fallback: try to detect as a different format
                    LogWarning("[Payload] 未识别的payload格式，尝试解析...");
                    ParseAsRawPayload(file);
                    return;
                }

                var headerSize = br.ReadInt64();
                var entryCount = br.ReadInt64();

                LogInfo($"[Payload] Magic: 0x{magic:X8}, HeaderSize: {headerSize}, 分区数: {entryCount}");

                // Read partition entries
                for (long i = 0; i < entryCount && fs.Position < headerSize; i++)
                {
                    var nameLen = br.ReadUInt32();
                    var nameBytes = br.ReadBytes((int)nameLen);
                    var name = Encoding.ASCII.GetString(nameBytes).TrimEnd('\0');

                    var size = br.ReadInt64();
                    var offset = br.ReadInt64();

                    // Align to 4 bytes
                    var pad = (4 - (fs.Position % 4)) % 4;
                    if (pad > 0) br.ReadBytes((int)pad);

                    _partitions.Add(new PayloadPartitionInfo
                    {
                        Index = _partitions.Count + 1,   // 「#」列：解析顺序，从 1 开始
                        Name = name,
                        Size = FormatSize(size),
                        Offset = offset,
                        DataLength = size
                    });
                }

                RefreshPartitionView();
                TxtInfo.Text = $"文件: {Path.GetFileName(file)}    分区数: {_partitions.Count}    总大小: {FormatSize(new FileInfo(file).Length)}";
                LogSuccess($"[Payload] 解析完成: {_partitions.Count} 个分区");
            }
            catch (Exception ex)
            {
                LogError($"[Payload] 解析失败: {ex.Message}");
                ParseAsRawPayload(file);
            }
        }

        private void ParseAsRawPayload(string file)
        {
            try
            {
                _partitions.Clear();
                using var fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read);

                // Try to find ASCII partition names in the binary
                var buffer = new byte[Math.Min(fs.Length, 1024 * 1024)];
                fs.ReadExactly(buffer, 0, buffer.Length);

                var knownParts = new[] { "boot", "system", "vendor", "product", "system_ext", "dtbo", "recovery", "init_boot", "modem" };
                foreach (var part in knownParts)
                {
                    var partBytes = Encoding.ASCII.GetBytes(part);
                    for (int i = 0; i < buffer.Length - partBytes.Length; i++)
                    {
                        bool match = true;
                        for (int j = 0; j < partBytes.Length; j++)
                        {
                            if (buffer[i + j] != partBytes[j]) { match = false; break; }
                        }
                        if (match)
                        {
                            _partitions.Add(new PayloadPartitionInfo
                            {
                                Index = _partitions.Count + 1,
                                Name = part,
                                Size = "未知",
                                Offset = i,
                                DataLength = 0
                            });
                            break;
                        }
                    }
                }

                if (_partitions.Count > 0)
                {
                    RefreshPartitionView();
                    TxtInfo.Text = $"文件: {Path.GetFileName(file)}    检测到 {_partitions.Count} 个分区名（格式不标准）";
                    LogWarning($"[Payload] 使用名称检测模式，找到 {_partitions.Count} 个分区");
                }
                else
                {
                    RefreshPartitionView();   // _partitions 已清空 → 表格也要跟着空，别留上一次的行
                    LogError("[Payload] 无法解析payload.bin格式");
                    TxtInfo.Text = "无法解析payload.bin格式";
                }
            }
            catch (Exception ex)
            {
                LogError($"[Payload] 备用解析失败: {ex.Message}");
            }
        }

        /// <summary>
        /// 让表格重新读一遍 _partitions。
        ///
        /// <para>
        /// ⚠️ _partitions 从头到尾是**同一个 List 实例**，而 List 不实现 INotifyCollectionChanged：
        /// 重复解析时把 ItemsSource 赋成同一个引用不会触发界面刷新（WPF 认为值没变），
        /// 所以内容改完必须显式 Refresh 一次视图（搜索过滤也是同一个视图，一起重算）。
        /// </para>
        /// </summary>
        private void RefreshPartitionView()
        {
            DgPartitions.ItemsSource = _partitions;
            CollectionViewSource.GetDefaultView(_partitions).Refresh();
        }

        /// <summary>
        /// 「搜索分区」：按分区名过滤表格（只过滤视图，全选 / 提取仍按 _partitions 全量算）。
        /// 🔴 过滤等手停 220ms 再做（与 EDL 页同款）：原来每敲一键就同步 Refresh 一次视图 ——
        ///    Refresh 会重建 CollectionView 并逐行重排，几百个分区的 payload.bin 时连打字都掉帧。
        /// </summary>
        private void TxtPartSearch_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (_filterDebounce == null)
            {
                _filterDebounce = new System.Windows.Threading.DispatcherTimer
                {
                    Interval = TimeSpan.FromMilliseconds(220)
                };
                _filterDebounce.Tick += (_, _) =>
                {
                    _filterDebounce!.Stop();
                    ApplyPartFilter();
                };
            }
            _filterDebounce.Stop();
            _filterDebounce.Start();
        }

        private void ApplyPartFilter()
        {
            var q = TxtPartSearch?.Text?.Trim() ?? "";
            var view = CollectionViewSource.GetDefaultView(_partitions);
            view.Filter = o => string.IsNullOrEmpty(q)
                || (o is PayloadPartitionInfo p && p.Name.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0);
            view.Refresh();
        }

        private void BtnSelectAll_Click(object sender, RoutedEventArgs e)
        {
            _partitions.ForEach(p => p.IsSelected = true);
            DgPartitions.Items.Refresh();
        }

        private void BtnDeselectAll_Click(object sender, RoutedEventArgs e)
        {
            _partitions.ForEach(p => p.IsSelected = false);
            DgPartitions.Items.Refresh();
        }

        /// <summary>提取目录：「保存路径」框里有就用它，没填才弹目录选择框，选完写回框里。</summary>
        private string? ResolveOutputDir()
        {
            var dir = TxtSaveDir.Text?.Trim() ?? "";
            if (!string.IsNullOrEmpty(dir)) return dir;

            var dialog = new OpenFolderDialog { Title = "选择提取目录" };
            if (dialog.ShowDialog() != true) return null;
            TxtSaveDir.Text = dialog.FolderName;
            return dialog.FolderName;
        }

        private async void BtnExtractSelected_Click(object sender, RoutedEventArgs e)
        {
            var selected = _partitions.FindAll(p => p.IsSelected);
            if (selected.Count == 0) { ShowError("请先选择要提取的分区"); return; }

            if (!Services.MembershipService.EnsureAllowed("Payload 分区提取", "payload.extract")) return;

            var outputDir = ResolveOutputDir();
            if (outputDir == null) return;

            if (!Services.MembershipService.Consume("payload.extract")) return;
            await ExtractPartitionsAsync(selected, outputDir);
        }

        private async void BtnExtractAll_Click(object sender, RoutedEventArgs e)
        {
            if (_partitions.Count == 0) { ShowError("请先解析payload.bin"); return; }

            // 「提取全部」与「提取选中」共用同一份额度：漏掉这里 = 用户用全部提取就能绕过额度
            if (!Services.MembershipService.EnsureAllowed("Payload 分区提取", "payload.extract")) return;

            var outputDir = ResolveOutputDir();
            if (outputDir == null) return;

            if (!Services.MembershipService.Consume("payload.extract")) return;
            await ExtractPartitionsAsync(_partitions, outputDir);
        }

        private async Task ExtractPartitionsAsync(List<PayloadPartitionInfo> partitions, string outputDir)
        {
            // 参考稿三明治：Section 开头 / Footer 收尾；进度同时喂页面细条与外壳双进度条
            PageLog.Section(TxtLog, "提取文件");
            ShellStatus.BeginTask("提取文件");
            var sw = Stopwatch.StartNew();
            try
            {
                LogInfo($"[Payload] 开始提取 {partitions.Count} 个分区...");
                Directory.CreateDirectory(outputDir);
                ProgressBar.Value = 0;
                TxtProgress.Text = $"准备提取 {partitions.Count} 个分区...";

                using var fs = new FileStream(_payloadPath, FileMode.Open, FileAccess.Read, FileShare.Read);

                for (int i = 0; i < partitions.Count; i++)
                {
                    var part = partitions[i];
                    LogInfo($"[Payload] 提取: {part.Name} ({part.Size})");
                    TxtProgress.Text = $"提取中 ({i + 1}/{partitions.Count}): {part.Name}";
                    ShellStatus.Report(partition: part.Name, totalSize: $"{i + 1} / {partitions.Count} 个分区");

                    if (part.DataLength > 0 && part.Offset > 0)
                    {
                        fs.Seek(part.Offset, SeekOrigin.Begin);
                        var outputPath = Path.Combine(outputDir, $"{part.Name}.img");
                        using var outFs = new FileStream(outputPath, FileMode.Create, FileAccess.Write);
                        
                        var remaining = part.DataLength;
                        var buffer = new byte[81920];
                        while (remaining > 0)
                        {
                            var toRead = (int)Math.Min(buffer.Length, remaining);
                            var read = await fs.ReadAsync(buffer, 0, toRead);
                            if (read == 0) break;
                            await outFs.WriteAsync(buffer, 0, read);
                            remaining -= read;
                        }
                        LogSuccess($"[Payload] 已提取: {outputPath}");
                    }
                    else
                    {
                        LogWarning($"[Payload] 跳过 {part.Name}（无偏移信息）");
                    }

                    ProgressBar.Value = (double)(i + 1) / partitions.Count * 100;
                    ShellStatus.Report(percent: ProgressBar.Value, partitionPercent: ProgressBar.Value);
                }

                LogSuccess($"[Payload] 提取完成: {outputDir}");
                TxtProgress.Text = $"提取完成: {partitions.Count} 个分区 → {outputDir}";
                ShowSuccess($"提取完成: {partitions.Count} 个分区");
            }
            catch (Exception ex)
            {
                LogError($"[Payload] 提取错误: {ex.Message}");
                TxtProgress.Text = $"提取失败: {ex.Message}";
            }
            finally
            {
                PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
                ShellStatus.Report(percent: 100);
                ShellStatus.EndTask();
            }
        }

        private static string FormatSize(long bytes)
        {
            if (bytes < 1024) return $"{bytes} B";
            if (bytes < 1024 * 1024) return $"{bytes / 1024.0:F1} KB";
            if (bytes < 1024 * 1024 * 1024) return $"{bytes / 1024.0 / 1024.0:F1} MB";
            return $"{bytes / 1024.0 / 1024.0 / 1024.0:F2} GB";
        }
    }
}
