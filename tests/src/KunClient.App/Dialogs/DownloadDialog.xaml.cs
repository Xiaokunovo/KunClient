using System;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using KunFlashTool.App.Services;
using KunFlashTool.Core.Configuration;

namespace KunFlashTool.App.Dialogs
{
    public partial class DownloadDialog : Window
    {
        private static string ServerUrl => KunFlashTool.App.Services.AppConfig.ServerUrl;
        private readonly string _toolsDirectory;
        private readonly string _scrcpyDirectory;
        private readonly BackgroundDownloader _downloader;
        public bool DownloadCompleted { get; private set; }

        public DownloadDialog()
        {
            InitializeComponent();
            // 🔴 2026-10-08：这两个目录**必须**与消费方指向同一处，否则「下载工具」下完的东西
            //    谁也找不到，用户会看到同一个包被下两遍（11.3MB + 5.1MB）：
            //      · platform-tools ←→ PlatformToolsManager.ResolveToolsDirectory()
            //      · scrcpy          ←→ ScreenMirrorPage.ScrcpyDir
            //    而且都从「退出即被清理的 %TEMP%\KunClient」搬到了 KunClientState\cache ——
            //    原来每启动一次就要重下 16.4MB，这才是「下载太慢」的主因。
            _toolsDirectory = AppStatePaths.ResolveCache("platform-tools", "tools", "adb.exe");
            _scrcpyDirectory = AppStatePaths.ResolveCache("scrcpy", "scrcpy", "scrcpy.exe");
            _downloader = new BackgroundDownloader();
            _downloader.ProgressChanged += OnProgressChanged;
            Loaded += async (_, _) =>
            {
                // 与登录框一致：淡入 + 微缩放"长出来"，避免下载窗口"啪"地出现
                _ = WindowAnimation.FadeInAsync(this);
                await StartDownloadAsync();
            };
        }

        private void OnProgressChanged(DownloadProgress p)
        {
            // 🔴 2026-10-08 修复（用户反馈：下载窗口停在「下载出错 / 调用线程无法访问此对象，
            //    因为另一个线程拥有该对象」）：BackgroundDownloader 的 ProgressChanged 在
            //    线程池线程上触发（内部全是 ConfigureAwait(false)），而 **IsLoaded 是
            //    DependencyProperty**（FrameworkElement.IsLoaded → GetValue → VerifyAccess），
            //    在非 UI 线程上读会直接抛 InvalidOperationException。实测确认 IsLoaded 与
            //    FindName 在后台线程抛同一条消息（IsVisible 不抛）。
            //    这个异常抛在下载任务体里 → 被 BackgroundDownloader 的 catch 当成「传输失败」
            //    吞掉重试 → 整包下载看着像网络问题 → 兜到 StartDownloadAsync 的 catch 里
            //    显示成「下载出错」。所以**判断必须放在调度到 UI 线程之后**。
            Dispatcher.BeginInvoke(() =>
            {
                if (!IsLoaded) return;
                StatusText.Text = p.Status;
                DetailText.Text = p.Detail;
                // 优先用「映射到整条进度条」的百分比；单次传输若不映射则回落到自身百分比
                var pct = p.OverallPercent >= 0 ? p.OverallPercent : p.Percent;
                if (pct >= 0) ProgressBar.Value = pct;
            });
        }

        /// <summary>把进度条平滑推到某个值（用于解压等没有细粒度进度的阶段，避免硬跳）。</summary>
        private async Task EaseToAsync(double target, int ms = 260)
        {
            var from = ProgressBar.Value;
            if (Math.Abs(target - from) < 0.5) { ProgressBar.Value = target; return; }

            var sw = System.Diagnostics.Stopwatch.StartNew();
            while (true)
            {
                var t = Math.Min(1.0, sw.ElapsedMilliseconds / (double)ms);
                // ease-out，和全局动效口径一致
                var eased = 1 - Math.Pow(1 - t, 3);
                ProgressBar.Value = from + (target - from) * eased;
                if (t >= 1) break;
                await Task.Delay(16);
            }
            ProgressBar.Value = target;
        }

        private void SafeCloseDialog()
        {
            if (IsLoaded && IsVisible)
            {
                try { DialogResult = true; } catch { }
            }
        }

        public async Task StartDownloadAsync()
        {
            try
            {
                // === Step 1: ADB/Fastboot tools ===
                if (!Directory.Exists(_toolsDirectory))
                    Directory.CreateDirectory(_toolsDirectory);

                var adbExists = File.Exists(Path.Combine(_toolsDirectory, "adb.exe"));
                var fastbootExists = File.Exists(Path.Combine(_toolsDirectory, "fastboot.exe"));

                if (!adbExists || !fastbootExists)
                {
                    StatusText.Text = "正在下载辅助工具...";
                    DetailText.Text = "ADB / Fastboot · 正在连接服务器...";
                    ProgressBar.Value = 5;

                    // 整条进度条的分段：下载 5→45，解压 45→50
                    // E11：优先二进制端点 /api/packages/tools/file（原始 zip，比 base64 JSON 少 33% 字节），
                    // 失败自动回落旧的 base64 端点。
                    var toolsOk = await _downloader.DownloadPackageFromServerAsync(
                        ServerUrl, "/api/v2/packages/tools",
                        Path.Combine(_toolsDirectory, "tools.zip"),
                        "正在下载 ADB / Fastboot...",
                        overallFrom: 5, overallTo: 45);

                    if (!IsLoaded) return;

                    if (toolsOk)
                    {
                        StatusText.Text = "正在解压辅助工具...";
                        DetailText.Text = "解压中...";
                        await EaseToAsync(50);

                        ZipFile.ExtractToDirectory(Path.Combine(_toolsDirectory, "tools.zip"), _toolsDirectory, true);
                        File.Delete(Path.Combine(_toolsDirectory, "tools.zip"));
                    }
                    else
                    {
                        StatusText.Text = "辅助工具下载跳过";
                        DetailText.Text = "ADB / Fastboot 加载失败，部分功能可能不可用";
                        ProgressBar.Value = 50;
                        await Task.Delay(800);
                    }
                }
                else
                {
                    ProgressBar.Value = 50;
                }

                if (!IsLoaded) return;

                // === Step 2: scrcpy ===
                var scrcpyExe = FindScrcpyExe();
                if (scrcpyExe == null)
                {
                    StatusText.Text = "正在下载 scrcpy...";
                    DetailText.Text = "投屏工具 · 正在连接服务器...";
                    ProgressBar.Value = 52;

                    if (!Directory.Exists(_scrcpyDirectory))
                        Directory.CreateDirectory(_scrcpyDirectory);

                    // 下载 52→90，解压 90→97。E11：优先 /api/packages/scrcpy/file，失败回落 base64。
                    var scrcpyOk = await _downloader.DownloadPackageFromServerAsync(
                        ServerUrl, "/api/v2/packages/scrcpy",
                        Path.Combine(_scrcpyDirectory, "scrcpy.zip"),
                        "正在下载 scrcpy...",
                        overallFrom: 52, overallTo: 90);

                    if (!IsLoaded) return;

                    if (scrcpyOk)
                    {
                        StatusText.Text = "正在解压 scrcpy...";
                        DetailText.Text = "解压中...";
                        await EaseToAsync(97);

                        ZipFile.ExtractToDirectory(Path.Combine(_scrcpyDirectory, "scrcpy.zip"), _scrcpyDirectory, true);
                        File.Delete(Path.Combine(_scrcpyDirectory, "scrcpy.zip"));

                        scrcpyExe = FindScrcpyExe();
                        if (scrcpyExe == null)
                        {
                            StatusText.Text = "scrcpy 解压异常";
                            DetailText.Text = "压缩包中未找到 scrcpy.exe";
                            ProgressBar.Value = 97;
                            await Task.Delay(800);
                        }
                    }
                    else
                    {
                        StatusText.Text = "scrcpy 下载跳过";
                        DetailText.Text = "投屏工具加载失败，投屏功能可能不可用";
                        ProgressBar.Value = 92;
                        await Task.Delay(800);
                    }
                }
                else
                {
                    ProgressBar.Value = 92;
                }

                if (!IsLoaded) return;

                // === Done ===
                await EaseToAsync(100);
                StatusText.Text = "加载完成";
                DetailText.Text = "工具准备就绪";
                DownloadCompleted = true;
                await Task.Delay(600);
                SafeCloseDialog();
            }
            catch (Exception ex)
            {
                if (!IsLoaded) return;
                StatusText.Text = "下载出错";
                DetailText.Text = ex.Message;
                ProgressBar.Value = 100;
                DownloadCompleted = false;
                await Task.Delay(1500);
                SafeCloseDialog();
            }
        }

        private string? FindScrcpyExe()
        {
            if (!Directory.Exists(_scrcpyDirectory)) return null;
            var files = Directory.GetFiles(_scrcpyDirectory, "scrcpy.exe", SearchOption.AllDirectories);
            return files.Length > 0 ? files[0] : null;
        }

        protected override void OnClosed(EventArgs e)
        {
            _downloader.ProgressChanged -= OnProgressChanged;
            _downloader.Cancel();
            _downloader.Dispose();
            base.OnClosed(e);
        }
    }
}
