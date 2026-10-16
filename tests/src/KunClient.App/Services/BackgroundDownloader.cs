using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KunFlashTool.Core.Net;

namespace KunFlashTool.App.Services
{
    public class DownloadProgress
    {
        public string Status { get; set; } = "";
        public string Detail { get; set; } = "";
        public long BytesReceived { get; set; }
        public long TotalBytes { get; set; }
        public double Percent => TotalBytes > 0 ? (double)BytesReceived / TotalBytes * 100 : -1;
        /// <summary>
        /// 映射到**整个进度条**的百分比（0~100）。
        /// 分阶段下载时，单次传输的 0~100% 只占整条进度条的一段（如 5%~45%），
        /// 由调用方通过 overallFrom/overallTo 指定；未指定时为 -1，界面回落到 <see cref="Percent"/>。
        /// </summary>
        public double OverallPercent { get; set; } = -1;
        /// <summary>瞬时速度（字节/秒），用于实时展示。</summary>
        public double BytesPerSecond { get; set; }
        public bool IsComplete { get; set; }
        public bool IsError { get; set; }
        public string? ErrorMessage { get; set; }
    }

    public class DownloadTask
    {
        public string Url { get; set; } = "";
        public string SavePath { get; set; } = "";
        public string FileName { get; set; } = "";
        public string Description { get; set; } = "";
        public bool Overwrite { get; set; }
        public int MaxRetries { get; set; } = 3;
        public TimeSpan Timeout { get; set; } = TimeSpan.FromMinutes(10);
    }

    public class BackgroundDownloader : IDisposable
    {
        private static readonly HttpClient _sharedClient = CreateSharedClient();
        private CancellationTokenSource? _cts;
        private bool _disposed;
        private int _isDownloading; // 0=idle, 1=downloading

        /// <summary>
        /// 下载统一客户端（D1，2026-10-07）。
        /// <para>CF 对 <c>application/json</c> 会按请求头的 <c>Accept-Encoding</c> 压缩，
        /// 而 .NET 默认**不发这个头** → 收到的永远是未压缩体。实测
        /// <c>/api/packages/tools</c>：6 826 893 → 5 162 728 B（<b>-24.4%</b>）。
        /// 这里打开 AutomaticDecompression，由 handler 自动带上 Accept-Encoding 并解压。</para>
        /// <para>🔴 打开压缩后必须同步改进度口径（见 D2）：此时
        /// <c>Content-Length</c> = 压缩后字节数，而流里读出来的是解压后字节数，
        /// downloaded 会超过 totalBytes → 进度爆表。</para>
        /// </summary>
        public static HttpClient CreateSharedClient(TimeSpan? timeout = null)
        {
            var handler = new SocketsHttpHandler
            {
                AutomaticDecompression = DecompressionMethods.GZip | DecompressionMethods.Deflate,
                PooledConnectionLifetime = TimeSpan.FromMinutes(5),
                ConnectTimeout = TimeSpan.FromSeconds(15),
                MaxConnectionsPerServer = 8,
            };
            return new HttpClient(handler) { Timeout = timeout ?? TimeSpan.FromMinutes(30) };
        }

        public event Action<DownloadProgress>? ProgressChanged;

        public bool IsDownloading => Interlocked.CompareExchange(ref _isDownloading, 0, 0) == 1;

        public void Cancel()
        {
            var cts = _cts;
            if (cts != null)
            {
                try { cts.Cancel(); } catch { }
                _cts = null;
            }
        }

        public async Task<bool> DownloadFileAsync(DownloadTask task)
        {
            if (Interlocked.CompareExchange(ref _isDownloading, 1, 0) != 0) return false;

            _cts = new CancellationTokenSource();
            var ct = _cts.Token;
            var progress = new DownloadProgress { Status = task.Description };

            try
            {
                for (int attempt = 0; attempt <= task.MaxRetries; attempt++)
                {
                    try
                    {
                        if (attempt > 0)
                        {
                            progress.Detail = $"第 {attempt} 次重试...";
                            ProgressChanged?.Invoke(progress);
                            await Task.Delay(1000, ct).ConfigureAwait(false);
                        }

                        var dir = Path.GetDirectoryName(task.SavePath);
                        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                            Directory.CreateDirectory(dir);

                        if (!task.Overwrite && File.Exists(task.SavePath))
                        {
                            progress.Status = task.Description;
                            progress.Detail = "文件已存在，跳过下载";
                            progress.IsComplete = true;
                            ProgressChanged?.Invoke(progress);
                            return true;
                        }

                        progress.Status = task.Description;
                        progress.Detail = "正在连接服务器...";
                        progress.BytesReceived = 0;
                        progress.TotalBytes = 0;
                        ProgressChanged?.Invoke(progress);

                        // 2026-10-08：单流 → 6 连接分片（ChunkedDownloader）。原来这里是
                        // GetAsync + 81920 缓冲边读边写；实测同一个包 31~44s → 8~20s。
                        // 进度口径不变（OverallPercent = 已收/总长），所以 DownloadTask 的
                        // 各调用方（EDL 引导文件、欧加线刷包）不用改。
                        var sw = System.Diagnostics.Stopwatch.StartNew();
                        var r = await ChunkedDownloader.DownloadAsync(
                            _sharedClient, task.Url, task.SavePath, ChunkedDownloader.DefaultParallelism,
                            (got, total) => ReportChunkProgress(progress, task.Description, got, total,
                                                                sw.Elapsed.TotalSeconds, ratio => ratio * 100),
                            ct).ConfigureAwait(false);

                        if (!r.Success) throw new IOException(r.Error ?? "下载失败");

                        progress.BytesReceived = r.TotalBytes;
                        progress.TotalBytes = r.TotalBytes;
                        progress.BytesPerSecond = r.BytesPerSecond;
                        progress.Status = task.Description;
                        progress.Detail = $"下载完成（{r.TotalBytes / 1024.0 / 1024.0:F1} MB · "
                                          + $"{r.BytesPerSecond / 1024.0 / 1024.0:F1} MB/s"
                                          + (r.Parallelism > 1 ? $" · {r.Parallelism} 连接" : "") + "）";
                        progress.IsComplete = true;
                        ProgressChanged?.Invoke(progress);
                        return true;
                    }
                    catch (OperationCanceledException)
                    {
                        throw;
                    }
                    catch (Exception ex) when (attempt < task.MaxRetries)
                    {
                        progress.Detail = $"下载出错: {ex.Message}，准备重试...";
                        ProgressChanged?.Invoke(progress);
                    }
                }

                progress.Status = task.Description;
                progress.Detail = "达到最大重试次数";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            catch (OperationCanceledException)
            {
                progress.Status = task.Description;
                progress.Detail = "下载已取消";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            catch (Exception ex)
            {
                progress.Status = task.Description;
                progress.Detail = $"下载失败: {ex.Message}";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            finally
            {
                Interlocked.Exchange(ref _isDownloading, 0);
                var cts = _cts;
                _cts = null;
                try { cts?.Dispose(); } catch { }
            }
        }

        /// <summary>
        /// 从服务器的 base64 接口取包并落盘。
        /// <para>
        /// 🔴 **必须流式读**。原来用 <c>GetStringAsync</c>：它会把整个响应体缓冲完才返回，
        ///    传输期间一帧进度都报不出来 —— 进度条只能靠调用方在阶段边界手动跳（10/40/50/85/100），
        ///    用户看到的就是「卡住不动，然后猛地跳一格」。这里改成
        ///    <c>ResponseHeadersRead</c> + 边读边报，进度条才真正跟着字节走。
        /// </para>
        /// <param name="overallFrom">本次传输占整条进度条的起点百分比（如 5）；&lt;0 表示不映射。</param>
        /// <param name="overallTo">本次传输占整条进度条的终点百分比（如 45）。</param>
        /// </summary>
        public async Task<bool> DownloadBase64FromServerAsync(string serverUrl, string apiPath, string savePath,
                                                             string description, int maxRetries = 3,
                                                             double overallFrom = -1, double overallTo = -1)
        {
            if (Interlocked.CompareExchange(ref _isDownloading, 1, 0) != 0) return false;

            _cts = new CancellationTokenSource();
            var ct = _cts.Token;
            var progress = new DownloadProgress { Status = description };

            // 单次传输的 0~1 映射到整条进度条的 overallFrom~overallTo
            double Map(double ratio01)
            {
                ratio01 = Math.Clamp(ratio01, 0, 1);
                return (overallFrom < 0 || overallTo < 0)
                    ? ratio01 * 100
                    : overallFrom + ratio01 * (overallTo - overallFrom);
            }

            try
            {
                var dir = Path.GetDirectoryName(savePath);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                    Directory.CreateDirectory(dir);

                for (int attempt = 0; attempt <= maxRetries; attempt++)
                {
                    try
                    {
                        if (attempt > 0)
                        {
                            progress.Detail = $"第 {attempt} 次重试...";
                            ProgressChanged?.Invoke(progress);
                            await Task.Delay(1000, ct).ConfigureAwait(false);
                        }

                        progress.BytesReceived = 0;
                        progress.TotalBytes = 0;
                        progress.BytesPerSecond = 0;
                        progress.OverallPercent = Map(0);
                        progress.Detail = "正在连接服务器...";
                        ProgressChanged?.Invoke(progress);

                        using var response = await _sharedClient.GetAsync(
                            $"{serverUrl}{apiPath}", HttpCompletionOption.ResponseHeadersRead, ct)
                            .ConfigureAwait(false);
                        response.EnsureSuccessStatusCode();

                        // 同 D2：压缩响应下 Content-Length 是压缩后字节数，直接拿来算进度会爆表。
                        var ce = response.Content.Headers.ContentEncoding;
                        var compressed = ce != null && ce.Count > 0
                              && !string.Equals(ce.First(), "identity", StringComparison.OrdinalIgnoreCase);
                        var totalBytes = compressed ? -1L : (response.Content.Headers.ContentLength ?? -1L);

                        using var stream = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
                        using var ms = new MemoryStream(totalBytes > 0 ? (int)Math.Min(totalBytes, int.MaxValue) : 0);

                        var buffer = new byte[81920];
                        long received = 0;
                        var sw = System.Diagnostics.Stopwatch.StartNew();
                        long lastReportTicks = 0;

                        int read;
                        while ((read = await stream.ReadAsync(buffer, 0, buffer.Length, ct).ConfigureAwait(false)) > 0)
                        {
                            ms.Write(buffer, 0, read);
                            received += read;

                            // 150ms 节流：足够跟手，又不会把 UI 线程刷爆
                            if (sw.ElapsedMilliseconds - lastReportTicks >= 150)
                            {
                                lastReportTicks = sw.ElapsedMilliseconds;
                                progress.BytesReceived = received;
                                progress.TotalBytes = totalBytes;
                                progress.BytesPerSecond = sw.Elapsed.TotalSeconds > 0
                                    ? received / sw.Elapsed.TotalSeconds : 0;

                                var got = received / 1024.0 / 1024.0;
                                var speed = progress.BytesPerSecond / 1024.0 / 1024.0;
                                progress.OverallPercent = totalBytes > 0
                                    ? Map((double)received / totalBytes)
                                    : Map(0);

                                if (totalBytes > 0)
                                {
                                    var totalMb = totalBytes / 1024.0 / 1024.0;
                                    progress.Detail = $"{got:F1} MB / {totalMb:F1} MB · {speed:F1} MB/s";
                                }
                                else
                                {
                                    progress.Detail = $"{got:F1} MB · {speed:F1} MB/s";
                                }
                                ProgressChanged?.Invoke(progress);
                            }
                        }

                        progress.BytesReceived = received;
                        progress.TotalBytes = totalBytes > 0 ? totalBytes : received;
                        progress.BytesPerSecond = sw.Elapsed.TotalSeconds > 0
                            ? received / sw.Elapsed.TotalSeconds : 0;
                        progress.OverallPercent = Map(1);
                        progress.Detail = "传输完成，正在解码...";
                        ProgressChanged?.Invoke(progress);

                        // 从缓冲区直接解析，避免再复制一份大字符串
                        JsonElement json;
                        try
                        {
                            json = System.Text.Json.JsonSerializer.Deserialize<JsonElement>(
                                new ReadOnlySpan<byte>(ms.GetBuffer(), 0, (int)ms.Length));
                        }
                        catch
                        {
                            progress.Detail = "服务器返回的 JSON 无法解析";
                            progress.IsError = true;
                            ProgressChanged?.Invoke(progress);
                            return false;
                        }

                        if (!json.TryGetProperty("success", out var okEl) || !okEl.GetBoolean())
                        {
                            progress.Status = description;
                            progress.Detail = json.TryGetProperty("message", out var msgEl)
                                ? msgEl.GetString() ?? "服务器返回失败" : "服务器返回失败";
                            progress.IsError = true;
                            ProgressChanged?.Invoke(progress);
                            return false;
                        }

                        var base64Data = json.TryGetProperty("data", out var dataEl) ? dataEl.GetString() : null;
                        if (string.IsNullOrEmpty(base64Data))
                        {
                            progress.Detail = "服务器无数据";
                            progress.IsError = true;
                            ProgressChanged?.Invoke(progress);
                            return false;
                        }

                        var bytes = Convert.FromBase64String(base64Data);
                        await File.WriteAllBytesAsync(savePath, bytes, ct).ConfigureAwait(false);

                        progress.BytesReceived = bytes.Length;
                        progress.TotalBytes = bytes.Length;
                        progress.OverallPercent = Map(1);
                        progress.Status = description;
                        progress.Detail = $"完成（{bytes.Length / 1024.0 / 1024.0:F1} MB）";
                        progress.IsComplete = true;
                        ProgressChanged?.Invoke(progress);
                        return true;
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception ex) when (attempt < maxRetries)
                    {
                        progress.Detail = $"获取失败: {ex.Message}，准备重试...";
                        ProgressChanged?.Invoke(progress);
                    }
                }

                progress.Status = description;
                progress.Detail = "达到最大重试次数";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            catch (OperationCanceledException)
            {
                progress.Status = description;
                progress.Detail = "已取消";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            catch (Exception ex)
            {
                progress.Status = description;
                progress.Detail = $"失败: {ex.Message}";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            finally
            {
                Interlocked.Exchange(ref _isDownloading, 0);
                var cts = _cts;
                _cts = null;
                try { cts?.Dispose(); } catch { }
            }
        }

        /// <summary>
        /// 二进制端点的下载（E1/E11 配套，2026-10-07）：URL → 文件，边读边报进度。
        /// 与 DownloadBase64FromServerAsync 只差数据来源 —— 那条是「JSON 塞 base64」
        /// （+33% 字节），这条直接拿原始 zip，少传三分之一。
        /// <para>2026-10-08：内部换成 <see cref="ChunkedDownloader"/>（默认 6 连接分片）。
        /// 真机实测（chunk_probe，11.3MB）：单连接 31.4~43.8s → 分片 8.4~20.5s，2.2~2.8 倍。
        /// 生命周期（_cts / _isDownloading）留在本方法上，实际下载交给
        /// <see cref="DownloadUrlInternalAsync"/>，好让
        /// <see cref="DownloadPackageFromServerAsync"/> 能按顺序试多个 URL 而只占一次闸门。</para>
        /// </summary>
        public async Task<bool> DownloadFileFromServerAsync(string serverUrl, string apiPath, string savePath,
                                                             string description, int maxRetries = 3,
                                                             double overallFrom = -1, double overallTo = -1)
        {
            if (Interlocked.CompareExchange(ref _isDownloading, 1, 0) != 0) return false;

            _cts = new CancellationTokenSource();
            var ct = _cts.Token;
            try
            {
                return await DownloadUrlInternalAsync($"{serverUrl}{apiPath}", savePath, description,
                                                      maxRetries, overallFrom, overallTo, ct).ConfigureAwait(false);
            }
            finally
            {
                Interlocked.Exchange(ref _isDownloading, 0);
                var cts = _cts;
                _cts = null;
                try { cts?.Dispose(); } catch { }
            }
        }

        /// <summary>
        /// 一个 URL 的实际下载：分片并行 + 进度映射。**不碰** _cts / _isDownloading ——
        /// 闸门由调用方持有，这样「先试边缘缓存 URL、失败再试 API 端点」不会自己把自己挡掉。
        /// </summary>
        private async Task<bool> DownloadUrlInternalAsync(string url, string savePath, string description,
                                                          int maxRetries, double overallFrom, double overallTo,
                                                          CancellationToken ct)
        {
            var progress = new DownloadProgress { Status = description };

            double Map(double ratio01)
            {
                ratio01 = Math.Clamp(ratio01, 0, 1);
                return (overallFrom < 0 || overallTo < 0)
                    ? ratio01 * 100
                    : overallFrom + ratio01 * (overallTo - overallFrom);
            }

            try
            {
                var dir = Path.GetDirectoryName(savePath);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                    Directory.CreateDirectory(dir);

                for (int attempt = 0; attempt <= maxRetries; attempt++)
                {
                    try
                    {
                        if (attempt > 0)
                        {
                            progress.Detail = $"第 {attempt} 次重试...";
                            ProgressChanged?.Invoke(progress);
                            await Task.Delay(1000, ct).ConfigureAwait(false);
                        }

                        progress.BytesReceived = 0;
                        progress.TotalBytes = 0;
                        progress.BytesPerSecond = 0;
                        progress.OverallPercent = Map(0);
                        progress.Detail = "正在连接服务器...";
                        ProgressChanged?.Invoke(progress);

                        var sw = System.Diagnostics.Stopwatch.StartNew();
                        var r = await ChunkedDownloader.DownloadAsync(
                            _sharedClient, url, savePath, ChunkedDownloader.DefaultParallelism,
                            (got, total) => ReportChunkProgress(progress, description, got, total, sw.Elapsed.TotalSeconds, Map),
                            ct).ConfigureAwait(false);

                        if (r.Success)
                        {
                            progress.BytesReceived = r.TotalBytes;
                            progress.TotalBytes = r.TotalBytes;
                            progress.BytesPerSecond = r.BytesPerSecond;
                            progress.OverallPercent = Map(1);
                            progress.Status = description;
                            progress.Detail = $"完成（{r.TotalBytes / 1024.0 / 1024.0:F1} MB · "
                                              + $"{r.BytesPerSecond / 1024.0 / 1024.0:F1} MB/s"
                                              + (r.Parallelism > 1 ? $" · {r.Parallelism} 连接" : "") + "）";
                            progress.IsComplete = true;
                            ProgressChanged?.Invoke(progress);
                            return true;
                        }

                        // 端点根本不存在（404/403/410）→ 原地重试毫无意义，立刻把「失败」交回上层去换端点
                        if (r.HttpStatus is 404 or 403 or 410)
                        {
                            progress.Detail = $"端点不可用: {r.Error}";
                            progress.IsError = true;
                            ProgressChanged?.Invoke(progress);
                            return false;
                        }

                        throw new IOException(r.Error ?? "下载失败");
                    }
                    catch (OperationCanceledException) { throw; }
                    catch (Exception ex) when (attempt < maxRetries)
                    {
                        progress.Detail = $"获取失败: {ex.Message}，准备重试...";
                        ProgressChanged?.Invoke(progress);
                    }
                }

                progress.Status = description;
                progress.Detail = "达到最大重试次数";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            catch (OperationCanceledException)
            {
                progress.Status = description;
                progress.Detail = "已取消";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            catch (Exception ex)
            {
                progress.Status = description;
                progress.Detail = $"失败: {ex.Message}";
                progress.IsError = true;
                ProgressChanged?.Invoke(progress);
                return false;
            }
            finally
            {
                // 半截文件必须删掉：留着会让下一次解压拿到坏包。
                try
                {
                    if (!progress.IsComplete && File.Exists(savePath)) File.Delete(savePath);
                }
                catch { }
            }
        }

        /// <summary>分片进度 → DownloadProgress（含 overallFrom/To 映射与瞬时速度）。</summary>
        private void ReportChunkProgress(DownloadProgress p, string description, long got, long total,
                                         double seconds, Func<double, double> map)
        {
            p.BytesReceived = got;
            p.TotalBytes = total;
            p.Status = description;
            p.BytesPerSecond = seconds > 0 ? got / seconds : 0;

            var gotMb = got / 1024.0 / 1024.0;
            var speedMb = p.BytesPerSecond / 1024.0 / 1024.0;
            p.OverallPercent = total > 0 ? map((double)got / total) : map(0);
            p.Detail = total > 0
                ? $"{gotMb:F1} MB / {total / 1024.0 / 1024.0:F1} MB · {speedMb:F1} MB/s"
                : $"{gotMb:F1} MB · {speedMb:F1} MB/s";

            ProgressChanged?.Invoke(p);
        }


        /// <summary>
        /// 服务端大包下载的统一入口（E11 + 2026-10-08 提速）。
        /// <para>端点优先级（逐级回落，任何一级成功即止）：</para>
        /// <list type="number">
        ///   <item><description><c>&lt;serverUrl&gt;/assets/&lt;pkg&gt;.zip</c> —— <b>静态路径，走 CDN 边缘缓存</b>
        ///     （实测 <c>cf-cache-status: HIT</c>，TTFB 1.6s → 0.85s）。注意它省的是**握手与回源**，
        ///     吞吐和 API 端点基本一样（实测 4 连接下 14.39s vs 14.88s）—— 提速靠的是分片并发，不是这一级。
        ///     服务端 <c>assets/</c> 下放的就是同一份 zip，字节与 <c>/api/packages/*</c> 完全一致。</description></item>
        ///   <item><description><c>&lt;apiPath&gt;/file</c> —— 二进制端点（原始字节，比 base64 少 33%）。</description></item>
        ///   <item><description><c>&lt;apiPath&gt;</c> —— 老服务端才走的 base64 JSON 回落。</description></item>
        /// </list>
        /// <para>前两级都是「端点不存在就立刻换下一个」，各只试一次；重试次数全部留给最慢但最稳的
        /// base64 那级。整个方法只占一次 _cts / _isDownloading 闸门 —— 前几级失败的半截文件由
        /// <see cref="DownloadUrlInternalAsync"/> 自己清掉。</para>
        /// </summary>
        public async Task<bool> DownloadPackageFromServerAsync(string serverUrl, string apiPath, string savePath,
                                                                string description, int maxRetries = 3,
                                                                double overallFrom = -1, double overallTo = -1)
        {
            // "/api/v1/packages/scrcpy" → "scrcpy"：静态路径的文件名与 API 末段同名
            var segments = apiPath.Trim('/').Split('/');
            var pkg = segments.Length > 0 ? segments[^1] : "";

            if (Interlocked.CompareExchange(ref _isDownloading, 1, 0) != 0) return false;

            _cts = new CancellationTokenSource();
            var ct = _cts.Token;
            try
            {
                if (!string.IsNullOrEmpty(pkg))
                {
                    // ① 边缘缓存优先。给 1 次重试：它是主路径，一次网络抖动不该直接掉到最慢的 base64。
                    var viaCdn = await DownloadUrlInternalAsync($"{serverUrl}/assets/{pkg}.zip", savePath,
                        description, maxRetries: 1, overallFrom, overallTo, ct).ConfigureAwait(false);
                    if (viaCdn) return true;
                }

                var viaApi = await DownloadUrlInternalAsync($"{serverUrl}{apiPath}/file", savePath,
                    description, maxRetries: 0, overallFrom, overallTo, ct).ConfigureAwait(false);
                if (viaApi) return true;
            }
            finally
            {
                Interlocked.Exchange(ref _isDownloading, 0);
                var cts = _cts;
                _cts = null;
                try { cts?.Dispose(); } catch { }
            }

            return await DownloadBase64FromServerAsync(serverUrl, apiPath, savePath,
                description, maxRetries, overallFrom, overallTo).ConfigureAwait(false);
        }

        public void Dispose()
        {
            if (!_disposed)
            {
                Cancel();
                _disposed = true;
            }
        }
    }
}
