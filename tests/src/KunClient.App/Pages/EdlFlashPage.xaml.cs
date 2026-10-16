// ============================================================================
// EDL（Qualcomm 9008）页 —— 代码后置
//
// 2026-10-09：整套照 `third_party/swt-edl/src/ui/MainWindow.cpp` 复刻，
// 底层是 C# 逐文件移植的 `KunFlashTool.Core.SwtEdl.*`（EdlWorker 编排）。
//
// 🔴 两个必须知道的引擎事实（改本页前先看）：
//   ① **EdlWorker 是纯同步的** —— 它没有自己的线程，`ConnectDevice` / `FlashXml`
//      这些方法会**一路阻塞到操作结束**，且事件回调在**调用线程**上触发。
//      所以：所有 worker 调用必须走本文件的 RunWorker（内部 Task.Run），
//      所有事件回调必须 Dispatcher.BeginInvoke 切回 UI 线程。
//      直接在 UI 线程调 = 界面假死到刷机结束（长任务期间连"停止"都点不到）。
//   ② 引擎的 PartitionsReady 只给 List<PartitionInfo>（分区名 + LUN + LBA），
//      **不含**文件名/大小/状态 —— 那三列由 UI 自己从「选中的 rawprogram XML」
//      或「用户双击选中的镜像」推出来。别指望引擎给。
//
// 🔴 与 XAML 的隐形契约（BasePage 靠名字取，改名即静默失效）：
//      TxtLog（RichTextBox）· NotifyBar / NotifyIcon / NotifyText
// ============================================================================
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.IO.Ports;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Threading;
using KunFlashTool.App.Services;
using KunFlashTool.Core.SwtEdl;
using KunFlashTool.Core.SwtEdl.App;
using KunFlashTool.Core.SwtEdl.Oplus;

namespace KunFlashTool.App.Pages;

public partial class EdlFlashPage : BasePage
{
    // ── 引擎 ──────────────────────────────────────────────────────────────
    private readonly EdlWorker _worker = new();

    // ── 运行状态（对应 C++ 的 m_connected / m_busy / …）──────────────────
    private bool _connected;
    private bool _busy;
    private bool _dark;
    private bool _debugLogEnabled;
    private bool _readGptAfterConnect;
    private bool _readVersionAfterGpt;

    private readonly ObservableCollection<PartitionRow> _rows = new();
    private List<string> _selectedXmlPaths = new();
    private string _lastPortSnapshot = "";
    private OplusVipPackage _vipPackage = new();

    // ── 自动扫描 ─────────────────────────────────────────────────────────
    private readonly DispatcherTimer _autoScan = new() { Interval = TimeSpan.FromMilliseconds(1500) };

    // ── 速率平滑（对应 C++ 的 m_smoothedBytesPerSecond）──────────────────
    private DateTime _speedStamp = DateTime.MinValue;
    private ulong _speedLastCompleted, _speedTotal;
    private double _speedLastUpdateMs;
    private double _smoothedBps;

    // ── 日志缓冲（Debug 关掉时也留着，开启后立刻补全）────────────────────
    private readonly List<(DateTime Time, string Text)> _logHistory = new();
    private const int MaxLogRecords = 50000;

    public EdlFlashPage()
    {
        InitializeComponent();

        DgPartitions.ItemsSource = _rows;

        _worker.LogMessage += OnLog;
        _worker.StateChanged += OnStateChanged;
        _worker.ProgressChanged += OnProgress;
        _worker.OverallProgressChanged += OnOverallProgress;
        _worker.DeviceInfoChanged += OnDeviceInfo;
        _worker.PartitionsReady += OnPartitionsReady;
        _worker.OperationFinished += OnOperationFinished;

        _autoScan.Tick += (_, _) => RefreshPorts();

        Loaded += OnLoadedOnce;
        Unloaded += OnUnloadedOnce;
        ThemeManager.ThemeChanged += OnThemeChanged;
    }

    private void OnLoadedOnce(object sender, RoutedEventArgs e)
    {
        Loaded -= OnLoadedOnce;
        _dark = ThemeManager.CurrentTheme == ThemeManager.Theme.Dark;

        RestoreSettings();
        RefreshPorts();
        SetBusy(false);

        LogInfo("EDL 初始化完成");
        LogInfo("请选择 Qualcomm 9008 串口和 Firehose Programmer");
    }

    private void OnUnloadedOnce(object sender, RoutedEventArgs e)
    {
        Unloaded -= OnUnloadedOnce;
        ThemeManager.ThemeChanged -= OnThemeChanged;
        _autoScan.Stop();
        // ⚠️ EdlWorker 没有 Dispose（它的 SerialTransport 是每次操作自建自收的）；
        //    页面卸载时只要停掉任务、别让回调再打回来即可。
        try { _worker.RequestStop(); } catch { }
    }

    private void OnThemeChanged(ThemeManager.Theme theme)
    {
        _dark = theme == ThemeManager.Theme.Dark;
        RebuildLogView();   // 日志着色是按当前主题算的，换主题要重画
    }

    // =====================================================================
    // 设置默认值
    //   ⚠️ C++ 侧用 QSettings 持久化；本页暂用固定默认值（自动扫描/保护分区/读取信息默认开，
    //     与 C++ 首次启动的默认一致）。要持久化时接 AppSettings，别直接写注册表。
    // =====================================================================
    private void RestoreSettings()
    {
        ChkAutoScan.IsChecked = true;
        ChkAutoReboot.IsChecked = false;
        ChkProtect.IsChecked = true;
        ChkReadInfo.IsChecked = true;
        ChkGenerateXml.IsChecked = true;
        ChkVip.IsChecked = false;
        CmbMode.SelectedIndex = 0;
        CmbChipset.SelectedIndex = 0;
        CmbStorage.SelectedIndex = 0;
        if (ChkAutoScan.IsChecked == true) _autoScan.Start();
    }

    // =====================================================================
    // 端口
    // =====================================================================
    private void RefreshPorts()
    {
        if (_connected || _busy) return;

        var ports = EnumeratePorts(out var snapshot);
        if (snapshot == _lastPortSnapshot && CmbPort.Items.Count > 0) return;
        _lastPortSnapshot = snapshot;

        var previous = CurrentPortName();
        CmbPort.Items.Clear();

        var preferred = -1;
        foreach (var p in ports)
        {
            CmbPort.Items.Add(new ComboBoxItem { Content = p.Display, Tag = p.Name });
            if (preferred < 0 && p.IsQualcomm) preferred = CmbPort.Items.Count - 1;
            if (p.Name == previous) preferred = CmbPort.Items.Count - 1;
        }

        if (CmbPort.Items.Count == 0)
            CmbPort.Items.Add(new ComboBoxItem { Content = "未发现串口", Tag = "" });
        else
            CmbPort.SelectedIndex = preferred >= 0 ? preferred : 0;

        if (!_busy && !_connected)
            BtnConnect.IsEnabled = CurrentPortName().Length > 0;

        LogInfo($"扫描完成，发现 {ports.Count} 个串口");
    }

    private string CurrentPortName()
        => (CmbPort.SelectedItem as ComboBoxItem)?.Tag as string ?? "";

    private sealed record PortEntry(string Name, string Display, bool IsQualcomm);

    /// <summary>
    /// 枚举串口。优先用 WMI 拿「描述 + VID/PID」以便自动挑出 9008；
    /// WMI 不可用时**降级成只有端口名**，绝不因此让整页不可用
    /// （C++ 的 QSerialPortInfo 天然带这些信息，.NET 侧得自己查）。
    /// </summary>
    private static List<PortEntry> EnumeratePorts(out string snapshot)
    {
        var names = new List<string>();
        try { names.AddRange(SerialPort.GetPortNames()); } catch { }
        names.Sort(StringComparer.OrdinalIgnoreCase);

        var desc = new Dictionary<string, (string Text, bool Qualcomm)>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var searcher = new System.Management.ManagementObjectSearcher(
                "SELECT Caption, PNPDeviceID FROM Win32_PnPEntity WHERE Caption LIKE '%(COM%'");
            foreach (System.Management.ManagementBaseObject o in searcher.Get())
            {
                try
                {
                    var caption = o["Caption"] as string ?? "";
                    var pnp = o["PNPDeviceID"] as string ?? "";
                    var m = Regex.Match(caption, @"\((COM\d+)\)");
                    if (!m.Success) continue;
                    var port = m.Groups[1].Value;
                    // 05C6 = Qualcomm；9008 / QDLoader 是 EDL 的设备名特征
                    var isQualcomm = pnp.Contains("VID_05C6", StringComparison.OrdinalIgnoreCase)
                                     || caption.Contains("9008", StringComparison.OrdinalIgnoreCase)
                                     || caption.Contains("QDLoader", StringComparison.OrdinalIgnoreCase)
                                     || caption.Contains("Qualcomm", StringComparison.OrdinalIgnoreCase);
                    desc[port] = (caption.Replace("(" + port + ")", "").Trim(), isQualcomm);
                    if (!names.Contains(port, StringComparer.OrdinalIgnoreCase)) names.Add(port);
                }
                catch { }
                finally { try { o.Dispose(); } catch { } }
            }
        }
        catch { /* 无 WMI 权限：只剩端口名，功能不受影响 */ }

        var entries = new List<PortEntry>();
        foreach (var n in names)
        {
            var has = desc.TryGetValue(n, out var d);
            entries.Add(new PortEntry(n,
                has && d.Text.Length > 0 ? $"{n}  ·  {d.Text}" : $"{n}  ·  串口设备",
                has && d.Qualcomm));
        }
        snapshot = string.Join(";", entries.Select(e => e.Name));
        return entries;
    }

    private void CmbPort_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_busy || _connected) return;
        BtnConnect.IsEnabled = CurrentPortName().Length > 0;
    }

    private void ChkAutoScan_Changed(object sender, RoutedEventArgs e)
    {
        if (ChkAutoScan.IsChecked == true) { RefreshPorts(); _autoScan.Start(); }
        else _autoScan.Stop();
    }

    private void CmbStorage_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        // 存储类型变了 → 分区大小/保护的显示口径都要重算
        foreach (var row in _rows)
        {
            row.Size = FormatBytes(SafeMul(row.Info.SectorCount, BytesPerSector()));
            RefreshRowStatus(row);
        }
    }

    private StorageType CurrentStorage()
        => CmbStorage.SelectedIndex == 1 ? StorageType.Emmc : StorageType.Ufs;

    private ulong BytesPerSector() => CurrentStorage() == StorageType.Ufs ? 4096UL : 512UL;

    // =====================================================================
    // 连接 / 断开
    // =====================================================================
    private void BtnConnect_Click(object sender, RoutedEventArgs e)
    {
        if (_connected)
        {
            _readGptAfterConnect = false;
            SetBusy(true);
            RunWorker(() => _worker.DisconnectDevice());
            return;
        }

        var port = CurrentPortName();
        if (port.Length == 0)
        {
            ShowError("请选择有效的 Qualcomm 9008 串口。");
            return;
        }

        var programmer = TxtProgrammer.Text.Trim();
        var storage = CurrentStorage();

        var vip = new OplusVipPackage();
        if (ChkVip.IsChecked == true)
        {
            if (!LoadVipPackage(showDialog: true)) return;
            vip = _vipPackage;
            programmer = vip.ProgrammerPath;
            // 走授权通道这件事必须在日志里留一条**人能看懂**的记录：
            // 后面设备侧任何异常（含厂商侧审计）都能回溯到"这次是谁、什么时候开的"。
            LogInfo("本次连接启用 OPPO VIP 授权通道（需厂商签发的 digest + signature 授权文件），"
                    + "仅应用于本人所有或已获授权维修的设备。");
        }

        TxtStateBanner.Text = "正在连接设备…";
        _readGptAfterConnect = true;
        SetBusy(true);
        RunWorker(() => _worker.ConnectDevice(port, programmer, storage, vip));
    }

    // =====================================================================
    // 分区表
    // =====================================================================
    private sealed class PartitionRow : INotifyPropertyChanged
    {
        public PartitionInfo Info { get; init; } = new();
        public string Name => Info.Name;
        public string ImagePath { get; set; } = "";

        private bool _isSelected;
        public bool IsSelected
        {
            get => _isSelected;
            set { if (_isSelected != value) { _isSelected = value; Raise(nameof(IsSelected)); } }
        }

        private string _fileName = "—";
        public string FileName
        {
            get => _fileName;
            set { if (_fileName != value) { _fileName = value; Raise(nameof(FileName)); } }
        }

        private string _size = "—";
        public string Size
        {
            get => _size;
            set { if (_size != value) { _size = value; Raise(nameof(Size)); } }
        }

        private string _status = "就绪";
        public string Status
        {
            get => _status;
            set { if (_status != value) { _status = value; Raise(nameof(Status)); } }
        }

        public event PropertyChangedEventHandler? PropertyChanged;
        private void Raise(string n) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
    }

    private void UpdateEmptyState()
        => TxtEmptyState.Visibility = _rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

    private void OnPartitionsReady(List<PartitionInfo> partitions)
    {
        Dispatcher.BeginInvoke(new Action(() =>
        {
            _rows.Clear();
            var bps = BytesPerSector();
            foreach (var p in partitions)
            {
                var row = new PartitionRow
                {
                    Info = p,
                    Size = FormatBytes(SafeMul(p.SectorCount, bps)),
                };
                RefreshRowStatus(row);
                _rows.Add(row);
            }
            UpdateEmptyState();
            if (_rows.Count > 0) DgPartitions.SelectedIndex = 0;
            LogInfo($"分区表已载入：{_rows.Count} 个分区");
        }));
    }

    private static ulong SafeMul(ulong a, ulong b)
        => b == 0 || a > ulong.MaxValue / b ? (b == 0 ? 0 : ulong.MaxValue) : a * b;

    private static ulong SafeAdd(ulong a, ulong b)
        => a > ulong.MaxValue - b ? ulong.MaxValue : a + b;

    /// <summary>重算某行的「状态」列。口径与 C++ 的 refreshPartitionRowStatus 一致。</summary>
    private void RefreshRowStatus(PartitionRow row)
    {
        var guarded = PartitionProtection.IsProtectedPartitionName(row.Info.Name)
                      && ChkProtect.IsChecked == true;

        if (row.ImagePath.Length > 0 && File.Exists(row.ImagePath))
            row.Status = "镜像已选择";
        else if (row.IsSelected)
            row.Status = "双击选择镜像";
        else
            row.Status = guarded ? "保护已开启" : "就绪";
    }

    private void ChkPartition_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not CheckBox cb) return;
        if (cb.DataContext is not PartitionRow row) return;
        RefreshRowStatus(row);
    }

    private void ChkProtect_Changed(object sender, RoutedEventArgs e)
    {
        foreach (var row in _rows) RefreshRowStatus(row);
    }

    /// <summary>
    /// 保护分区二次确认（对应 C++ 的 applyProtectionPolicy）。
    /// 返回 false = 勾选的全是保护分区且用户选择跳过 → 调用方应中止本次任务。
    /// </summary>
    private bool ApplyProtectionPolicy(List<PartitionInfo> partitions, List<string>? imagePaths,
                                       string operation, out bool allowProtected)
    {
        allowProtected = ChkProtect.IsChecked != true;
        if (allowProtected) return partitions.Count > 0;

        var names = partitions.Where(p => PartitionProtection.IsProtectedPartitionName(p.Name))
                              .Select(p => p.Name)
                              .Distinct(StringComparer.OrdinalIgnoreCase)
                              .ToList();
        if (names.Count == 0) return partitions.Count > 0;

        var text = $"{operation}任务包含以下校准、基带指纹或设备配置分区：\n\n"
                 + string.Join("、", names)
                 + "\n\n默认动作会跳过这些分区，其余分区继续执行。";

        var ok = MessageBox.Show(text, "保护分区确认", MessageBoxButton.YesNo,
            MessageBoxImage.Warning, MessageBoxResult.No) == MessageBoxResult.Yes;
        if (ok)
        {
            allowProtected = true;
            LogInfo($"保护分区二次确认已通过：{string.Join("、", names)}");
            return true;
        }

        // 跳过保护分区：连同对应的镜像路径一起删（索引必须同步，否则镜像会错位）
        for (var i = partitions.Count - 1; i >= 0; i--)
        {
            if (!PartitionProtection.IsProtectedPartitionName(partitions[i].Name)) continue;
            partitions.RemoveAt(i);
            if (imagePaths != null && i < imagePaths.Count) imagePaths.RemoveAt(i);
        }
        LogInfo($"保护分区已跳过：{string.Join("、", names)}");
        return partitions.Count > 0;
    }

    // =====================================================================
    // 资料 / 镜像选择
    // =====================================================================
    private void BtnPickXml_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "选择刷机 XML",
            Filter = "rawprogram XML (*.xml)|*.xml|所有文件 (*.*)|*.*",
            Multiselect = true,
            InitialDirectory = _selectedXmlPaths.Count > 0
                ? Path.GetDirectoryName(_selectedXmlPaths[0]) ?? "" : "",
        };
        if (dlg.ShowDialog() != true) return;
        PickXmlFiles(dlg.FileNames);
    }

    private void PickXmlFiles(IEnumerable<string> selectedPaths)
    {
        // 1) 只认 rawprogram*.xml；patch / provision / contents 等不是刷写描述文件，
        //    强行喂给解析器只会得到「没有可刷写的 program 条目」。忽略并提示。
        var rawSelected = selectedPaths
            .Where(p => ParseXmlName(Path.GetFileName(p)).Kind != XmlKind.Other)
            .ToList();
        foreach (var other in selectedPaths.Except(rawSelected, StringComparer.OrdinalIgnoreCase))
            LogWarning($"已忽略非 rawprogram 文件：{Path.GetFileName(other)}（patch/provision/contents.xml 不是刷写描述）");
        if (rawSelected.Count == 0)
        {
            ShowError("请选择 rawprogram*.xml（如包内的 rawprogram1.xml）。\n" +
                      "patch*.xml / provision*.xml / contents.xml 不是刷写描述文件，无法载入。");
            return;
        }

        // 2) 同目录同族兄弟一起带上（一个包的所有 LUN 必须一起刷）
        var expanded = ExpandRawprogramXmlSelection(rawSelected);

        // 3) 双族（标准 rawprogram* + 非标准 rawprogram_unsparse*）只保留用户选的那一族，
        //    另一族是同一份固件的「稀疏 / 非稀疏」替代表述，不能混刷。择一即可，不是漏刷。
        var resolved = ResolveSingleFamily(expanded, rawSelected, out var droppedFamilies, out var keptFamily);
        if (droppedFamilies.Count > 0)
            LogInfo($"检测到双族固件包：已自动选用「{FamilyLabel(keptFamily)}」，跳过「{string.Join("、", droppedFamilies.Select(FamilyLabel))}」"
                    + "（两套是同一份固件的不同表述，择一即可，不影响刷写完整性）。");
        if (resolved.Count == 0) return;

        var xmlPaths = resolved;
        if (xmlPaths.Count == 0) return;

        var plan = new RawProgramPlan();
        if (!RawProgramParser.Parse(xmlPaths, plan, out var error))
        {
            ShowError(error);
            LogError($"XML 资料解析失败：{error}");
            return;
        }

        _selectedXmlPaths = plan.XmlPaths.ToList();
        var first = _selectedXmlPaths[0];
        var category = XmlCategoryName(_selectedXmlPaths);
        TxtXmlPath.Text = _selectedXmlPaths.Count == 1
            ? first
            : $"[{category}] {Path.GetFileName(first)}  +{_selectedXmlPaths.Count - 1}";
        TxtXmlHint.Visibility = Visibility.Collapsed;
        TxtXmlPath.ToolTip = string.Join("\n", _selectedXmlPaths);

        // 未连接时，直接用 XML 计划填充表格（对应 C++ chooseFirmwareDirectory 的做法）
        if (!_connected)
        {
            _rows.Clear();
            foreach (var entry in plan.Entries)
            {
                var file = entry.SourcePath;
                var exists = file.Length > 0 && File.Exists(file);
                long len = 0;
                if (exists) { try { len = new FileInfo(file).Length; } catch { } }
                var row = new PartitionRow
                {
                    Info = new PartitionInfo { Name = entry.Label.Length > 0 ? entry.Label : entry.Filename },
                    FileName = Path.GetFileName(file).Length > 0 ? Path.GetFileName(file) : entry.Filename,
                    Size = len > 0 ? FormatBytes((ulong)len) : "—",
                    Status = entry.DynamicSuper ? "META 免合并" : (exists ? "就绪" : "镜像缺失，跳过"),
                    ImagePath = file,
                };
                row.IsSelected = exists;
                _rows.Add(row);
            }
            UpdateEmptyState();
        }

        LogInfo($"{category} 资料已载入：{plan.XmlPaths.Count} 个 XML，{plan.Entries.Count} 个可刷写条目");
        foreach (var w in plan.Warnings) LogWarning(w);
    }

    private void TxtProgrammer_MouseDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "选择 Firehose Programmer",
            Filter = "Firehose Programmer (*.elf *.melf *.mbn *.bin)|*.elf;*.melf;*.mbn;*.bin|所有文件 (*.*)|*.*",
        };
        if (dlg.ShowDialog() != true) return;
        TxtProgrammer.Text = dlg.FileName;

        // 同目录若是 OPPO VIP 引导包，顺手识别（recursive:false —— 桌面/包根可能挂着
        // 几十万个后代文件，递归会把 UI 卡死；C++ 侧同样是"只查同级"）
        var dir = Path.GetDirectoryName(dlg.FileName) ?? "";
        if (dir.Length > 0
            && OplusVipSession.DiscoverPackage(dir, CurrentStorage(), out var package, out _,
                    recursive: false, preferredProgrammerPath: dlg.FileName)
            && package.Enabled)
        {
            _vipPackage = package;
            TxtProgrammer.Text = package.ProgrammerPath;
            // 🔴 **不再自动勾上 OPPO VIP**（2026-10-09 逆向报告 §6 之后改）。
            // VIP 模式会走 OPPO 的授权通道（Digest → Verify → Sign → SHA-256），
            // 属于**带 OEM 授权文件**的维修流程，不该在"用户只是挑了个 programmer"时
            // 被静默打开 —— 打开与否必须是用户的一次明确点击。
            // 这里只做识别 + 提示，勾选权交回给用户。
            LogInfo($"同目录已识别为 OPPO VIP 引导包：{Path.GetFileName(package.ProgrammerPath)}"
                    + $"（{package.ProgrammerCandidates.Count} 个签名候选）。"
                    + "如需使用授权通道，请手动勾选「OPPO VIP」。");
        }
        else
        {
            _vipPackage = new OplusVipPackage();
        }
    }

    private void ChkVip_Changed(object sender, RoutedEventArgs e)
    {
        if (ChkVip.IsChecked != true) return;
        if (_busy || _connected) return;
        if (_vipPackage.Enabled) return;

        // 先试「已选文件所在目录」，识别不到再让用户手动挑目录
        var candidate = TxtProgrammer.Text.Length > 0
            ? Path.GetDirectoryName(TxtProgrammer.Text) ?? ""
            : (_selectedXmlPaths.Count > 0 ? Path.GetDirectoryName(_selectedXmlPaths[0]) ?? "" : "");
        if (candidate.Length > 0 && LoadVipPackage(showDialog: false)) return;

        var dlg = new Microsoft.Win32.OpenFolderDialog { Title = "选择 OPPO VIP 引导包目录" };
        if (dlg.ShowDialog() != true) return;
        if (!OplusVipSession.DiscoverPackage(dlg.FolderName, CurrentStorage(),
                out var package, out var err) || !package.Enabled)
        {
            ShowError(string.IsNullOrEmpty(err) ? "该目录不是有效的 OPPO VIP 引导包。" : err);
            return;
        }
        _vipPackage = package;
        TxtProgrammer.Text = package.ProgrammerPath;
        LogInfo($"VIP 引导包已识别：{Path.GetFileName(package.ProgrammerPath)}"
                + $"（{package.ProgrammerCandidates.Count} 个同载荷签名候选）");
    }

    private bool LoadVipPackage(bool showDialog)
    {
        var dir = Path.GetDirectoryName(TxtProgrammer.Text) ?? "";
        if (dir.Length == 0) return false;
        if (!OplusVipSession.DiscoverPackage(dir, CurrentStorage(), out var package, out var error)
            || !package.Enabled)
        {
            _vipPackage = new OplusVipPackage();
            if (showDialog) ShowError(string.IsNullOrEmpty(error) ? "不是有效的 OPPO VIP 引导包。" : error);
            else LogWarning($"VIP 引导包识别：{error}");
            return false;
        }
        _vipPackage = package;
        TxtProgrammer.Text = package.ProgrammerPath;
        return true;
    }

    // =====================================================================
    // 主操作
    // =====================================================================
    private void BtnReadGpt_Click(object sender, RoutedEventArgs e)
    {
        if (_connected)
        {
            SetBusy(true);
            RunFlashTask("读取分区表", () => _worker.ReadPartitionTable());
        }
        else
        {
            BtnConnect_Click(sender, e);
        }
    }

    private void BtnReadVersion_Click(object sender, RoutedEventArgs e)
    {
        if (!_connected)
        {
            _readVersionAfterGpt = true;
            BtnConnect_Click(sender, e);
        }
        else if (_rows.Count == 0)
        {
            _readVersionAfterGpt = true;
            SetBusy(true);
            RunFlashTask("读取分区表", () => _worker.ReadPartitionTable());
        }
        else
        {
            SetBusy(true);
            RunFlashTask("读取系统版本", () => _worker.ReadSystemVersionInfo());
        }
    }

    private void BtnBackup_Click(object sender, RoutedEventArgs e)
    {
        var parts = CheckedPartitions();
        if (parts.Count == 0) { ShowError("请先勾选需要读取的分区。"); return; }

        var dlg = new Microsoft.Win32.OpenFolderDialog { Title = "选择回读文件夹的保存位置" };
        if (dlg.ShowDialog() != true) return;

        var baseName = $"QST-回读-{DateTime.Now:yyyyMMddHHmmss}";
        var folder = Path.Combine(dlg.FolderName, baseName);
        for (var i = 1; Directory.Exists(folder); i++)
            folder = Path.Combine(dlg.FolderName, $"{baseName}-{i:00}");
        try { Directory.CreateDirectory(folder); }
        catch (Exception ex) { ShowError($"创建回读文件夹失败：{ex.Message}"); return; }

        LogInfo($"回读保存目录：{folder}");
        SetBusy(true);
        var generateXml = ChkGenerateXml.IsChecked == true;
        RunFlashTask("回读分区", () => _worker.BackupPartitions(parts, folder, generateXml));
    }

    private void BtnErase_Click(object sender, RoutedEventArgs e)
    {
        var parts = CheckedPartitions();
        if (parts.Count == 0) { ShowError("请先勾选需要擦除的分区。"); return; }

        if (!ApplyProtectionPolicy(parts, null, "擦除", out var allowProtected))
        {
            LogInfo("擦除任务中仅包含保护分区，本次任务已结束");
            return;
        }

        var bps = BytesPerSector();
        var total = parts.Aggregate(0UL, (acc, p) => SafeAdd(acc, SafeMul(p.SectorCount, bps)));
        var names = string.Join("\n", parts.Select(p => $"{p.Name} (LUN {p.Lun})"));
        if (MessageBox.Show($"即将擦除 {parts.Count} 个分区，共 {FormatBytes(total)}：\n\n{names}",
                "确认擦除", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No)
            != MessageBoxResult.Yes) return;

        SetBusy(true);
        RunFlashTask("擦除分区", () => _worker.ErasePartitions(parts, allowProtected));
    }

    private void BtnFlash_Click(object sender, RoutedEventArgs e)
    {
        var checkedRows = _rows.Where(r => r.IsSelected).ToList();
        if (checkedRows.Count == 0) { FlashViaXml(); return; }
        FlashSelectedImages(checkedRows);
    }

    /// <summary>整包走 rawprogram XML（没勾任何分区时的默认路径）。</summary>
    private void FlashViaXml()
    {
        if (_selectedXmlPaths.Count == 0)
        {
            var dlg = new Microsoft.Win32.OpenFileDialog
            {
                Title = "选择 rawprogram XML（可多选）",
                Filter = "rawprogram XML (*.xml)|*.xml|所有文件 (*.*)|*.*",
                Multiselect = true,
            };
            if (dlg.ShowDialog() != true) return;
            PickXmlFiles(dlg.FileNames);
            if (_selectedXmlPaths.Count == 0) return;
        }

        var plan = new RawProgramPlan();
        if (!RawProgramParser.Parse(_selectedXmlPaths, plan, out var error)) { ShowError(error); return; }

        var dynamicStarts = plan.Entries.Count(e => !e.StartSectorResolved);
        var summary = $"已选择 {plan.XmlPaths.Count} 个 XML，共 {plan.Entries.Count} 个 program 条目。\n"
                    + $"镜像数据约 {FormatBytes(plan.TotalSourceBytes)}。\n"
                    + "将按 XML 原始顺序直接写入各 LUN 和起始扇区。";
        if (plan.HasSplitSuper())
            summary += $"\n\n检测到 {plan.SuperEntryCount} 个 super 条目，将逐段直刷，全程免合并。";
        if (dynamicStarts > 0)
            summary += $"\n\n有 {dynamicStarts} 个 NUM_DISK_SECTORS 条目，将按设备各 LUN 容量解析。";
        summary += "\n\n开始执行 XML 刷机？";

        if (MessageBox.Show(summary, "确认 XML 刷机", MessageBoxButton.YesNo,
                MessageBoxImage.Warning, MessageBoxResult.No) != MessageBoxResult.Yes) return;

        // 保护分区二次确认**走 label/filename 口径**（与分区路径分开 —— 这里面对的是 XML 条目）
        var allowProtected = ChkProtect.IsChecked != true;
        if (!allowProtected)
        {
            var guarded = plan.Entries
                .Where(en => PartitionProtection.IsProtectedPartitionMapping(en.Label, en.Filename))
                .Select(en => PartitionProtection.NormalizedPartitionName(
                    en.Label.Length > 0 ? en.Label : en.Filename))
                .Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            if (guarded.Count > 0)
            {
                var ok = MessageBox.Show(
                    $"XML 计划包含以下保护分区：\n\n{string.Join("、", guarded)}\n\n"
                    + "默认动作会跳过这些分区，并锁定对应物理范围。",
                    "XML 保护分区确认", MessageBoxButton.YesNo, MessageBoxImage.Warning,
                    MessageBoxResult.No) == MessageBoxResult.Yes;
                allowProtected = ok;
                LogInfo(ok
                    ? $"XML 保护分区二次确认已通过：{string.Join("、", guarded)}"
                    : $"XML 将跳过保护分区：{string.Join("、", guarded)}");
            }
        }

        SetBusy(true);
        var paths = plan.XmlPaths.ToList();
        RunFlashTask("XML 刷机", () => _worker.FlashXml(paths, allowProtected));
    }

    /// <summary>按勾选分区逐个刷写（每个分区一份独立镜像）。</summary>
    private void FlashSelectedImages(List<PartitionRow> checkedRows)
    {
        var partitions = new List<PartitionInfo>();
        var imagePaths = new List<string>();
        var missing = new List<string>();
        foreach (var row in checkedRows)
        {
            partitions.Add(row.Info);
            imagePaths.Add(row.ImagePath);
            if (row.ImagePath.Length == 0 || !File.Exists(row.ImagePath)) missing.Add(row.Name);
        }

        if (!ApplyProtectionPolicy(partitions, imagePaths, "写入", out var allowProtected))
        {
            LogInfo("写入任务中仅包含保护分区，本次任务已结束");
            return;
        }

        if (missing.Count > 0)
        {
            foreach (var r in _rows) RefreshRowStatus(r);
            ShowError($"以下已勾选分区还没有指定镜像：\n{string.Join("、", missing)}\n\n"
                      + "请双击对应分区行选择镜像，然后再次点击写入分区。");
            return;
        }

        var mapping = string.Join("\n",
            partitions.Select((p, i) => $"{Path.GetFileName(imagePaths[i])}  →  {p.Name}"));
        if (MessageBox.Show($"即将执行 {partitions.Count} 个分区写入：\n\n{mapping}\n\n该操作会覆盖原有数据。",
                "确认写入", MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No)
            != MessageBoxResult.Yes) return;

        SetBusy(true);
        RunFlashTask("写入分区", () => _worker.FlashPartitions(partitions, imagePaths, allowProtected));
    }

    private void DgPartitions_MouseDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (_busy || !_connected) return;
        if (DgPartitions.SelectedItem is not PartitionRow row) return;

        if (!row.IsSelected)
        {
            ShowError("请先勾选该分区，再双击选择刷写镜像。");
            return;
        }

        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = $"为分区 {row.Name} 选择镜像",
            Filter = "磁盘镜像 (*.img *.bin *.raw)|*.img;*.bin;*.raw|所有文件 (*.*)|*.*",
            InitialDirectory = row.ImagePath.Length > 0 ? Path.GetDirectoryName(row.ImagePath) ?? "" : "",
        };
        if (dlg.ShowDialog() != true) return;

        // 容量校验：sparse 要按**展开后**的大小算，否则会放行一个其实塞不下的包
        ulong payload;
        try
        {
            using var fs = File.OpenRead(dlg.FileName);
            if (fs.Length <= 0) { ShowError("镜像文件为空。"); return; }
            payload = (ulong)fs.Length;
            if (SparseImage.HasSparseMagic(fs))
            {
                if (!SparseImage.Inspect(fs, out var info, out var sparseError))
                {
                    ShowError(sparseError);
                    return;
                }
                payload = info.ExpandedSize;
            }
        }
        catch (Exception ex) { ShowError($"镜像文件读取失败：{ex.Message}"); return; }

        var capacity = SafeMul(row.Info.SectorCount, BytesPerSector());
        if (payload > capacity)
        {
            ShowError($"{Path.GetFileName(dlg.FileName)} 展开后为 {FormatBytes(payload)}，"
                      + $"超过分区 {row.Name} 的容量 {FormatBytes(capacity)}。");
            return;
        }

        row.ImagePath = dlg.FileName;
        row.FileName = Path.GetFileName(dlg.FileName);
        RefreshRowStatus(row);
        DgPartitions.SelectedItem = row;
        LogInfo($"分区镜像已选择：{row.FileName} → {row.Name}（{FormatBytes(payload)}）");
    }

    // =====================================================================
    // 重启 / 停止
    // =====================================================================
    private void BtnReboot_Click(object sender, RoutedEventArgs e)
    {
        if (BtnReboot.ContextMenu == null) return;
        BtnReboot.ContextMenu.PlacementTarget = BtnReboot;
        BtnReboot.ContextMenu.IsOpen = true;
    }

    private void MenuRebootSystem_Click(object sender, RoutedEventArgs e) => ConfirmReboot(false);
    private void MenuRebootEdl_Click(object sender, RoutedEventArgs e) => ConfirmReboot(true);

    private void ConfirmReboot(bool toEdl)
    {
        if (!_connected) return;
        var dest = toEdl ? "9008 模式" : "系统";
        if (MessageBox.Show($"确认通过 Firehose 将设备重启到{dest}？", $"重启到{dest}",
                MessageBoxButton.YesNo, MessageBoxImage.Question, MessageBoxResult.No)
            != MessageBoxResult.Yes) return;

        SetBusy(true);
        LogInfo($"正在发送重启到{dest}命令…");
        RunFlashTask("重启设备", () => _worker.RebootDevice(toEdl));
    }

    private void BtnStop_Click(object sender, RoutedEventArgs e)
    {
        LogWarning("正在停止当前任务…");
        try { _worker.RequestStop(); } catch { }
    }

    private void BtnClearLog_Click(object sender, RoutedEventArgs e)
    {
        _logHistory.Clear();
        TxtLog.Document.Blocks.Clear();
    }

    private void ChkDebugLog_Changed(object sender, RoutedEventArgs e)
    {
        _debugLogEnabled = ChkDebugLog.IsChecked == true;
        ChkDebugLog.Content = _debugLogEnabled ? "Debug：完整" : "Debug";
        RebuildLogView();
    }

    // =====================================================================
    // 引擎事件（都在**后台线程**触发 —— 必须切回 UI）
    // =====================================================================
    private void OnLog(string message) => Dispatcher.BeginInvoke(new Action(() => AppendLog(message)));
    private void OnStateChanged(SessionState state, string detail)
        => Dispatcher.BeginInvoke(new Action(() => ApplyState(state, detail)));
    private void OnProgress(ulong completed, ulong total)
        => Dispatcher.BeginInvoke(new Action(() => ApplyProgress(completed, total)));
    private void OnOverallProgress(ulong completed, ulong total)
        => Dispatcher.BeginInvoke(new Action(() => ApplyOverallProgress(completed, total)));
    private void OnDeviceInfo(DeviceInfo info)
        => Dispatcher.BeginInvoke(new Action(() => ApplyDeviceInfo(info)));
    private void OnOperationFinished(bool success, string message)
        => Dispatcher.BeginInvoke(new Action(() => ApplyOperationFinished(success, message)));

    private void ApplyState(SessionState state, string detail)
    {
        if (state == SessionState.Ready) _connected = true;
        if (state is SessionState.Disconnected or SessionState.Failed) _connected = false;

        BtnConnect.Content = _connected ? "断开连接" : "查找分区";
        if (!string.IsNullOrEmpty(detail)) TxtStateBanner.Text = detail;
        TxtCurrentMode.Text = EdlTypes.StateText(state);
        _autoScan.Stop();
    }

    private void ApplyProgress(ulong completed, ulong total)
    {
        // 当前任务 / 当前分区进度 —— 上报外壳底部 ShellStatus 第二条进度条（不再画页内重复条）
        if (total == 0) { ShellStatus.Report(partitionPercent: 0); return; }
        var ratio = Math.Min(1.0, (double)completed / total);
        ShellStatus.Report(partitionPercent: ratio * 100.0);
    }

    private void ApplyOverallProgress(ulong completed, ulong total)
    {
        // 速率：起步/换任务时清零；总量 ≥64KiB 后每 500ms（或收尾）出一次平滑值
        if (total == 0)
        {
            ResetSpeed();
            TxtSpeed.Text = "速率  0.00 MB/s";
            ShellStatus.Report(percent: 0, speed: "—");
            return;
        }

        string speedText;
        var transferChanged = _speedStamp == DateTime.MinValue || total != _speedTotal
                              || completed < _speedLastCompleted;
        if (transferChanged)
        {
            ResetSpeed();
            _speedStamp = DateTime.Now;
            _speedLastCompleted = completed;
            _speedTotal = total;
            speedText = "速率  0.00 MB/s";
        }
        else if (total >= 64UL * 1024UL)
        {
            var elapsedMs = (DateTime.Now - _speedStamp).TotalMilliseconds;
            if (elapsedMs - _speedLastUpdateMs >= 500 || completed == total)
            {
                var transferred = completed - _speedLastCompleted;
                if (elapsedMs > 0 && transferred > 0)
                {
                    var average = transferred * 1000.0 / elapsedMs;
                    _smoothedBps = _smoothedBps <= 0 ? average : _smoothedBps * 0.80 + average * 0.20;
                }
                _speedLastUpdateMs = elapsedMs;
            }
            speedText = $"速率  {_smoothedBps / (1024.0 * 1024.0):F2} MB/s";
        }
        else
        {
            speedText = "速率  —";
        }

        TxtSpeed.Text = speedText;
        var ratio = Math.Min(1.0, (double)completed / total);
        ShellStatus.Report(percent: ratio * 100.0, speed: _smoothedBps > 0
            ? $"{_smoothedBps / (1024.0 * 1024.0):F2} MB/s" : "—");
    }

    private void ResetSpeed()
    {
        _speedStamp = DateTime.MinValue;
        _speedLastCompleted = 0;
        _speedTotal = 0;
        _speedLastUpdateMs = 0;
        _smoothedBps = 0;
    }

    private void ApplyDeviceInfo(DeviceInfo info)
    {
        var brand = info.Brand.Length > 0
            ? info.Brand
            : (info.VipMode.Contains("OPPO", StringComparison.OrdinalIgnoreCase) ? "OPLUS" : "Qualcomm");
        TxtBrand.Text = info.PortName.Length == 0 ? "—" : brand;

        // 芯片：先从型号里抠 SM/MSM/SDM/APQ + 数字，抠不到再退 MSM ID
        var chip = info.Model ?? "";
        var m = Regex.Match(chip, @"((?:SM|MSM|SDM|APQ)[0-9]+)", RegexOptions.IgnoreCase);
        if (m.Success) chip = m.Groups[1].Value.ToUpperInvariant();
        else if (chip.Length == 0 && info.MsmId.Length > 0) chip = info.MsmId.ToUpperInvariant();
        TxtChip.Text = chip.Length == 0 ? "—" : chip;

        var identity = info.Serial.Length > 0 ? info.Serial : info.Hwid;
        TxtCpuId.Text = identity.Length == 0 ? "—" : identity;

        TxtUserSize.Text = info.StorageBytes > 0
            ? $"{info.StorageBytes / (1024.0 * 1024.0 * 1024.0):F2} GB"
            : "—";
        TxtStorageModel.Text = info.StorageProduct.Length == 0 ? "—" : info.StorageProduct;
        TxtDeviceName.Text = info.ProductName.Length == 0 ? "—" : info.ProductName;
        TxtAndroid.Text = info.AndroidVersion.Length == 0 ? "—" : info.AndroidVersion;
        TxtSysVersion.Text = info.SystemVersion.Length == 0 ? "—" : info.SystemVersion;
        TxtSysVersion.ToolTip = info.BuildFingerprint;
        TxtSecPatch.Text = info.SecurityPatch.Length == 0 ? "—" : info.SecurityPatch;
    }

    private void ApplyOperationFinished(bool success, string message)
    {
        var startAutoGptRead = success && _readGptAfterConnect && _connected;
        var startVersionRead = success && _connected
            && message.StartsWith("分区表读取完成", StringComparison.Ordinal)
            && (_readVersionAfterGpt || ChkReadInfo.IsChecked == true);
        var completedDataIo =
            message.StartsWith("批量读取完成", StringComparison.Ordinal)
            || (message.StartsWith("分区 ", StringComparison.Ordinal)
                && (message.Contains("已备份") || message.EndsWith("写入完成", StringComparison.Ordinal)))
            || message.StartsWith("批量写入完成", StringComparison.Ordinal)
            || message.StartsWith("XML 刷机完成", StringComparison.Ordinal);
        var autoReboot = success && _connected && ChkAutoReboot.IsChecked == true && completedDataIo;

        if (!success || !_connected)
        {
            _readGptAfterConnect = false;
            _readVersionAfterGpt = false;
        }

        SetBusy(false);
        ShellStatus.EndTask();

        if (!success && !message.Contains("停止")) ShowError(message);
        else if (success) ShowSuccess(message);

        if (startAutoGptRead)
        {
            _readGptAfterConnect = false;
            LogInfo("连接完成，自动读取 GPT 分区表…");
            SetBusy(true);
            RunFlashTask("读取分区表", () => _worker.ReadPartitionTable());
        }
        else if (startVersionRead)
        {
            _readVersionAfterGpt = false;
            LogInfo("分区表已就绪，自动读取系统版本信息…");
            SetBusy(true);
            RunFlashTask("读取系统版本", () => _worker.ReadSystemVersionInfo());
        }
        else if (autoReboot)
        {
            LogInfo("读写任务完成，按设置自动重启到系统…");
            SetBusy(true);
            RunFlashTask("重启设备", () => _worker.RebootDevice(false));
        }
    }

    /// <summary>
    /// 把一次引擎调用丢到线程池。**必须这么调** —— 引擎是同步的，
    /// 直接在 UI 线程调会让界面一直假死到操作结束。
    /// </summary>
    private void RunWorker(Action action)
    {
        _ = Task.Run(() =>
        {
            try { action(); }
            catch (Exception ex)
            {
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    SetBusy(false);
                    LogError($"操作异常：{ex.Message}");
                    ShowError(ex.Message);
                }));
            }
        });
    }

    /// <summary>
    /// 数据类操作（读/写/擦除/回读/刷机/重启）统一在此开始/结束外壳进度。
    /// 进度条只画在外壳底部 ShellStatus，避免与页内重复。
    /// </summary>
    private void RunFlashTask(string taskName, Action action)
    {
        ShellStatus.BeginTask(taskName);
        RunWorker(action);
    }

    // =====================================================================
    // 忙闲 / 控件可用性（对应 C++ 的 setBusy）
    // =====================================================================
    private void SetBusy(bool busy)
    {
        if (busy && !_busy) ResetSpeed();
        _busy = busy;

        var editable = !busy && !_connected;
        CmbPort.IsEnabled = editable;
        CmbStorage.IsEnabled = editable;
        CmbMode.IsEnabled = editable;
        CmbChipset.IsEnabled = editable;
        TxtProgrammer.IsEnabled = editable;
        ChkAutoScan.IsEnabled = editable;
        ChkVip.IsEnabled = editable;

        TxtXmlPath.IsEnabled = !busy;
        ChkAutoReboot.IsEnabled = !busy;
        ChkGenerateXml.IsEnabled = !busy;
        ChkProtect.IsEnabled = !busy;
        ChkReadInfo.IsEnabled = !busy;
        BtnConnect.IsEnabled = !busy;

        var ready = _connected && !busy;
        var hasPort = CurrentPortName().Length > 0;
        BtnReadGpt.IsEnabled = !busy && (_connected || hasPort);
        BtnReadVersion.IsEnabled = !busy && (_connected || hasPort);
        BtnBackup.IsEnabled = ready && _rows.Count > 0;
        BtnErase.IsEnabled = ready && _rows.Count > 0;
        BtnFlash.IsEnabled = ready;
        BtnReboot.IsEnabled = ready;
        BtnStop.IsEnabled = busy;

        if (!busy && !_connected && ChkAutoScan.IsChecked == true) _autoScan.Start();
        else _autoScan.Stop();
    }

    // =====================================================================
    // 日志
    // =====================================================================
    private void AppendLog(string message)
    {
        if (_logHistory.Count >= MaxLogRecords) _logHistory.RemoveRange(0, 5000);
        var record = (DateTime.Now, message);
        _logHistory.Add(record);
        if (!ShouldShowLog(message)) return;
        AppendLogLine(record);
    }

    /// <summary>
    /// Debug 关闭时挡掉高频协议噪声。**记录仍然全部保留在 _logHistory** ——
    /// 打开 Debug 会立刻把历史补齐（对应 C++ shouldShowLog）。
    /// </summary>
    private bool ShouldShowLog(string m)
    {
        if (_debugLogEnabled) return true;
        if (m.StartsWith("Firehose: INFO:", StringComparison.Ordinal)) return false;
        if (m.StartsWith("发送 Firehose 命令：", StringComparison.Ordinal)) return false;
        if (m.StartsWith("VIP READ：", StringComparison.Ordinal)) return false;
        if (m.StartsWith("VIP PROGRAM：", StringComparison.Ordinal)) return false;
        if (m.StartsWith("VIP ERASE：", StringComparison.Ordinal)) return false;
        if (m.StartsWith("[Sahara][DEBUG]", StringComparison.Ordinal)) return false;
        if (m.StartsWith("Firehose: ERROR: operation 0 on", StringComparison.Ordinal)
            && m.Contains("is forbidden", StringComparison.OrdinalIgnoreCase)) return false;
        if (m.Contains("not get permission,line=", StringComparison.OrdinalIgnoreCase)) return false;
        if (m.Contains("begin boot log", StringComparison.OrdinalIgnoreCase)
            || m.Contains("end boot log", StringComparison.OrdinalIgnoreCase)
            || (m.StartsWith("Firehose: ERROR: B -", StringComparison.Ordinal)
                && m.Contains("usb: init start", StringComparison.OrdinalIgnoreCase))) return false;
        if (m.Contains("DevprgRSAVerify verify signature", StringComparison.Ordinal)) return false;
        if (m.Contains("SpmiBus_ReadLong", StringComparison.Ordinal)) return false;
        if (m.Contains("devprg_pm_charger_set", StringComparison.Ordinal)) return false;
        return true;
    }

    private void AppendLogLine((DateTime Time, string Text) record)
    {
        var para = new Paragraph(new Run(record.Time.ToString("HH:mm:ss") + "  ")
        {
            Foreground = FrozenBrush(_dark ? "#86A0B9" : "#64748B"),
        })
        { Margin = new Thickness(0) };
        para.Inlines.Add(new Run(record.Text) { Foreground = FrozenBrush(LogColor(record.Text)) });
        TxtLog.Document.Blocks.Add(para);
        TxtLog.ScrollToEnd();
    }

    private string LogColor(string m)
    {
        if (m.Contains("错误") || m.Contains("失败")) return _dark ? "#FF6B72" : "#C92F37";
        if (m.Contains("完成") || m.Contains("成功") || m.Contains("就绪")) return _dark ? "#37DB91" : "#138451";
        if (m.Contains("等待") || m.Contains("正在")) return _dark ? "#F4C95D" : "#A96C09";
        return _dark ? "#D8E1EB" : "#27364A";
    }

    private static SolidColorBrush FrozenBrush(string hex)
    {
        var b = new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
        b.Freeze();
        return b;
    }

    private void RebuildLogView()
    {
        TxtLog.Document.Blocks.Clear();
        foreach (var r in _logHistory)
            if (ShouldShowLog(r.Text)) AppendLogLine(r);
    }

    // =====================================================================
    // XML 名称工具（对应 C++ 匿名 namespace 里的同名函数）
    // =====================================================================
    private enum XmlKind { Other, Standard, Nonstandard }

    private static (XmlKind Kind, string Family, int Index) ParseXmlName(string fileName)
    {
        var m = Regex.Match(fileName, @"^(rawprogram)([0-9]+)\.xml$", RegexOptions.IgnoreCase);
        if (m.Success) return (XmlKind.Standard, m.Groups[1].Value.ToLowerInvariant(), int.Parse(m.Groups[2].Value));
        m = Regex.Match(fileName, @"^(rawprogram_.+?)([0-9]+)\.xml$", RegexOptions.IgnoreCase);
        if (m.Success) return (XmlKind.Nonstandard, m.Groups[1].Value.ToLowerInvariant(), int.Parse(m.Groups[2].Value));
        return (XmlKind.Other, "", -1);
    }

    /// <summary>
    /// 标准族（rawprogram*）与非标准族（rawprogram_unsparse*）是同一份固件的两种表述，
    /// 不能混刷。当展开后同时出现两族时，只保留**用户显式所选**的那一族；若用户两族都选了，
    /// 则保留文件数更多的一族。被丢弃的族名通过 <paramref name="droppedFamilies"/> 返回，供提示。
    /// </summary>
    private static List<string> ResolveSingleFamily(List<string> expanded, List<string> selected,
        out List<string> droppedFamilies, out string keptFamily)
    {
        droppedFamilies = new List<string>();
        keptFamily = "";
        var fams = expanded
            .Select(p => ParseXmlName(Path.GetFileName(p)).Family)
            .Where(f => f.Length > 0)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();
        if (fams.Count <= 1)
        {
            keptFamily = fams.Count == 1 ? fams[0] : "";
            return expanded;
        }

        var selFams = selected
            .Select(p => ParseXmlName(Path.GetFileName(p)).Family)
            .Where(f => f.Length > 0)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();
        string keep;
        if (selFams.Count == 1) keep = selFams[0];
        else keep = expanded
            .GroupBy(p => ParseXmlName(Path.GetFileName(p)).Family, StringComparer.OrdinalIgnoreCase)
            .OrderByDescending(g => g.Count()).First().Key;

        keptFamily = keep;
        droppedFamilies = fams
            .Where(f => !string.Equals(f, keep, StringComparison.OrdinalIgnoreCase))
            .ToList();
        return expanded
            .Where(p => string.Equals(ParseXmlName(Path.GetFileName(p)).Family, keep,
                StringComparison.OrdinalIgnoreCase))
            .ToList();
    }

    /// <summary>族名 → 面向用户的标签（对应 SWT-EDL 的「标准 / 非标准」表述）。</summary>
    private static string FamilyLabel(string family)
        => family.StartsWith("rawprogram_", StringComparison.OrdinalIgnoreCase)
            ? "非标准 XML（rawprogram_unsparse*）"
            : "标准 XML（rawprogram*）";

    private static string XmlCategoryName(List<string> paths)
    {
        foreach (var p in paths)
        {
            var k = ParseXmlName(Path.GetFileName(p)).Kind;
            if (k == XmlKind.Standard) return "标准 XML";
            if (k == XmlKind.Nonstandard) return "非标准 XML";
        }
        return "XML";
    }

    /// <summary>
    /// 选中一个 rawprogramN.xml 时，把**同目录同族**的兄弟 XML（rawprogram0/1/2…）一起带上，
    /// 按「族名 → 序号」排序。用户往往只选一个文件，但一个包的所有 LUN 必须一起刷。
    /// </summary>
    private static List<string> ExpandRawprogramXmlSelection(List<string> selectedPaths)
    {
        var paths = new List<string>();
        foreach (var selected in selectedPaths)
        {
            var abs = Path.GetFullPath(selected);
            if (!paths.Contains(abs, StringComparer.OrdinalIgnoreCase)) paths.Add(abs);

            var name = ParseXmlName(Path.GetFileName(abs));
            if (name.Kind == XmlKind.Other) continue;
            var dir = Path.GetDirectoryName(abs);
            if (string.IsNullOrEmpty(dir)) continue;

            IEnumerable<string> siblings;
            try { siblings = Directory.GetFiles(dir, "rawprogram*.xml"); }
            catch { continue; }

            foreach (var sib in siblings)
            {
                var sn = ParseXmlName(Path.GetFileName(sib));
                if (sn.Kind != name.Kind) continue;
                if (!string.Equals(sn.Family, name.Family, StringComparison.OrdinalIgnoreCase)) continue;
                if (!paths.Contains(sib, StringComparer.OrdinalIgnoreCase)) paths.Add(sib);
            }
        }

        return paths
            .OrderBy(p => ParseXmlName(Path.GetFileName(p)).Family, StringComparer.OrdinalIgnoreCase)
            .ThenBy(p => ParseXmlName(Path.GetFileName(p)).Index)
            .ThenBy(p => p, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    private List<PartitionInfo> CheckedPartitions()
        => _rows.Where(r => r.IsSelected).Select(r => r.Info).ToList();

    /// <summary>字节格式化（B / KiB / MiB / GiB / TiB）。</summary>
    private static string FormatBytes(ulong bytes)
    {
        string[] units = { "B", "KiB", "MiB", "GiB", "TiB" };
        double v = bytes;
        var u = 0;
        while (v >= 1024.0 && u + 1 < units.Length) { v /= 1024.0; u++; }
        return u == 0 ? $"{v:F0} {units[u]}" : $"{v:F2} {units[u]}";
    }
}
