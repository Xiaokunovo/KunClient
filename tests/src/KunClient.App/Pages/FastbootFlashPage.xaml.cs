using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Media;
using Microsoft.Win32;
using KunFlashTool.Core.PlatformTools;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    /// <summary>
    /// 「分区管理」表格的一行。
    ///
    /// <para>
    /// 工具箱版式把原来 7 行「标签 + 输入框 + 选择按钮」换成了表格，所以这里需要一个行模型；
    /// <see cref="Key"/> 是稳定键（boot / init_boot / … / custom），
    /// 靠它把表格行和代码后置里那 7 个镜像框（TxtBootImage …）对上 ——
    /// **镜像框仍然是刷写流程唯一的数据源**，表格只是它的展示位，两者由代码后置同步。
    /// </para>
    /// </summary>
    public sealed class FastbootPartitionRow : INotifyPropertyChanged
    {
        public FastbootPartitionRow(string key, string name, string type)
        {
            Key = key;
            _name = name;
            Type = type;
        }

        /// <summary>稳定键：boot / init_boot / vendor_boot / dtbo / recovery / system / custom。</summary>
        public string Key { get; }

        /// <summary>类型列：这些都是原始镜像分区，统一 raw（参考稿该列就写 raw）。</summary>
        public string Type { get; }

        private string _name;
        /// <summary>分区名（自定义分区跟随 TxtCustomPartition 输入框）。</summary>
        public string Name
        {
            get => _name;
            set { if (_name == value) return; _name = value; Raise(); }
        }

        private bool _isSelected;
        /// <summary>勾选框：勾上的行才会参与「写入分区 / 擦除分区」。</summary>
        public bool IsSelected
        {
            get => _isSelected;
            set { if (_isSelected == value) return; _isSelected = value; Raise(); }
        }

        private string _size = "-";
        /// <summary>大小：由「读分区表」（fastboot getvar all 的 partition-size）填，读不到就是 "-"。</summary>
        public string Size
        {
            get => _size;
            set { if (_size == value) return; _size = value; Raise(); }
        }

        private string _filePath = "";
        /// <summary>镜像文件路径（双击行选择）。</summary>
        public string FilePath
        {
            get => _filePath;
            set
            {
                if (_filePath == value) return;
                _filePath = value;
                Raise();
                Raise(nameof(FilePathDisplay));
                Raise(nameof(HasFile));
            }
        }

        /// <summary>「文件路径」列的显示文本：没选文件时是提示文案。</summary>
        public string FilePathDisplay => HasFile ? FilePath : "双击选择文件";

        /// <summary>表格用 DataTrigger 绑它：没选文件 → 灰色提示（颜色走 Theme.TextTertiary）。</summary>
        public bool HasFile => !string.IsNullOrEmpty(FilePath);

        public event PropertyChangedEventHandler? PropertyChanged;

        private void Raise([CallerMemberName] string? name = null)
            => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name ?? ""));
    }

    public partial class FastbootFlashPage : BasePage
    {
        private string _customImagePath = "";

        /// <summary>「分区管理」表格的行集合（顺序 = 固定分区顺序 + 自定义分区）。</summary>
        private readonly List<FastbootPartitionRow> _rows = new();

        public FastbootFlashPage()
        {
            InitializeComponent();
            BuildPartitionRows();
            // 2026-10-03：「可视刷写」从独立页签合并成页内的一个模式。
            // 🔴 **不在构造函数里 new**：WPF 里 `Visibility=Collapsed` 的元素**照样会触发 Loaded**，
            //    而 VisualFlashPage.Loaded 会去订阅 ShellStatus.Changed / DeviceStateHub.Changed。
            //    在构造期就创建 = 用户一直待在「分区管理」模式时，这个看不见的页签仍在
            //    跟着每次状态变化做无用功。改成首次切过去时才创建（见 EnsureVisualCreated）。
            ResetFbTab(TabVisual, TabVisualText);
            ActivateFbTab(TabPartition, TabPartitionText);
            ColFbLeft.Width = new GridLength(316);
            ColFbGap.Width = new GridLength(8);
            PanelFbVisual.Visibility = Visibility.Collapsed;
            ColLeftPanel.Visibility = Visibility.Visible;
            ColRightPanel.Visibility = Visibility.Visible;
        }

        /// <summary>
        /// 可视刷写页实例：**首次切换时创建一次，之后一直留着**。
        /// 留着的理由：刷机进行中来回切模式不能把会话对象扔掉，否则用户切回来会看到「未连接」
        /// 而后台还在写分区。不每次新建的理由同上（构造函数会订阅状态变化）。
        /// </summary>
        private VisualFlashPage? _visual;

        private void EnsureVisualCreated()
        {
            if (_visual != null) return;
            _visual = new VisualFlashPage();
            PanelFbVisual.Content = _visual;
        }

        // ═══════════════ 模式切换：分区管理 / 可视刷写（整宽接管） ═══════════════

        private void TabPartition_Click(object sender, MouseButtonEventArgs e) => _ = FbMode_Partition();
        private void TabVisual_Click(object sender, MouseButtonEventArgs e) => _ = FbMode_Visual();

        /// <summary>左右列宽的现值（补间的起点；构造期可能还没测量，退回 XAML 里的 316/8）。</summary>
        private double _fbLeftW = 316, _fbGapW = 8;

        private async Task FbMode_Partition()
        {
            if (PanelFbVisual.Visibility == Visibility.Visible && ColFbLeft.Width.Value < 1) return;

            // 先淡出当前内容，再收列宽 —— 顺序反了会出现「列已经空了、内容还在淡出」的错位感
            await Motion.ConcealAsync(PanelFbVisual, 120, -6, collapseAfter: false);
            await Motion.ConcealAsync(ColRightPanel, 120, -6, collapseAfter: false);

            PanelFbVisual.Visibility = Visibility.Collapsed;
            ColLeftPanel.Visibility = Visibility.Visible;
            ColRightPanel.Visibility = Visibility.Visible;

            // 列宽补间：316/8 ↔ 0。AnimateGridLengthAsync 收尾会摘动画并落终值
            await Task.WhenAll(
                Motion.AnimateGridLengthAsync(ColFbLeft, ColumnDefinition.WidthProperty, _fbLeftW, 316, GridUnitType.Pixel, 200, Motion.SilkEase),
                Motion.AnimateGridLengthAsync(ColFbGap, ColumnDefinition.WidthProperty, _fbGapW, 8, GridUnitType.Pixel, 200, Motion.SilkEase));
            _fbLeftW = 316;
            _fbGapW = 8;

            _ = Motion.RevealAsync(ColRightPanel, 220, -8);
            ResetFbTab(TabVisual, TabVisualText);
            ActivateFbTab(TabPartition, TabPartitionText);
        }

        /// <summary>
        /// 可视刷写**整宽**接管：它自带左栏（设备信息 + 日志），若与本页左列并存会变成 4 列。
        /// 所以这里把左列与间隔列收成 0 并隐藏本页左右两列，让可视刷写跨第 2~3 列铺满。
        /// </summary>
        private async Task FbMode_Visual()
        {
            if (PanelFbVisual.Visibility == Visibility.Visible) return;

            EnsureVisualCreated();

            await Motion.ConcealAsync(ColLeftPanel, 120, -6, collapseAfter: false);
            await Motion.ConcealAsync(ColRightPanel, 120, -6, collapseAfter: false);

            ColLeftPanel.Visibility = Visibility.Collapsed;
            ColRightPanel.Visibility = Visibility.Collapsed;

            // 读当前实际宽度作为起点（用户可能中途拖过窗口，别用写死的 316）
            _fbLeftW = ColFbLeft.Width.Value > 0 ? ColFbLeft.Width.Value : 316;
            _fbGapW = ColFbGap.Width.Value > 0 ? ColFbGap.Width.Value : 8;
            await Task.WhenAll(
                Motion.AnimateGridLengthAsync(ColFbLeft, ColumnDefinition.WidthProperty, _fbLeftW, 0, GridUnitType.Pixel, 200, Motion.SilkEase),
                Motion.AnimateGridLengthAsync(ColFbGap, ColumnDefinition.WidthProperty, _fbGapW, 0, GridUnitType.Pixel, 200, Motion.SilkEase));
            _fbLeftW = 0;
            _fbGapW = 0;

            PanelFbVisual.Visibility = Visibility.Visible;
            if (PanelFbVisual.Content is FrameworkElement ve) _ = Motion.RevealAsync(ve, 240, -8);
            ResetFbTab(TabPartition, TabPartitionText);
            ActivateFbTab(TabVisual, TabVisualText);
        }

        private static void ActivateFbTab(Border tab, TextBlock text)
        {
            tab.BorderBrush = (Brush)Application.Current.FindResource("Theme.Accent");
            text.Foreground = (Brush)Application.Current.FindResource("Theme.TextPrimary");
            text.FontWeight = FontWeights.Bold;
        }

        private static void ResetFbTab(Border tab, TextBlock text)
        {
            tab.BorderBrush = Brushes.Transparent;
            text.Foreground = (Brush)Application.Current.FindResource("Theme.TextTertiary");
            text.FontWeight = FontWeights.SemiBold;
        }

        /// <summary>建表：7 行对应原有 7 个镜像框，之后不再增删（表格只反映它们的状态）。</summary>
        private void BuildPartitionRows()
        {
            _rows.Add(new FastbootPartitionRow("boot", "boot", "raw"));
            _rows.Add(new FastbootPartitionRow("init_boot", "init_boot", "raw"));
            _rows.Add(new FastbootPartitionRow("vendor_boot", "vendor_boot", "raw"));
            _rows.Add(new FastbootPartitionRow("dtbo", "dtbo", "raw"));
            _rows.Add(new FastbootPartitionRow("recovery", "recovery", "raw"));
            _rows.Add(new FastbootPartitionRow("system", "system", "raw"));
            _rows.Add(new FastbootPartitionRow("custom", "自定义分区", "raw"));
            DgFastbootPartitions.ItemsSource = _rows;
        }

        /// <summary>行键 → 镜像框（唯一的映射表，选文件 / 同步显示都走它）。</summary>
        private TextBox? ImageBoxFor(string key) => key switch
        {
            "boot" => TxtBootImage,
            "init_boot" => TxtInitBootImage,
            "vendor_boot" => TxtVendorBootImage,
            "dtbo" => TxtDtboImage,
            "recovery" => TxtRecoveryImage,
            "system" => TxtSystemImage,
            "custom" => TxtCustomPartition,
            _ => null
        };

        private string BrowseImage()
        {
            var dialog = new OpenFileDialog { Filter = "镜像文件|*.img;*.bin;*.mbn;*.zip" };
            return dialog.ShowDialog() == true ? dialog.FileName : "";
        }

        // ─────────────────────────────────────────────────────────────
        //  选镜像文件（表格双击 / 原有 7 个「选择」按钮共用同一条路径）
        // ─────────────────────────────────────────────────────────────

        /// <summary>
        /// 给某个分区选镜像。原来 7 个 BtnBrowse*_Click 各写一遍的选文件逻辑集中到这里，
        /// 表格「双击选择文件」和按钮走的是同一套，避免两份流程各选各的。
        /// </summary>
        private void PickImageFor(string key)
        {
            var row = _rows.Find(r => r.Key == key);
            if (row == null) return;

            var p = BrowseImage();
            if (string.IsNullOrEmpty(p)) return;

            if (key == "custom")
            {
                // 自定义分区的镜像路径沿用原有的 _customImagePath（分区名仍由 TxtCustomPartition 提供）
                _customImagePath = p;
            }
            else
            {
                var box = ImageBoxFor(key);
                if (box != null) box.Text = p;
            }

            row.FilePath = p;
            LogInfo($"[Fastboot] 已选择 {row.Name} 镜像: {p}");
        }

        private void BtnBrowseBoot_Click(object sender, RoutedEventArgs e) => PickImageFor("boot");
        private void BtnBrowseInitBoot_Click(object sender, RoutedEventArgs e) => PickImageFor("init_boot");
        private void BtnBrowseVendorBoot_Click(object sender, RoutedEventArgs e) => PickImageFor("vendor_boot");
        private void BtnBrowseDtbo_Click(object sender, RoutedEventArgs e) => PickImageFor("dtbo");
        private void BtnBrowseRecovery_Click(object sender, RoutedEventArgs e) => PickImageFor("recovery");
        private void BtnBrowseSystem_Click(object sender, RoutedEventArgs e) => PickImageFor("system");
        private void BtnBrowseCustom_Click(object sender, RoutedEventArgs e) => PickImageFor("custom");

        /// <summary>
        /// 勾选分区：这一行还没有镜像文件时，顺手弹出选文件对话框（勾了就是要刷它，先得有文件）；
        /// 用户取消就退回未勾选 —— 否则「已勾选但没文件」的行会在写入时报错，用户还得自己猜原因。
        /// 已经有文件的行只切换勾选，批量重刷不必重复选文件。
        /// </summary>
        private void ChkPartition_Click(object sender, RoutedEventArgs e)
        {
            if (sender is not CheckBox { DataContext: FastbootPartitionRow row }) return;
            if (!row.IsSelected || row.HasFile) return;

            PickImageFor(row.Key);
            // PickImageFor 没选到文件（用户取消）→ 勾选回退，保持「勾选 = 可写入」的直觉
            if (!row.HasFile) row.IsSelected = false;
        }

        /// <summary>表格双击行 = 给该分区选镜像（参考稿的交互）。</summary>
        private void DgFastbootPartitions_MouseDoubleClick(object sender, MouseButtonEventArgs e)
        {
            // 双击勾选框只是把勾选切了两次，不该弹文件对话框（勾选自己的对话框在 ChkPartition_Click 里）
            if (FindAncestor<CheckBox>(e.OriginalSource as DependencyObject) != null) return;

            // 按命中行取，而不是 SelectedItem：双击空白处不该弹文件对话框
            var row = (ItemsControl.ContainerFromElement(DgFastbootPartitions, e.OriginalSource as DependencyObject)
                       as DataGridRow)?.Item as FastbootPartitionRow;
            if (row == null) return;
            PickImageFor(row.Key);
        }

        /// <summary>右键点在哪一行就选中哪一行，再弹菜单。</summary>
        private void DgFastbootPartitions_PreviewMouseRightButtonDown(object sender, MouseButtonEventArgs e)
        {
            var row = FindAncestor<DataGridRow>(e.OriginalSource as DependencyObject);
            if (row != null && !row.IsSelected) row.IsSelected = true;
        }

        // 右键菜单：单分区的选择 / 刷写 / 擦除（对应原来的「选择」与「刷写 X」按钮）
        private void DgRowPickImage_Click(object sender, RoutedEventArgs e)
        {
            if (DgFastbootPartitions.SelectedItem is FastbootPartitionRow row) PickImageFor(row.Key);
        }

        private void DgRowFlash_Click(object sender, RoutedEventArgs e)
        {
            if (DgFastbootPartitions.SelectedItem is not FastbootPartitionRow row) return;
            if (string.IsNullOrWhiteSpace(row.Name)) { ShowError("自定义分区还没填分区名，请先在「自定义分区」输入框里填上"); return; }
            // 自定义分区的镜像路径不在文本框里，走 _customImagePath（与 BtnFlashCustom_Click 同一口径）
            var image = row.Key == "custom" ? _customImagePath : ImageBoxFor(row.Key)?.Text ?? "";
            FlashPartitionAsync(row.Name, image);
        }

        private void DgRowErase_Click(object sender, RoutedEventArgs e)
        {
            if (DgFastbootPartitions.SelectedItem is not FastbootPartitionRow row) return;
            if (string.IsNullOrWhiteSpace(row.Name)) { ShowError("自定义分区还没填分区名，请先在「自定义分区」输入框里填上"); return; }
            if (!Services.MembershipService.GuardAndConsume("Fastboot 擦除 / 格式化", "fastboot.erase")) return;
            RunFastbootCommandAsync($"erase {row.Name}", $"擦除 {row.Name}");
        }

        /// <summary>沿可视树找最近的指定类型祖先（右键 / 双击判断点在了哪个元素上）。</summary>
        private static T? FindAncestor<T>(DependencyObject? node) where T : DependencyObject
        {
            while (node != null)
            {
                if (node is T hit) return hit;
                node = node is Visual || node is System.Windows.Media.Media3D.Visual3D
                    ? VisualTreeHelper.GetParent(node)
                    : LogicalTreeHelper.GetParent(node);
            }
            return null;
        }

        /// <summary>自定义分区名改了 → 表格里的行名跟着变（不用刷新，行实现了 INotifyPropertyChanged）。</summary>
        private void TxtCustomPartition_TextChanged(object sender, TextChangedEventArgs e)
        {
            var row = _rows.Find(r => r.Key == "custom");
            if (row == null) return;
            var name = TxtCustomPartition.Text?.Trim() ?? "";
            row.Name = string.IsNullOrEmpty(name) ? "自定义分区" : name;
        }

        // ─────────────────────────────────────────────────────────────
        //  固件目录（ROM 行）
        // ─────────────────────────────────────────────────────────────

        /// <summary>
        /// 选固件目录：把目录里认得出的镜像自动填进对应分区行（参考稿 ROM 行右侧的文件夹按钮）。
        /// 认不出的分区保持原样，不会把用户已经选好的文件清掉。
        /// </summary>
        private void BtnPickRomDir_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFolderDialog { Title = "选择固件目录" };
            if (dialog.ShowDialog() != true) return;

            var dir = dialog.FolderName;
            TxtRomDir.Text = dir;

            PageLog.Section(TxtLog, "扫描固件目录");
            var sw = Stopwatch.StartNew();
            var matched = 0;
            foreach (var row in _rows)
            {
                if (row.Key == "custom") continue;   // 自定义分区的镜像名由用户给，不猜

                var file = FindImageInDir(dir, row.Key);
                if (file == null)
                {
                    PageLog.Info(TxtLog, $"目录里没有 {row.Key}.img / .bin，跳过");
                    continue;
                }

                var box = ImageBoxFor(row.Key);
                if (box != null) box.Text = file;
                row.FilePath = file;
                PageLog.Info(TxtLog, $"匹配 {row.Key}: {Path.GetFileName(file)}");
                matched++;
            }

            if (matched == 0) LogWarning($"[Fastboot] 目录里没有匹配到可识别的镜像: {dir}");
            PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            ShellStatus.Report(percent: 100);
            ShellStatus.EndTask();
        }

        /// <summary>在目录里找 &lt;分区名&gt;.img / .bin / .mbn（按这个顺序取第一个存在的）。</summary>
        private static string? FindImageInDir(string dir, string partition)
        {
            foreach (var ext in new[] { ".img", ".bin", ".mbn" })
            {
                var path = Path.Combine(dir, partition + ext);
                if (File.Exists(path)) return path;
            }
            return null;
        }

        // ─────────────────────────────────────────────────────────────
        //  分区表：读大小 / 搜索 / 批量写入 / 批量擦除
        // ─────────────────────────────────────────────────────────────

        /// <summary>「搜索分区」：按分区名过滤表格（只动视图，不动 _rows）。</summary>
        private void TxtPartitionSearch_TextChanged(object sender, TextChangedEventArgs e)
        {
            var q = TxtPartitionSearch?.Text?.Trim() ?? "";
            var view = CollectionViewSource.GetDefaultView(_rows);
            view.Filter = o => string.IsNullOrEmpty(q)
                || (o is FastbootPartitionRow r && r.Name.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0);
            view.Refresh();
        }

        /// <summary>把镜像框里的现成内容同步回表格（表格是展示位，镜像是数据源）。</summary>
        private void RefreshPartitionRows()
        {
            foreach (var row in _rows)
            {
                if (row.Key == "custom")
                {
                    if (!string.IsNullOrEmpty(_customImagePath)) row.FilePath = _customImagePath;
                    continue;
                }
                var box = ImageBoxFor(row.Key);
                if (box != null && !string.IsNullOrEmpty(box.Text)) row.FilePath = box.Text;
            }
        }

        /// <summary>
        /// 读分区表：一次 `fastboot getvar all`，把 partition-size 填进表格「大小」列。
        /// 拿不到（工具缺失 / 设备不在 fastboot）就只刷新本地显示，不弹错误框打断用户。
        /// </summary>
        private async void BtnReadPartitionTable_Click(object sender, RoutedEventArgs e)
        {
            PageLog.Section(TxtLog, "读分区表");
            var sw = Stopwatch.StartNew();
            try
            {
                PageLog.BeginOperation(TxtLog, "读取分区表 (fastboot getvar all)");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                if (!await tools.EnsureToolsExistAsync(ReportToolsProgress))
                {
                    PageLog.CompleteOperation(TxtLog, ok: false);
                    LogWarning("[Fastboot] fastboot 工具不可用，仅刷新本地显示");
                }
                else
                {
                    var (success, output, error) = await tools.ExecuteFastbootAsync("getvar all", 120000);
                    PageLog.CompleteOperation(TxtLog, ok: success);
                    if (!string.IsNullOrEmpty(error)) LogWarning($"[Fastboot错误] {error.Trim()}");
                    if (success)
                    {
                        ApplyPartitionSizes(output);
                        UpdateDeviceReadings(output);   // getvar all 顺带把左列 6 行读数也刷一遍
                    }
                    else
                    {
                        LogWarning("[Fastboot] 没读到分区表，仅刷新本地显示");
                    }
                }
            }
            catch (Exception ex)
            {
                PageLog.CompleteOperation(TxtLog, ok: false);
                LogError($"[Fastboot错误] {ex.Message}");
            }

            RefreshPartitionRows();
            PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            ShellStatus.Report(percent: 100);
            ShellStatus.EndTask();
        }

        /// <summary>把 getvar 输出里的 `partition-size:boot: 0x4000000` 填到对应行。</summary>
        private void ApplyPartitionSizes(string? output)
        {
            var sizes = ParsePartitionSizes(output);
            if (sizes.Count == 0)
            {
                LogWarning("[Fastboot] getvar 输出里没有 partition-size 行");
                return;
            }

            foreach (var row in _rows)
            {
                var name = row.Name?.Trim() ?? "";
                if (name.Length > 0 && sizes.TryGetValue(name, out var bytes)) row.Size = FormatSize(bytes);
            }
            PageLog.Info(TxtLog, $"[Fastboot] 读到 {sizes.Count} 个分区大小");
        }

        private static Dictionary<string, long> ParsePartitionSizes(string? output)
        {
            var map = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
            if (string.IsNullOrEmpty(output)) return map;

            foreach (var raw in output.Split('\n'))
            {
                var m = Regex.Match(raw.Trim(), @"partition-size:\s*([A-Za-z0-9_\-\.]+)\s*:\s*(0x[0-9A-Fa-f]+|\d+)");
                if (!m.Success) continue;

                var text = m.Groups[2].Value;
                try
                {
                    map[m.Groups[1].Value] = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
                        ? Convert.ToInt64(text.Substring(2), 16)
                        : long.Parse(text);
                }
                catch
                {
                    // 单个分区大小解析失败不影响其它分区
                }
            }
            return map;
        }

        /// <summary>写入分区：把勾选行（且已选镜像）的镜像依次刷进对应分区。</summary>
        private async void BtnFlashSelected_Click(object sender, RoutedEventArgs e)
        {
            var targets = _rows.Where(r => r.IsSelected).ToList();
            if (targets.Count == 0)
            {
                ShowError("请先勾选要写入的分区");
                return;
            }

            var missing = targets.Where(t => !t.HasFile).ToList();
            if (missing.Count > 0)
            {
                ShowError($"这些分区还没选镜像文件（双击对应行选择）: {string.Join("、", missing.Select(t => t.Name))}");
                return;
            }

            // 自定义分区勾上了却没填分区名 → 下发 `fastboot flash  ""` 只有坏处，先拦下来
            if (targets.Any(t => string.IsNullOrWhiteSpace(t.Name)))
            {
                ShowError("自定义分区还没填分区名，请先在「自定义分区」输入框里填上");
                return;
            }

            PageLog.Section(TxtLog, "写入分区");
            var sw = Stopwatch.StartNew();
            foreach (var row in targets)
            {
                // 逐分区一行操作行：Section 已经开了整段，这里用 Begin/Complete 保持「一行一次操作」
                PageLog.BeginOperation(TxtLog, $"写入 {row.Name}: {Path.GetFileName(row.FilePath)}");
                var ok = await FlashPartitionCoreAsync(row.Name, row.FilePath);
                PageLog.CompleteOperation(TxtLog, ok);
            }
            PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            ShellStatus.Report(percent: 100);
            ShellStatus.EndTask();
        }

        /// <summary>擦除分区：擦掉勾选的分区（顺序下发，避免两条 fastboot 命令互相打断）。</summary>
        private async void BtnEraseSelected_Click(object sender, RoutedEventArgs e)
        {
            var targets = _rows.Where(r => r.IsSelected).ToList();
            if (targets.Count == 0)
            {
                ShowError("请先勾选要擦除的分区");
                return;
            }

            // 分区名为空（自定义分区没填名）时不能下发 `fastboot erase ` 这种半截命令
            if (targets.Any(t => string.IsNullOrWhiteSpace(t.Name)))
            {
                ShowError("自定义分区还没填分区名，请先在「自定义分区」输入框里填上");
                return;
            }

            if (!Services.MembershipService.EnsureAllowed("Fastboot 擦除 / 格式化", "fastboot.erase")) return;
            var names = string.Join("、", targets.Select(t => t.Name));
            if (MessageBox.Show($"确定擦除以下分区？\n{names}", "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
                return;

            PageLog.Section(TxtLog, "擦除分区");
            var sw = Stopwatch.StartNew();
            foreach (var row in targets)
            {
                // 与「擦除 Cache / 擦除 Metadata」同一口径：一个分区一份额度，额度用完立刻停手
                if (!Services.MembershipService.GuardAndConsume("Fastboot 擦除 / 格式化", "fastboot.erase")) break;
                await RunFastbootCommandCoreAsync($"erase {row.Name}", $"擦除 {row.Name}");
            }
            PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            ShellStatus.Report(percent: 100);
            ShellStatus.EndTask();
        }

        // ─────────────────────────────────────────────────────────────
        //  刷写 / 通用 fastboot 命令
        // ─────────────────────────────────────────────────────────────

        private void BtnFlashBoot_Click(object sender, RoutedEventArgs e) => FlashPartitionAsync("boot", TxtBootImage.Text);
        private void BtnFlashInitBoot_Click(object sender, RoutedEventArgs e) => FlashPartitionAsync("init_boot", TxtInitBootImage.Text);
        private void BtnFlashVendorBoot_Click(object sender, RoutedEventArgs e) => FlashPartitionAsync("vendor_boot", TxtVendorBootImage.Text);
        private void BtnFlashDtbo_Click(object sender, RoutedEventArgs e) => FlashPartitionAsync("dtbo", TxtDtboImage.Text);
        private void BtnFlashRecovery_Click(object sender, RoutedEventArgs e) => FlashPartitionAsync("recovery", TxtRecoveryImage.Text);
        private void BtnFlashSystem_Click(object sender, RoutedEventArgs e) => FlashPartitionAsync("system", TxtSystemImage.Text);

        private void BtnFlashCustom_Click(object sender, RoutedEventArgs e)
        {
            var partition = TxtCustomPartition.Text?.Trim();
            if (string.IsNullOrEmpty(partition))
            {
                ShowError("请输入自定义分区名");
                return;
            }
            if (string.IsNullOrEmpty(_customImagePath))
            {
                ShowError("请选择镜像文件");
                return;
            }
            FlashPartitionAsync(partition, _customImagePath);
        }

        /// <summary>
        /// 单分区刷写入口（6 个固定分区按钮 + 自定义分区按钮共用）。
        /// 参考稿的日志三明治（Section 开头 / Footer 收尾）放在这一层；
        /// 原来的校验与 fastboot 调用顺序一行未动，只是搬进了 <see cref="FlashPartitionCoreAsync"/>，
        /// 好让「写入分区」（批量）能 await 同一个实现，不必抄一份。
        /// </summary>
        private async void FlashPartitionAsync(string partition, string image)
        {
            PageLog.Section(TxtLog, $"写入 {partition}");
            var sw = Stopwatch.StartNew();
            await FlashPartitionCoreAsync(partition, image);
            PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            ShellStatus.Report(percent: 100);
            ShellStatus.EndTask();
        }

        /// <summary>刷写一个分区的实际流程（原 FlashPartitionAsync 正文，逻辑未改，只改成可 await）。</summary>
        private async Task<bool> FlashPartitionCoreAsync(string partition, string image)
        {
            if (string.IsNullOrEmpty(image))
            {
                ShowError($"请选择 {partition} 镜像文件");
                return false;
            }
            if (!File.Exists(image))
            {
                ShowError($"镜像文件不存在: {image}");
                return false;
            }
            // 6 个固定分区按钮 + 自定义分区都汇到这一个方法，额度校验放这里即可全覆盖
            if (!Services.MembershipService.GuardAndConsume("Fastboot 刷写", "fastboot.flash")) return false;
            try
            {
                LogInfo($"[Fastboot] 刷写 {partition}: fastboot flash {partition} \"{image}\"");
                ShellStatus.Report(partition: partition, percent: 0);
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                if (!await tools.EnsureToolsExistAsync(ReportToolsProgress))
                {
                    ShowError("fastboot 工具不可用，请检查网络后重试");
                    return false;
                }

                var (success, output, error) = await tools.ExecuteFastbootAsync($"flash {partition} \"{image}\"", 600000);
                if (success && !string.IsNullOrEmpty(output)) LogInfo(output.Trim());
                if (!string.IsNullOrEmpty(error)) LogWarning($"[Fastboot错误] {error.Trim()}");
                if (success)
                {
                    LogSuccess($"[Fastboot] {partition} 刷写成功");
                    ShowSuccess($"{partition} 刷写成功");
                }
                else
                {
                    ShowError($"{partition} 刷写失败，请确认设备处于 Fastboot 模式且镜像与机型匹配");
                }
                return success;
            }
            catch (Exception ex)
            {
                LogError($"[Fastboot错误] {ex.Message}");
                ShowError($"刷写异常: {ex.Message}");
                return false;
            }
        }

        private void BtnUnlockBl_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.EnsureAllowed("引导锁解锁 / 回锁", "fastboot.bl")) return;
            if (MessageBox.Show("解锁Bootloader会清除数据，确定继续？", "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
            {
                if (!Services.MembershipService.Consume("fastboot.bl")) return;
                RunFastbootCommandAsync("oem unlock", "解锁Bootloader");
            }
        }

        private void BtnLockBl_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.EnsureAllowed("引导锁解锁 / 回锁", "fastboot.bl")) return;
            if (MessageBox.Show("确定要锁定Bootloader？", "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
            {
                if (!Services.MembershipService.Consume("fastboot.bl")) return;
                RunFastbootCommandAsync("oem lock", "锁定Bootloader");
            }
        }

        private void BtnGetUnlockStatus_Click(object sender, RoutedEventArgs e) => RunFastbootCommandAsync("oem device-info", "查询解锁状态");

        private void BtnFormatData_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.EnsureAllowed("Fastboot 擦除 / 格式化", "fastboot.erase")) return;
            if (MessageBox.Show("格式化数据会清除所有用户数据，确定继续？", "确认", MessageBoxButton.YesNo, MessageBoxImage.Warning) == MessageBoxResult.Yes)
            {
                if (!Services.MembershipService.Consume("fastboot.erase")) return;
                RunFastbootCommandAsync("-w", "格式化数据");
            }
        }

        private void BtnEraseCache_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.GuardAndConsume("Fastboot 擦除 / 格式化", "fastboot.erase")) return;
            RunFastbootCommandAsync("erase cache", "擦除 Cache");
        }

        private void BtnEraseMetadata_Click(object sender, RoutedEventArgs e)
        {
            if (!Services.MembershipService.GuardAndConsume("Fastboot 擦除 / 格式化", "fastboot.erase")) return;
            RunFastbootCommandAsync("erase metadata", "擦除 Metadata");
        }

        /// <summary>
        /// 平台工具（adb / fastboot）下载的**真实**字节进度 → 外壳底部通栏进度条 + 状态栏。
        /// <para>
        /// 本页没有页内进度条（进度由外壳承担），所以只喂 <see cref="ShellStatus"/>。
        /// 服务端不给 Content-Length 时 total = -1 ⇒ 报 -1（外壳按"与总进度相同"处理，
        /// 不会出现编造的百分比）。
        /// </para>
        /// </summary>
        private void ReportToolsProgress(long got, long total)
        {
            var pct = total > 0 ? (double)got / total * 100 : -1;
            Dispatcher.BeginInvoke(new Action(() =>
                ShellStatus.Report(task: "加载平台工具", percent: pct, partitionPercent: -1)));
        }

        private void BtnErasePartition_Click(object sender, RoutedEventArgs e)
        {
            var partition = TxtErasePartition.Text?.Trim();
            if (!string.IsNullOrEmpty(partition))
            {
                if (!Services.MembershipService.GuardAndConsume("Fastboot 擦除 / 格式化", "fastboot.erase")) return;
                RunFastbootCommandAsync($"erase {partition}", $"擦除 {partition}");
            }
        }

        private void BtnGetvarAll_Click(object sender, RoutedEventArgs e) => RunFastbootCommandAsync("getvar all", "Getvar全部信息");
        private void BtnGetDeviceInfo_Click(object sender, RoutedEventArgs e) => RunFastbootCommandAsync("getvar product", "设备信息");
        private void BtnGetSlotInfo_Click(object sender, RoutedEventArgs e) => RunFastbootCommandAsync("getvar slot-count", "Slot信息");

        /// <summary>按钮入口：区段三明治（Section / Footer）包住一次完整命令。</summary>
        private async void RunFastbootCommandAsync(string arguments, string description)
        {
            PageLog.Section(TxtLog, description);
            var sw = Stopwatch.StartNew();
            await RunFastbootCommandCoreAsync(arguments, description);
            PageLog.Footer(TxtLog, $"完成, 耗时: {sw.Elapsed.TotalSeconds:F2}秒");
            ShellStatus.Report(percent: 100);
            ShellStatus.EndTask();
        }

        /// <summary>下发一条 fastboot 命令（原 RunFastbootCommandAsync 正文，逻辑未改，只改成可 await）。</summary>
        private async Task<bool> RunFastbootCommandCoreAsync(string arguments, string description)
        {
            try
            {
                LogInfo($"[Fastboot] {description}: fastboot {arguments}");
                PageLog.BeginOperation(TxtLog, $"fastboot {arguments}");
                var tools = new PlatformToolsManager(msg => LogInfo(msg));
                if (!await tools.EnsureToolsExistAsync(ReportToolsProgress))
                {
                    PageLog.CompleteOperation(TxtLog, ok: false);
                    ShowError("fastboot 工具不可用，请检查网络后重试");
                    return false;
                }

                var (success, output, error) = await tools.ExecuteFastbootAsync(arguments, 120000);
                PageLog.CompleteOperation(TxtLog, ok: success);
                if (success && !string.IsNullOrEmpty(output))
                {
                    LogInfo(output.Trim());
                    // getvar / oem device-info 的输出顺手填进左列「设备信息」6 行读数
                    UpdateDeviceReadings(output);
                }
                if (!string.IsNullOrEmpty(error)) LogWarning($"[Fastboot错误] {error.Trim()}");

                if (success)
                {
                    ShowSuccess($"{description} 完成");
                }
                else
                {
                    ShowError($"{description} 失败，请确认设备已连接并处于 Fastboot 模式");
                }
                return success;
            }
            catch (Exception ex)
            {
                PageLog.CompleteOperation(TxtLog, ok: false);
                LogError($"[Fastboot错误] {ex.Message}");
                return false;
            }
        }

        // ─────────────────────────────────────────────────────────────
        //  设备信息读数（左列 6 行，数据源就是上面的 getvar 输出）
        // ─────────────────────────────────────────────────────────────

        /// <summary>
        /// 解析 getvar / oem device-info 的输出，填「序列号 / 安全启动 / 设备代号 / 解锁状态 / 启动槽位 / 设备模式」。
        /// 解析到哪条填哪条（没解析到的保持原样，初始就是 "-"），避免「查 product」把序列号冲成空。
        /// </summary>
        private void UpdateDeviceReadings(string? output)
        {
            var vars = ParseGetvarLines(output);
            if (vars.Count == 0) return;

            var touched = false;

            if (TryVar(vars, "serialno", out var serial)) { TxtFbSerial.Text = serial; touched = true; }
            // secure / unlocked 这类布尔量翻成人话，其余变量原样显示
            if (TryVar(vars, "secure", out var secure)) { TxtFbSecure.Text = YesNo(secure, "开启", "关闭"); touched = true; }
            if (TryVar(vars, "product", out var product)) { TxtFbCodename.Text = product; touched = true; }
            // 解锁状态：getvar all 给 unlocked=yes/no，oem device-info 给 "Device unlocked: true/false"
            var unlocked = TryVar(vars, "unlocked", out var unlockedVar)
                ? unlockedVar
                : (TryVar(vars, "device-unlocked", out var deviceUnlockedVar) ? deviceUnlockedVar : "");
            if (!string.IsNullOrEmpty(unlocked))
            {
                TxtFbUnlocked.Text = YesNo(unlocked, "已解锁", "未解锁");
                touched = true;
            }
            if (TryVar(vars, "current-slot", out var slot)) { TxtFbSlot.Text = slot; touched = true; }
            else if (TryVar(vars, "slot-count", out var slotCount)) { TxtFbSlot.Text = $"{slotCount} 槽位"; touched = true; }

            if (!touched) return;

            // 设备模式：is-userspace=yes 就是 fastbootd（用户态），否则按 fastboot 常态写 Bootloader
            var mode = TryVar(vars, "is-userspace", out var userspace) && userspace.Equals("yes", StringComparison.OrdinalIgnoreCase)
                ? "fastbootd"
                : "Bootloader";
            TxtFbMode.Text = mode;

            // 顺手上报外壳：底部状态栏的「设备」段与共用左栏都读这一份（参考稿里设备读数只有一个来源）
            var device = TxtFbSerial.Text != "-" ? TxtFbSerial.Text : TxtFbCodename.Text;
            if (device != "-")
            {
                ShellStatus.SetDevice(device: device, mode: mode, hint: "就绪");
                DeviceStateHub.Report(device, mode);
            }
        }

        /// <summary>把 fastboot 的 `(bootloader) key: value` 输出摊平成字典（键小写、空格换连字符）。</summary>
        private static Dictionary<string, string> ParseGetvarLines(string? output)
        {
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            if (string.IsNullOrEmpty(output)) return map;

            foreach (var raw in output.Split('\n'))
            {
                var line = raw.Trim();
                if (line.StartsWith("(bootloader)", StringComparison.OrdinalIgnoreCase))
                    line = line.Substring("(bootloader)".Length).Trim();

                var idx = line.IndexOf(':');
                if (idx <= 0) continue;

                var key = line.Substring(0, idx).Trim().ToLowerInvariant().Replace(' ', '-');
                if (key.Length == 0) continue;
                map[key] = line.Substring(idx + 1).Trim();
            }
            return map;
        }

        private static bool TryVar(Dictionary<string, string> vars, string key, out string value)
        {
            if (vars.TryGetValue(key, out var v) && !string.IsNullOrWhiteSpace(v))
            {
                value = v.Trim();
                return true;
            }
            value = "";
            return false;
        }

        /// <summary>yes/true → <paramref name="yes"/>，no/false → <paramref name="no"/>，其它原样返回。</summary>
        private static string YesNo(string value, string yes, string no)
        {
            if (string.IsNullOrEmpty(value) || value == "-") return "-";
            if (value.Equals("yes", StringComparison.OrdinalIgnoreCase) || value.Equals("true", StringComparison.OrdinalIgnoreCase)) return yes;
            if (value.Equals("no", StringComparison.OrdinalIgnoreCase) || value.Equals("false", StringComparison.OrdinalIgnoreCase)) return no;
            return value;
        }

        /// <summary>字节数转人话（与 Payload 页同一口径，两页读数格式保持一致）。</summary>
        private static string FormatSize(long bytes)
        {
            if (bytes < 1024) return $"{bytes} B";
            if (bytes < 1024 * 1024) return $"{bytes / 1024.0:F1} KB";
            if (bytes < 1024 * 1024 * 1024) return $"{bytes / 1024.0 / 1024.0:F1} MB";
            return $"{bytes / 1024.0 / 1024.0 / 1024.0:F2} GB";
        }
    }
}
