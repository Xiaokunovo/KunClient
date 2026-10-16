using System;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    /// <summary>
    /// 云端驱动：清单来自服务端 <c>/api/packages/drivers</c>（该接口直接读 <c>assets/drivers/</c> 目录，
    /// 所以「上传一个驱动包」就等于「上架一个驱动」，不用改库也不用重启服务）。
    /// </summary>
    public partial class CloudDriverPage : BasePage
    {
        private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(30) };
        private static string ServerUrl => AppConfig.ServerUrl;

        private readonly ObservableCollection<DriverItem> _items = new();
        private CancellationTokenSource? _downloadCts;

        public sealed class DriverItem
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

        public CloudDriverPage()
        {
            InitializeComponent();
            DgDrivers.ItemsSource = _items;
            Loaded += async (_, __) => await LoadListAsync();
        }

        // ───────────────────────── 列表 ─────────────────────────

        private async Task LoadListAsync()
        {
            BtnRefresh.IsEnabled = false;
            SetEmpty("正在从服务器获取驱动列表…");

            try
            {
                var json = await Http.GetStringAsync($"{ServerUrl}/api/v1/packages/drivers");
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;

                if (!root.TryGetProperty("success", out var ok) || ok.ValueKind != JsonValueKind.True)
                {
                    var msg = root.TryGetProperty("message", out var m) ? m.GetString() : null;
                    SetEmpty("获取失败：" + (msg ?? "服务器返回失败"));
                    return;
                }

                // 🔴 填表期间先摘开 ItemsSource：_items 绑定状态下每 Add 一格 DataGrid 就插一行
                //    容器并当场重排；挂回去只发一次 Reset、只排一次版。
                DgDrivers.ItemsSource = null;
                _items.Clear();
                if (root.TryGetProperty("data", out var arr) && arr.ValueKind == JsonValueKind.Array)
                {
                    foreach (var el in arr.EnumerateArray())
                    {
                        var name = el.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "";
                        if (string.IsNullOrEmpty(name)) continue;
                        _items.Add(new DriverItem
                        {
                            Name = name,
                            Size = el.TryGetProperty("size", out var s) && s.TryGetInt64(out var sz) ? sz : 0,
                            Url = el.TryGetProperty("url", out var u) ? u.GetString() ?? "" : "",
                            UpdatedAt = el.TryGetProperty("updatedAt", out var t) ? t.GetString() ?? "" : ""
                        });
                    }
                }
                DgDrivers.ItemsSource = _items;

                if (_items.Count == 0) SetEmpty("暂无云端驱动（服务端 assets/drivers 目录为空）");
                else SetEmpty(null);
            }
            catch (Exception ex)
            {
                SetEmpty("获取驱动列表失败：" + ex.Message);
            }
            finally
            {
                BtnRefresh.IsEnabled = true;
            }
        }

        private void BtnRefresh_Click(object sender, RoutedEventArgs e) => _ = LoadListAsync();

        private void SetEmpty(string? message)
        {
            if (string.IsNullOrEmpty(message))
            {
                TxtEmpty.Visibility = Visibility.Collapsed;
                DgDrivers.Visibility = Visibility.Visible;
            }
            else
            {
                TxtEmpty.Text = message;
                TxtEmpty.Visibility = Visibility.Visible;
                DgDrivers.Visibility = Visibility.Collapsed;
            }
        }

        private void DgDrivers_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            var item = DgDrivers.SelectedItem as DriverItem;
            BtnDownload.IsEnabled = item != null;
            TxtSelected.Text = item != null ? $"{item.Name} · {item.SizeText}" : "未选择驱动";
        }

        // ───────────────────────── 下载 ─────────────────────────

        private async void BtnDownload_Click(object sender, RoutedEventArgs e)
        {
            if (DgDrivers.SelectedItem is not DriverItem item) return;

            var dir = DriversDirectory;
            Directory.CreateDirectory(dir);
            var dest = Path.Combine(dir, item.Name);

            // 同名文件已存在就换一个名字，别静默覆盖（用户可能改过里面的东西）
            if (File.Exists(dest))
            {
                var stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
                dest = Path.Combine(dir,
                    Path.GetFileNameWithoutExtension(item.Name) + $"-{stamp}" + Path.GetExtension(item.Name));
            }

            var url = item.Url.StartsWith("http", StringComparison.OrdinalIgnoreCase)
                ? item.Url
                : ServerUrl.TrimEnd('/') + item.Url;

            _downloadCts = new CancellationTokenSource();
            BtnDownload.IsEnabled = false;
            BtnRefresh.IsEnabled = false;
            PrgDownload.Value = 0;
            TxtProgress.Text = "连接中…";
            // 页面没有 TxtLog（不做日志三明治），但下载仍是一次「任务」：
            // 进度与收尾一律上报外壳（外壳底部双进度条 + 状态栏「任务」段）。
            var sw = Stopwatch.StartNew();
            ShellStatus.BeginTask($"下载驱动 {item.Name}");

            try
            {
                using var response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, _downloadCts.Token);
                if (!response.IsSuccessStatusCode)
                {
                    ShowError($"下载失败：服务器返回 {(int)response.StatusCode}");
                    ShellStatus.Report(task: "下载失败");
                    return;
                }

                var total = response.Content.Headers.ContentLength ?? 0;
                await using var input = await response.Content.ReadAsStreamAsync();
                await using var output = new FileStream(dest, FileMode.CreateNew, FileAccess.Write, FileShare.None);

                var buffer = new byte[81920];
                long read = 0;
                int n;
                // D12（2026-10-07）：150ms 节流 —— 原来每个 80KB 块都刷一次 UI + 上报外壳，
                // 高速链路上每秒几十次布局/重绘纯属浪费，还会和下载本身抢 CPU。
                var progressSw = System.Diagnostics.Stopwatch.StartNew();
                long lastReportMs = 0;
                while ((n = await input.ReadAsync(buffer, _downloadCts.Token)) > 0)
                {
                    await output.WriteAsync(buffer.AsMemory(0, n), _downloadCts.Token);
                    read += n;
                    if (progressSw.ElapsedMilliseconds - lastReportMs < 150) continue;
                    lastReportMs = progressSw.ElapsedMilliseconds;
                    if (total > 0)
                    {
                        PrgDownload.Value = read * 100.0 / total;
                        TxtProgress.Text = $"{FormatSize(read)} / {FormatSize(total)}";
                    }
                    else
                    {
                        TxtProgress.Text = $"已下载 {FormatSize(read)}";
                    }
                    // 页面自己的进度条照旧，同时把进度/已传读数镜像给外壳（外壳底部双进度条 + 状态栏）
                    ShellStatus.Report(totalSize: TxtProgress.Text, percent: PrgDownload.Value);
                }

                PrgDownload.Value = 100;

                var extracted = TryExtract(dest, dir);
                TxtProgress.Text = "完成";
                ShowSuccess(extracted
                    ? $"已下载并解压：{Path.GetFileName(dest)}"
                    : $"已下载：{Path.GetFileName(dest)}");
                ReportElapsed(sw.Elapsed);
                ShellStatus.EndTask();
            }
            catch (OperationCanceledException)
            {
                TxtProgress.Text = "已取消";
                ShellStatus.Report(task: "下载已取消");
            }
            catch (Exception ex)
            {
                TxtProgress.Text = "失败";
                ShowError("下载失败：" + ex.Message);
                ShellStatus.Report(task: "下载失败");
            }
            finally
            {
                _downloadCts = null;
                BtnDownload.IsEnabled = DgDrivers.SelectedItem is DriverItem;
                BtnRefresh.IsEnabled = true;
            }
        }

        /// <summary>把本次下载耗时并进外壳状态栏的「任务」段（参考稿的状态栏没有单独的耗时槽位）。</summary>
        private static void ReportElapsed(TimeSpan t)
            => ShellStatus.SetElapsed(t.TotalHours >= 1
                ? $"{(int)t.TotalHours}:{t.Minutes:D2}:{t.Seconds:D2}"
                : $"{t.Minutes:D2}:{t.Seconds:D2}");

        /// <summary>zip 自动展开到同名目录；其他格式（exe / msi / inf）原样留着，不代为安装。</summary>
        private static bool TryExtract(string zipPath, string dir)
        {
            if (!zipPath.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) return false;
            try
            {
                var target = Path.Combine(dir, Path.GetFileNameWithoutExtension(zipPath));
                ZipFile.ExtractToDirectory(zipPath, target, overwriteFiles: true);
                return true;
            }
            catch
            {
                return false;
            }
        }

        private void BtnOpenFolder_Click(object sender, RoutedEventArgs e)
        {
            var dir = DriversDirectory;
            Directory.CreateDirectory(dir);
            try
            {
                Process.Start(new ProcessStartInfo("explorer.exe", dir) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                ShowError("打开文件夹失败：" + ex.Message);
            }
        }

        private static string DriversDirectory =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "KunClient", "drivers");

        private static string FormatSize(long bytes)
        {
            if (bytes <= 0) return "-";
            string[] units = { "B", "KB", "MB", "GB" };
            var value = (double)bytes;
            var unit = 0;
            while (value >= 1024 && unit < units.Length - 1) { value /= 1024; unit++; }
            return $"{value:0.##} {units[unit]}";
        }
    }
}
