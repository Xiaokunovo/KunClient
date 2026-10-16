using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;
using KunFlashTool.App.Services;
using Microsoft.Win32;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// 引导程序选择器。
    /// <para>
    /// 为什么不直接用 OpenFileDialog：真实的高通 firehose 引导文件**没有扩展名**
    /// （<c>firehose\mi\865</c>、<c>firehose\oppo\450</c> …），Windows 的文件过滤器
    /// 根本没有「无扩展名」这种通配符，只能靠 <c>*.*</c> 兜底 —— 于是整个目录里
    /// 上百个无关文件全糊在列表里，用户根本挑不出来。
    /// </para>
    /// <para>
    /// 这里改成自己扫目录 + 扩展名黑名单：无扩展名的文件反而是重点保留对象，
    /// .txt/.json/.7z/.png 这类一律先藏起来，并给出「已隐藏 N 个」的提示。
    /// </para>
    /// </summary>
    public partial class LoaderPickerDialog : Window
    {
        /// <summary>这些扩展名一定不是引导程序，默认全部隐藏。</summary>
        private static readonly HashSet<string> UselessExtensions = new(StringComparer.OrdinalIgnoreCase)
        {
            // 文本 / 配置 / 日志
            ".txt", ".md", ".rtf", ".json", ".ini", ".cfg", ".conf", ".log", ".xml", ".csv", ".tsv",
            ".yaml", ".yml", ".properties",
            // 文档
            ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
            // 图片
            ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".ico", ".svg", ".tif", ".tiff", ".heic",
            // 程序 / 库
            ".dll", ".exe", ".msi", ".sys", ".ocx", ".so", ".dylib", ".pdb", ".lib", ".exp", ".jar", ".class",
            // 脚本
            ".bat", ".cmd", ".ps1", ".psm1", ".vbs", ".sh", ".py", ".pyc", ".js", ".mjs", ".ts",
            ".html", ".htm", ".css", ".java", ".kt", ".c", ".h", ".cpp", ".cs", ".go", ".rs", ".php",
            // 压缩包 / 镜像
            ".zip", ".rar", ".7z", ".tar", ".gz", ".bz2", ".xz", ".zst", ".iso", ".apk", ".deb", ".rpm",
            // 杂项
            ".lnk", ".url", ".db", ".sqlite", ".sqlite3", ".tmp", ".bak", ".old", ".part", ".crdownload",
        };

        private static readonly HashSet<string> UselessFileNames = new(StringComparer.OrdinalIgnoreCase)
        {
            "desktop.ini", "thumbs.db", ".ds_store", ".gitignore", ".gitattributes",
        };

        private const int MaxFiles = 4000;

        private readonly ObservableCollection<LoaderCandidate> _all = new();
        private readonly ObservableCollection<LoaderCandidate> _view = new();
        private string _root = "";
        private string? _preselect;
        private bool _filtering;

        public sealed class LoaderCandidate
        {
            public string FullPath { get; init; } = "";
            public string Relative { get; init; } = "";
            public string Name { get; init; } = "";
            public long Size { get; init; }
            public string Ext { get; init; } = "";
            public bool Hidden { get; init; }

            public string SizeText => FormatSize(Size);
            /// <summary>无扩展名在 firehose 目录里恰恰就是引导程序本体，单独标出来。</summary>
            public string KindText => string.IsNullOrEmpty(Ext) ? "引导程序" : Ext.TrimStart('.').ToUpperInvariant();
        }

        /// <summary>已选中的完整路径；用户点「取消」时为 null。</summary>
        public string? SelectedPath { get; private set; }

        public LoaderPickerDialog(string? current = null, Window? owner = null)
        {
            InitializeComponent();
            if (owner != null) Owner = owner;

            _preselect = current;
            TxtSearch.Text = "搜索：品牌 / 机型（如 mi、865、redmagic）";
            TxtSearch.Foreground = (System.Windows.Media.Brush)FindResource("Theme.TextTertiary");
            DgLoaders.ItemsSource = _view;

            Loaded += OnDialogLoaded;
        }

        /// <summary>弹窗选文件；用户取消返回 null。</summary>
        public static string? Pick(Window? owner, string? current)
        {
            var dialog = new LoaderPickerDialog(current);
            if (owner != null) dialog.Owner = owner;
            return dialog.ShowDialog() == true ? dialog.SelectedPath : null;
        }

        private async void OnDialogLoaded(object sender, RoutedEventArgs e)
        {
            _root = ResolveInitialRoot(_preselect);
            SetRootText();
            await RescanAsync();
        }

        /// <summary>
        /// 初始目录优先级：调用方给的当前文件所在目录 → 上次用过的目录 → 常见 firehose 目录
        /// （OneToolBox 工具包、程序目录下的 firehose / loaders）→ 下载目录。
        /// </summary>
        private static string ResolveInitialRoot(string? current)
        {
            if (!string.IsNullOrWhiteSpace(current))
            {
                var dir = Path.GetDirectoryName(current);
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) return dir;
            }

            var last = AppSettings.Current.LastLoaderDir;
            if (!string.IsNullOrWhiteSpace(last) && Directory.Exists(last)) return last;

            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            var exeDir = AppContext.BaseDirectory;

            var candidates = new[]
            {
                Path.Combine(home, "Downloads", "OneToolBox", "tool", "edl", "firehose"),
                Path.Combine(home, "Downloads", "OneToolBox", "tool", "edl"),
                Path.Combine(exeDir, "firehose"),
                Path.Combine(exeDir, "loaders"),
                Path.Combine(home, "Downloads"),
            };

            foreach (var dir in candidates)
                if (Directory.Exists(dir)) return dir;

            return Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
        }

        private void SetRootText() => TxtRoot.Text = _root;

        // ───────────────────────── 扫描 ─────────────────────────

        private async Task RescanAsync()
        {
            var root = _root;
            TxtFilterInfo.Text = "正在扫描…";
            TxtHint.Text = "正在扫描目录…";

            var found = await Task.Run(() => ScanDirectory(root));

            _all.Clear();
            foreach (var item in found.OrderBy(x => x.Relative, StringComparer.OrdinalIgnoreCase))
                _all.Add(item);

            ApplyFilter();
        }

        /// <summary>递归收集候选文件；目录不可读就跳过，文件数超上限即止。</summary>
        private static List<LoaderCandidate> ScanDirectory(string root)
        {
            var list = new List<LoaderCandidate>();
            if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root)) return list;

            var stack = new Stack<string>();
            stack.Push(root);

            while (stack.Count > 0)
            {
                var dir = stack.Pop();
                string[] files;
                try { files = Directory.GetFiles(dir); }
                catch { continue; }

                foreach (var file in files)
                {
                    var name = Path.GetFileName(file);
                    if (UselessFileNames.Contains(name)) continue;

                    var ext = Path.GetExtension(file);
                    var useless = UselessExtensions.Contains(ext);

                    string relative;
                    try { relative = Path.GetRelativePath(root, file); }
                    catch { relative = name; }

                    long size = 0;
                    try { size = new FileInfo(file).Length; } catch { }

                    list.Add(new LoaderCandidate
                    {
                        FullPath = file,
                        Relative = relative,
                        Name = name,
                        Size = size,
                        Ext = ext,
                        Hidden = useless,
                    });

                    if (list.Count >= MaxFiles) return list;
                }

                try
                {
                    foreach (var sub in Directory.GetDirectories(dir)) stack.Push(sub);
                }
                catch { }
            }

            return list;
        }

        // ───────────────────────── 过滤 ─────────────────────────

        private void ApplyFilter()
        {
            _filtering = true;
            try
            {
                var showAll = ChkShowAll.IsChecked == true;
                var keyword = GetSearchKeyword();

                // 🔴 填表期间先把表格和 _view 摘开：_view 是 ObservableCollection，绑定状态下
                //    每 Add 一格 DataGrid 就插一行容器并当场重排 —— 扫描出上千个候选文件时是 O(n²)。
                //    摘开 → 整轮填完再挂回去 = 只发一次 Reset、只排一次版。
                DgLoaders.ItemsSource = null;
                _view.Clear();
                var hiddenCount = 0;
                foreach (var item in _all)
                {
                    if (!showAll && item.Hidden) { hiddenCount++; continue; }
                    if (keyword.Length > 0 &&
                        item.Relative.IndexOf(keyword, StringComparison.OrdinalIgnoreCase) < 0)
                        continue;
                    _view.Add(item);
                }
                DgLoaders.ItemsSource = _view;

                var total = _all.Count;
                TxtFilterInfo.Text = showAll
                    ? $"共 {total} 个文件（未过滤）"
                    : $"可选中 {_view.Count} 个 · 已隐藏 {hiddenCount} 个无关文件";

                TxtHint.Text = _view.Count == 0
                    ? "没有可选的引导文件，试试换个目录，或勾选「显示全部文件」"
                    : "双击一行即可加载";
                TxtSubtitle.Text = showAll ? "显示全部文件" : "已自动滤掉无关文件";

                PreselectIfPossible();
            }
            finally { _filtering = false; }
        }

        private string GetSearchKeyword()
        {
            var text = TxtSearch.Text ?? "";
            // 占位提示文字不算关键词
            return text == PlaceholderSearch ? "" : text.Trim();
        }

        private const string PlaceholderSearch = "搜索：品牌 / 机型（如 mi、865、redmagic）";

        private void PreselectIfPossible()
        {
            if (string.IsNullOrWhiteSpace(_preselect)) return;
            var hit = _view.FirstOrDefault(x =>
                string.Equals(x.FullPath, _preselect, StringComparison.OrdinalIgnoreCase));
            if (hit != null)
            {
                DgLoaders.SelectedItem = hit;
                DgLoaders.ScrollIntoView(hit);
            }
        }

        // ───────────────────────── 事件 ─────────────────────────

        private void TxtSearch_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
        {
            if (_filtering) return;
            if (!IsLoaded) return;
            ApplyFilter();
        }

        private void TxtSearch_GotFocus(object sender, RoutedEventArgs e)
        {
            ClearPlaceholder();
        }

        private void TxtSearch_LostFocus(object sender, RoutedEventArgs e)
        {
            if (string.IsNullOrWhiteSpace(TxtSearch.Text)) ShowPlaceholder();
        }

        private void ClearPlaceholder()
        {
            if (TxtSearch.Text == PlaceholderSearch)
            {
                TxtSearch.Text = "";
                TxtSearch.Foreground = (System.Windows.Media.Brush)FindResource("Theme.TextPrimary");
            }
        }

        private void ShowPlaceholder()
        {
            TxtSearch.Text = PlaceholderSearch;
            TxtSearch.Foreground = (System.Windows.Media.Brush)FindResource("Theme.TextTertiary");
        }

        private void ChkShowAll_Click(object sender, RoutedEventArgs e)
        {
            if (!IsLoaded) return;
            ApplyFilter();
        }

        private async void BtnChangeRoot_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFolderDialog { Title = "选择引导文件所在目录" };
            try { dialog.InitialDirectory = _root; } catch { }
            if (dialog.ShowDialog(this) != true) return;

            _root = dialog.FolderName;
            SetRootText();
            await RescanAsync();
        }

        private void BtnBrowseAny_Click(object sender, RoutedEventArgs e)
        {
            // 兜底出口：过滤规则万一误伤，这里可以选任意文件
            var dialog = new OpenFileDialog
            {
                Title = "选择引导程序",
                Filter = "引导程序 (*.mbn;*.bin;*.elf;*.img)|*.mbn;*.bin;*.elf;*.img|所有文件 (*.*)|*.*",
                FilterIndex = 2,
            };
            try { dialog.InitialDirectory = _root; } catch { }
            if (dialog.ShowDialog(this) == true)
            {
                SelectedPath = dialog.FileName;
                DialogResult = true;
            }
        }

        private void DgLoaders_MouseDoubleClick(object sender, MouseButtonEventArgs e) => Confirm();

        private void BtnConfirm_Click(object sender, RoutedEventArgs e) => Confirm();

        private void Confirm()
        {
            if (DgLoaders.SelectedItem is not LoaderCandidate item)
            {
                TxtHint.Text = "请先在列表里选中一个文件";
                return;
            }

            // 记住这个目录，下次打开直接落在这里
            try
            {
                var dir = Path.GetDirectoryName(item.FullPath);
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
                {
                    AppSettings.Current.LastLoaderDir = dir;
                    AppSettings.Current.Save();
                }
            }
            catch { }

            SelectedPath = item.FullPath;
            DialogResult = true;
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = false;
        }

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ButtonState == MouseButtonState.Pressed) DragMove();
        }

        internal static string FormatSize(long bytes)
        {
            if (bytes <= 0) return "-";
            if (bytes < 1024) return bytes + " B";
            if (bytes < 1024 * 1024) return (bytes / 1024.0).ToString("F0") + " KB";
            return (bytes / 1024.0 / 1024.0).ToString("F2") + " MB";
        }
    }
}
