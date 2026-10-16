using System;
using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// 云端背景选择器：清单来自服务端 <c>/api/wallpapers</c>（控制台里由用户自己上传）。
    /// 选中后下载到 <c>%LOCALAPPDATA%\KunClient\wallpapers\</c> 再交给调用方设为背景 ——
    /// 这个目录属于「本地数据」，App.OnExit 会整目录删除，所以不会在用户机器上留底。
    /// </summary>
    public partial class CloudWallpaperPickerDialog : Window
    {
        // D14（同 D1）：清单是 JSON、图片可被 CF 压缩 —— 统一走带 AutomaticDecompression 的共享客户端
        //（不发 Accept-Encoding 就永远拿不到 CF 的压缩收益），连接池参数也一并复用。
        private static readonly HttpClient Http =
            BackgroundDownloader.CreateSharedClient(TimeSpan.FromSeconds(30));
        private static string ServerUrl => AppConfig.ServerUrl;

        private readonly ObservableCollection<WallpaperItem> _items = new();

        public sealed class WallpaperItem
        {
            public string Name { get; init; } = "";
            public long Size { get; init; }
            public string Url { get; init; } = "";
            public string UpdatedAt { get; init; } = "";
            public string SizeText => FormatSize(Size);
            public string UpdatedText =>
                DateTime.TryParse(UpdatedAt, CultureInfo.InvariantCulture,
                    DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out var dt)
                    ? dt.ToLocalTime().ToString("yyyy-MM-dd HH:mm")
                    : "-";
        }

        /// <summary>下载成功后落在本机的完整路径；取消或失败为 null。</summary>
        public string? LocalPath { get; private set; }

        public CloudWallpaperPickerDialog(Window? owner = null)
        {
            InitializeComponent();
            if (owner != null) Owner = owner;
            DgWallpapers.ItemsSource = _items;
            Loaded += async (_, __) => await LoadListAsync();
        }

        /// <summary>弹出选择器；返回下载到本地的路径，取消为 null。</summary>
        public static string? Pick(Window? owner)
        {
            var dialog = new CloudWallpaperPickerDialog(owner);
            return dialog.ShowDialog() == true ? dialog.LocalPath : null;
        }

        private async Task LoadListAsync()
        {
            SetEmpty("正在从服务器获取云端背景…");
            BtnConfirm.IsEnabled = false;
            _items.Clear();

            try
            {
                var json = await Http.GetStringAsync($"{ServerUrl}/api/v2/wallpapers");
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;

                if (!root.TryGetProperty("success", out var ok) || ok.ValueKind != JsonValueKind.True)
                {
                    var msg = root.TryGetProperty("message", out var m) ? m.GetString() : null;
                    SetEmpty("获取失败：" + (msg ?? "服务器返回失败"));
                    return;
                }

                if (root.TryGetProperty("data", out var arr) && arr.ValueKind == JsonValueKind.Array)
                {
                    foreach (var el in arr.EnumerateArray())
                    {
                        var name = el.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "";
                        if (string.IsNullOrEmpty(name)) continue;
                        _items.Add(new WallpaperItem
                        {
                            Name = name,
                            Size = el.TryGetProperty("size", out var s) && s.TryGetInt64(out var sz) ? sz : 0,
                            Url = el.TryGetProperty("url", out var u) ? u.GetString() ?? "" : "",
                            UpdatedAt = el.TryGetProperty("updatedAt", out var t) ? t.GetString() ?? "" : ""
                        });
                    }
                }

                SetEmpty(_items.Count == 0 ? "云端还没有背景，去控制台「账号设置 → 云端背景」上传一张。" : null);
            }
            catch (Exception ex)
            {
                SetEmpty("获取云端背景失败：" + ex.Message);
            }
        }

        private void BtnRefresh_Click(object sender, RoutedEventArgs e) => _ = LoadListAsync();

        private void SetEmpty(string? message)
        {
            var show = !string.IsNullOrEmpty(message);
            if (show) TxtEmpty.Text = message;
            TxtEmpty.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
            DgWallpapers.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        }

        private void DgWallpapers_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            BtnConfirm.IsEnabled = DgWallpapers.SelectedItem is WallpaperItem;
        }

        private void DgWallpapers_MouseDoubleClick(object sender, MouseButtonEventArgs e) => _ = ConfirmAsync();

        private void BtnConfirm_Click(object sender, RoutedEventArgs e) => _ = ConfirmAsync();

        private async Task ConfirmAsync()
        {
            if (DgWallpapers.SelectedItem is not WallpaperItem item) return;

            var dir = CacheDirectory;
            Directory.CreateDirectory(dir);
            var dest = Path.Combine(dir, item.Name);

            var url = item.Url.StartsWith("http", StringComparison.OrdinalIgnoreCase)
                ? item.Url
                : ServerUrl.TrimEnd('/') + item.Url;

            BtnConfirm.IsEnabled = false;
            TxtProgress.Text = "下载中…";

            try
            {
                // 已下载过就直接复用，不必每次都重下一遍大文件
                if (!File.Exists(dest))
                {
                    var bytes = await Http.GetByteArrayAsync(url);
                    await File.WriteAllBytesAsync(dest, bytes);
                }

                LocalPath = dest;
                TxtProgress.Text = "已下载，正在应用…";
                DialogResult = true;
            }
            catch (Exception ex)
            {
                TxtProgress.Text = "下载失败";
                BtnConfirm.IsEnabled = true;
                System.Windows.MessageBox.Show(
                    "下载云端背景失败：" + ex.Message, "云端背景",
                    MessageBoxButton.OK, MessageBoxImage.Warning);
            }
        }

        /// <summary>本地缓存目录；属于 AppData\KunClient，退出时会被整体删除。</summary>
        public static string CacheDirectory =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "KunClient", "wallpapers");

        private static string FormatSize(long bytes)
        {
            if (bytes <= 0) return "-";
            string[] units = { "B", "KB", "MB", "GB" };
            var value = (double)bytes;
            var unit = 0;
            while (value >= 1024 && unit < units.Length - 1) { value /= 1024; unit++; }
            return $"{value:0.##} {units[unit]}";
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e) => DialogResult = false;

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton == MouseButton.Left) DragMove();
        }
    }
}
