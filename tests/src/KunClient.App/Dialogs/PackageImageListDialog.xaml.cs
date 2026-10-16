using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using KunFlashTool.Core.Protocol.Firehose;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// 「包内镜像清单」：把资料目录里所有 rawprogram*.xml 的 &lt;program&gt; 条目逐个列出来。
    ///
    /// 为什么需要它：主界面的分区表来自**设备** GPT，而 QFIL 等工具展示的是**包内**条目。
    /// 出厂包会把 super / userdata / metadata 拆成多块镜像（super_1..6、userdata_1..10，
    /// 且它们共用同一个 label），只按"分区名"看就会以为解析不全 / 认不出这些块。
    /// 这里逐条列出：分区名 / 第几块 / LUN / 起始扇区（表达式原样保留）/ 扇区数 / 镜像 / 状态。
    ///
    /// 纯只读展示，不改变任何刷写行为；「占位」条目在 rawprogram 里没有镜像文件，
    /// 按 QFIL 语义只是登记分区（不下发数据、也不擦除），清单里照样列出来只是为了对齐原包。
    /// </summary>
    public partial class PackageImageListDialog : Window
    {
        /// <summary>清单中的一行（对应一个 &lt;program&gt; 条目）。</summary>
        public sealed class ImageRow
        {
            public string Label { get; init; } = "";
            public string ChunkText { get; init; } = "";
            public uint Lun { get; init; }
            public string LunText => "LUN" + Lun;
            public string StartSectorText { get; init; } = "";
            public string NumSectorsText { get; init; } = "";
            public string FileText { get; init; } = "";
            public string StatusText { get; init; } = "";

            /// <summary>这次刷写是否会真正下发数据（供「只看会刷写的条目」过滤）。</summary>
            public bool WillFlash { get; init; }
        }

        private readonly List<ImageRow> _all = new();
        private readonly ObservableCollection<ImageRow> _view = new();

        /// <summary>空构造：离屏视图巡检会直接构造本窗口（无数据时列表为空）。</summary>
        public PackageImageListDialog()
        {
            InitializeComponent();
            DgImages.ItemsSource = _view;
            TxtSummary.Text = "尚未指定资料目录。";
        }

        /// <summary>打开清单。<paramref name="excludedLabels"/> 为本次「保数据刷写」排除的分区。</summary>
        public static void Pick(Window? owner, string imageDir, IEnumerable<string>? excludedLabels)
        {
            var dialog = new PackageImageListDialog();
            if (owner != null) dialog.Owner = owner;
            dialog.LoadPackage(imageDir, excludedLabels);
            dialog.ShowDialog();
        }

        private void LoadPackage(string imageDir, IEnumerable<string>? excludedLabels)
        {
            var excluded = excludedLabels?.ToHashSet(StringComparer.OrdinalIgnoreCase)
                           ?? new HashSet<string>(StringComparer.OrdinalIgnoreCase);

            if (string.IsNullOrWhiteSpace(imageDir) || !Directory.Exists(imageDir))
            {
                SetEmpty("资料目录不存在，请先在主界面选择正确的固件 / 镜像目录。");
                return;
            }

            var xmlFiles = Directory.GetFiles(imageDir, "rawprogram*.xml")
                .OrderBy(f => f, StringComparer.OrdinalIgnoreCase)
                .ToList();
            if (xmlFiles.Count == 0)
            {
                SetEmpty("资料目录内没有 rawprogram XML。这种包只能按镜像文件名匹配分区刷写，没有条目清单可列。");
                return;
            }

            var entries = new List<RawProgramEntry>();
            var badXml = new List<string>();
            foreach (var xml in xmlFiles)
            {
                try
                {
                    entries.AddRange(QfilXmlParser.ParseRawProgram(xml));
                }
                catch (Exception ex)
                {
                    badXml.Add($"{Path.GetFileName(xml)}（{ex.Message}）");
                }
            }

            if (entries.Count == 0)
            {
                SetEmpty("rawprogram XML 里没有解析出任何 <program> 条目。" +
                         (badXml.Count > 0 ? "\n解析失败: " + string.Join("；", badXml) : ""));
                return;
            }

            // 多块镜像：同一 (label, LUN) 出现多次即为一组，按包内顺序编号 #i/n。
            // 注意键必须落到**每个条目自己的下标**上 —— 用「分组」当键会把同一组的
            // 六行反复覆盖成最后一个块号（曾经 super 六行全显示 #6/6）。
            var chunkByIndex = new Dictionary<int, (int Index, int Total)>();
            var chunkTotals = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            foreach (var g in entries
                         .Select((e, i) => (Entry: e, Index: i))
                         .GroupBy(x => (Label: (x.Entry.Label ?? "").ToLowerInvariant(),
                                        Lun: x.Entry.PhysicalPartitionNumber)))
            {
                var ordered = g.OrderBy(x => x.Index).ToList();
                if (ordered.Count <= 1) continue;

                for (var i = 0; i < ordered.Count; i++)
                    chunkByIndex[ordered[i].Index] = (i + 1, ordered.Count);

                chunkTotals[g.Key.Label] = Math.Max(
                    chunkTotals.TryGetValue(g.Key.Label, out var prev) ? prev : 0, ordered.Count);
            }

            var willFlash = 0;
            var placeholder = 0;
            var missing = 0;
            var keptData = 0;

            for (var i = 0; i < entries.Count; i++)
            {
                var e = entries[i];
                var label = string.IsNullOrWhiteSpace(e.Label)
                    ? Path.GetFileNameWithoutExtension(e.FileName ?? "")
                    : e.Label!.Trim();
                var fileName = e.FileName ?? "";
                var isEmptyFile = string.IsNullOrWhiteSpace(fileName);

                string status;
                bool will;
                if (isEmptyFile)
                {
                    status = "占位（不刷）";
                    will = false;
                    placeholder++;
                }
                else
                {
                    var exists = File.Exists(Path.Combine(imageDir, fileName));
                    if (!exists)
                    {
                        status = "镜像缺失";
                        will = false;
                        missing++;
                    }
                    else if (excluded.Contains(label))
                    {
                        status = "保数据跳过";
                        will = false;
                        keptData++;
                    }
                    else
                    {
                        status = "会刷写";
                        will = true;
                        willFlash++;
                    }
                }

                var chunkText = chunkByIndex.TryGetValue(i, out var chunk)
                    ? $"#{chunk.Index}/{chunk.Total}"
                    : "";

                _all.Add(new ImageRow
                {
                    Label = label,
                    ChunkText = chunkText,
                    Lun = e.PhysicalPartitionNumber,
                    // start_sector 原样显示：表达式（NUM_DISK_SECTORS-5.）能让人看出
                    // 这条写的是分区表结构位置，而不是普通数据。
                    StartSectorText = string.IsNullOrWhiteSpace(e.StartSector) ? "" : e.StartSector!,
                    NumSectorsText = e.NumPartitionSectors > 0 ? e.NumPartitionSectors.ToString() : "",
                    FileText = FileText(imageDir, fileName),
                    StatusText = status,
                    WillFlash = will
                });
            }

            // 按 LUN 归拢（LUN0 在前的读法与 QFIL 一致）。必须用 OrderBy —— List.Sort 是
            // 不稳定排序，同 LUN 内会把 super_1..6 / userdata_1..10 的块序打乱。
            var byLun = _all.OrderBy(r => r.Lun).ToList();
            _all.Clear();
            _all.AddRange(byLun);
            var luns = _all.Select(r => r.Lun).Distinct().OrderBy(l => l).ToList();
            var multi = chunkTotals.Count == 0
                ? ""
                : " · 多块镜像分组: " + string.Join("、", chunkTotals
                    .OrderBy(kv => kv.Key)
                    .Select(kv => $"{kv.Key} ×{kv.Value}"));

            var sectorSizes = entries.Select(e => e.SectorSizeInBytes).Where(s => s > 0).Distinct().ToList();
            var summary =
                $"共 {entries.Count} 条 <program>（{xmlFiles.Count} 个 rawprogram，LUN{string.Join("、LUN", luns)}）· " +
                $"会刷写 {willFlash} · 占位 {placeholder} · 保数据跳过 {keptData} · 镜像缺失 {missing}" +
                (sectorSizes.Count > 0 ? $" · 包内扇区 {string.Join("/", sectorSizes)} B" : "") +
                multi;
            TxtSummary.Text = summary;

            if (badXml.Count > 0)
                TxtSummary.Text += "\n解析失败: " + string.Join("；", badXml);
            if (missing > 0)
                TxtSummary.Text += "\n有镜像文件缺失的条目不会被刷写，请检查资料目录是否解压完整。";

            ApplyFilter();
        }

        private static string FileText(string imageDir, string fileName)
        {
            if (string.IsNullOrWhiteSpace(fileName)) return "（无）";
            try
            {
                var full = Path.Combine(imageDir, fileName);
                if (!File.Exists(full)) return fileName;
                return $"{fileName}  ({LoaderPickerDialog.FormatSize(new FileInfo(full).Length)})";
            }
            catch
            {
                return fileName;
            }
        }

        private void SetEmpty(string text)
        {
            _all.Clear();
            _view.Clear();
            TxtEmpty.Text = text;
            TxtEmpty.Visibility = Visibility.Visible;
            TxtSummary.Text = text;
        }

        private void ApplyFilter()
        {
            var onlyFlash = ChkOnlyFlash.IsChecked == true;
            var rows = onlyFlash ? _all.Where(r => r.WillFlash).ToList() : _all;

            // 🔴 填表期间先摘开 ItemsSource（绑定状态下逐条 Add = 逐行插容器 + 逐次重排）；
            //    整轮填完挂回去只发一次 Reset、只排一次版。
            DgImages.ItemsSource = null;
            _view.Clear();
            foreach (var row in rows) _view.Add(row);
            DgImages.ItemsSource = _view;

            TxtEmpty.Text = "没有符合条件的条目。";
            TxtEmpty.Visibility = _view.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        }

        private void ChkOnlyFlash_Changed(object sender, RoutedEventArgs e) => ApplyFilter();

        private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ButtonState == MouseButtonState.Pressed) DragMove();
        }
    }
}
