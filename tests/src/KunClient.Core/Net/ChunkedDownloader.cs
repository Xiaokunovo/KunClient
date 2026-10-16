using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace KunFlashTool.Core.Net;

/// <summary>
/// 并行分片下载器（2026-10-08，用户反馈「下载速度太慢」）。
///
/// <para><b>为什么必须多连接</b>：用本类跑真机 A/B（chunk_probe，直连服务端，
/// 11.3MB 的 scrcpy.zip，同一时刻同一网络对比）：</para>
/// <list type="table">
///   <item><description>单连接：31.4 ~ 43.8 s（0.25 ~ 0.34 MB/s）—— 就是旧实现的口径</description></item>
///   <item><description>多连接分片：8.4 ~ 20.5 s（0.53 ~ 1.28 MB/s），<b>2.2 ~ 2.8 倍</b></description></item>
/// </list>
/// <para>另一个关键读数：<c>cf-cache-status: HIT</c>（边缘已命中）时 TTFB 从 1.6s 掉到 0.85s，
/// 但**吞吐几乎没变** —— 瓶颈在「本机 ↔ 边缘」这一跳的**单流 TCP 窗口/丢包**上，不在回源、
/// 也不在缓存。这种链路加连接数是唯一客户端能解的解法；换 CDN、上 Argo 都不是客户端的事。</para>
///
/// <para><b>为什么默认 6 而不是 4 或 8</b>：同一档连跑 3 轮做扫描（4/6/8 各 3 轮，交错跑以抹掉时间偏置），
/// 均值 4→14.10s、6→12.98s、8→15.33s，但**单轮波动比档位差异还大**（同一档 8.40s ~ 18.77s）。
/// 也就是说 4~8 基本在噪声内，6 只是略优且比 8 少压服务端一档连接。别拿单轮结果去改这个值。</para>
///
/// <para><b>服务端已具备的前提</b>（2026-10-07 的 E1/E2 就铺好了）：</para>
/// <list type="bullet">
///   <item><description><c>/assets/*.zip</c> 与 <c>/api/packages/*/file</c> 都返回 <c>Accept-Ranges: bytes</c> + 206；</description></item>
///   <item><description>Cloudflare 原样透传 Range（4/6/8 条并发 <c>Range</c> 端到端跑通，SHA256 与单流逐字节一致）。</description></item>
/// </list>
///
/// <para><b>与单流版的行为差异</b>：进度回调是「所有分片已收字节之和」，天然单调递增；
/// 但**首字节到达前**它会先报 (0, total)，调用方照旧能画出 0% 而不是空白。
/// 分片请求显式带 <c>Accept-Encoding: identity</c> —— 否则一旦中间层压缩，Range 偏移全部对不上。</para>
///
/// <para><b>失败即整体失败</b>：任一分片重试 2 次后仍失败 → 返回 false 并删掉半截文件，
/// 由上层回落到单流 / base64 端点（老服务端不保证支持 Range，不能把用户卡死）。</para>
/// </summary>
public static class ChunkedDownloader
{
    /// <summary>默认并发分片数。见类注释：4/6/8 在噪声内，6 略优（三轮扫描均值 12.98s）。</summary>
    public const int DefaultParallelism = 6;

    /// <summary>小于这个体积不值得开分片（握手/调度开销盖过收益）。</summary>
    private const long MinTotalForChunks = 1024 * 1024;

    private const int BufferSize = 128 * 1024;
    private const int MaxChunkRetry = 2;
    private const int ReportIntervalMs = 150;

    public sealed class Result
    {
        public bool Success { get; init; }
        public string? Error { get; init; }
        public long TotalBytes { get; init; }
        /// <summary>服务端是否真的支持 Range（false = 走的单流，进度口径仍是全长）。</summary>
        public bool RangeSupported { get; init; }
        /// <summary>实际用到的并发数（0 = 单流）。</summary>
        public int Parallelism { get; init; }
        /// <summary>
        /// 服务端直接给出的失败状态码（404/403 等），成功或纯传输层失败时为 null。
        /// 调用方据此判断「这个端点根本不存在」→ 直接换下一个端点，别原地重试。
        /// </summary>
        public int? HttpStatus { get; init; }
        public double Seconds { get; init; }
        public double BytesPerSecond => Seconds > 0 ? TotalBytes / Seconds : 0;
    }

    /// <summary>
    /// 下载 <paramref name="url"/> 到 <paramref name="savePath"/>（覆盖写）。
    /// 支持 Range 时自动分 <paramref name="parallelism"/> 片并发抓取，否则退化为单流。
    /// </summary>
    /// <param name="client">调用方提供的 <see cref="HttpClient"/>：<b>MaxConnectionsPerServer 必须 ≥ parallelism</b>，
    /// 且 Timeout 要够长（分片各自计时，不是全长计时）。</param>
    /// <param name="onProgress">(已下载字节, 总字节)；总长为 -1 时表示未知。</param>
    public static async Task<Result> DownloadAsync(
        HttpClient client,
        string url,
        string savePath,
        int parallelism,
        Action<long, long>? onProgress,
        CancellationToken ct)
    {
        if (parallelism < 1) parallelism = DefaultParallelism;

        var sw = Stopwatch.StartNew();

        // ── 步骤 1：用 1 字节的 Range 探一次「总长 + 是否支持 Range」 ──
        //    这一步同时把「服务端忽略 Range、照旧回 200 全长」这种情况当场识破：
        //    那种响应体是完整的包，直接顺着它单流落盘，不浪费一次往返。
        HttpResponseMessage? probe = null;
        try
        {
            probe = await SendAsync(client, url, rangeStart: 0, rangeEnd: 0, ct).ConfigureAwait(false);

            if (!probe.IsSuccessStatusCode)
            {
                // 404/403 之类的「这个 URL 根本不存在」要快速失败，让上层换下一个端点
                var code = (int)probe.StatusCode;
                return Fail($"HTTP {code} {probe.ReasonPhrase}", sw, code);
            }

            var total = -1L;
            var rangeOk = false;

            if (probe.StatusCode == HttpStatusCode.PartialContent)
            {
                total = ParseTotalFromContentRange(probe.Content.Headers.ContentRange);
                rangeOk = total > 0;
            }

            if (!rangeOk)
            {
                // 服务端不认 Range（200 全长）或没给 Content-Range → 单流。刚拿到的响应体就是完整内容。
                if (probe.StatusCode == HttpStatusCode.OK)
                {
                    var single = await SingleStreamAsync(probe, savePath, onProgress, ct).ConfigureAwait(false);
                    probe = null;   // 已被 SingleStreamAsync 消费，别再 Dispose 两次
                    return new Result
                    {
                        Success = single.ok,
                        Error = single.error,
                        TotalBytes = single.total,
                        RangeSupported = false,
                        Parallelism = 0,
                        Seconds = sw.Elapsed.TotalSeconds,
                    };
                }
                probe.Dispose();
                return Fail($"不支持 Range 且响应为 {(int)probe.StatusCode}", sw, (int)probe.StatusCode);
            }

            probe.Dispose();
            probe = null;

            if (total < MinTotalForChunks)
            {
                // 小文件：单流更划算（也避免「4 个连接各下 20KB」这种荒谬调度）
                var single = await SingleStreamAfterProbeAsync(client, url, savePath, onProgress, ct).ConfigureAwait(false);
                return new Result
                {
                    Success = single.ok,
                    Error = single.error,
                    TotalBytes = single.total > 0 ? single.total : total,
                    RangeSupported = true,
                    Parallelism = 0,
                    Seconds = sw.Elapsed.TotalSeconds,
                };
            }

            // ── 步骤 2：分片并发 ──
            var state = new ProgressState { Total = total };
            var chunkSize = total / parallelism;
            var handle = File.OpenHandle(savePath, FileMode.Create, FileAccess.Write, FileShare.ReadWrite,
                                         FileOptions.None, preallocationSize: total);
            try
            {
                Report(state, onProgress, force: true);

                var tasks = new List<Task<(bool ok, string? error)>>(parallelism);
                for (var i = 0; i < parallelism; i++)
                {
                    var start = i * chunkSize;
                    // 末片收尾到文件最后一字节：total 除不尽时余数全部归末片，避免漏字节
                    var end = i == parallelism - 1 ? total - 1 : start + chunkSize - 1;
                    tasks.Add(DownloadChunkAsync(client, url, handle, start, end, state, onProgress, ct));
                }

                var results = await Task.WhenAll(tasks).ConfigureAwait(false);
                Report(state, onProgress, force: true);

                // 🔴 别用 FirstOrDefault(...) 判失败：**全部成功**时它返回 default（false, null），
                //    `!bad.ok` 一样成立 —— 会把一次完整成功的下载报成「分片下载失败」。
                //    （这个坑是 chunk_probe 首跑抓出来的：5 次下载全报失败，实际文件都完整。）
                var failed = results.Where(r => !r.ok).ToList();
                if (failed.Count > 0)
                {
                    handle.Dispose();
                    TryDelete(savePath);
                    return Fail(failed[0].error ?? "分片下载失败", sw);
                }

                // 账必须平：少一个字节都算失败（半截包解压出来是坏工具，比下载失败更难查）
                var got = Interlocked.Read(ref state.Received);
                if (got != total)
                {
                    handle.Dispose();
                    TryDelete(savePath);
                    return Fail($"字节数不符：收到 {got}，应为 {total}", sw);
                }
            }
            finally
            {
                handle.Dispose();
            }

            return new Result
            {
                Success = true,
                TotalBytes = total,
                RangeSupported = true,
                Parallelism = parallelism,
                Seconds = sw.Elapsed.TotalSeconds,
            };
        }
        catch (OperationCanceledException)
        {
            probe?.Dispose();
            return Fail("已取消", sw);
        }
        catch (Exception ex)
        {
            probe?.Dispose();
            TryDelete(savePath);
            return Fail(ex.Message, sw);
        }
    }

    /// <summary>单个分片的抓取：从 start 到 end（含），带 2 次从断点续抓的重试。</summary>
    private static async Task<(bool ok, string? error)> DownloadChunkAsync(
        HttpClient client, string url, SafeFileHandle handle,
        long start, long end, ProgressState state,
        Action<long, long>? onProgress, CancellationToken ct)
    {
        var pos = start;
        for (var attempt = 0; attempt <= MaxChunkRetry; attempt++)
        {
            try
            {
                using var resp = await SendAsync(client, url, pos, end, ct).ConfigureAwait(false);
                if (resp.StatusCode != HttpStatusCode.PartialContent)
                    throw new InvalidOperationException($"分片请求返回 {(int)resp.StatusCode}（期望 206）");

                await using var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
                var buffer = new byte[BufferSize];
                while (pos <= end)
                {
                    var want = (int)Math.Min(buffer.Length, end - pos + 1);
                    var n = await src.ReadAsync(buffer.AsMemory(0, want), ct).ConfigureAwait(false);
                    if (n <= 0) break;
                    await RandomAccess.WriteAsync(handle, buffer.AsMemory(0, n), pos, ct).ConfigureAwait(false);
                    pos += n;
                    Interlocked.Add(ref state.Received, n);
                    Report(state, onProgress, force: false);
                }

                if (pos <= end)
                    throw new InvalidOperationException($"分片提前结束（{pos}/{end + 1}）");

                return (true, null);
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception ex)
            {
                if (attempt == MaxChunkRetry) return (false, ex.Message);
                // 断点续抓：pos 已经推进到哪就从哪接着要，不重下已收到的部分
                await Task.Delay(400 * (attempt + 1), ct).ConfigureAwait(false);
            }
        }
        return (false, "未知分片错误");
    }

    /// <summary>消费一个已经拿到的 200 全长响应，边读边写。</summary>
    private static async Task<(bool ok, string? error, long total)> SingleStreamAsync(
        HttpResponseMessage resp, string savePath, Action<long, long>? onProgress, CancellationToken ct)
    {
        var total = resp.Content.Headers.ContentLength ?? -1;
        long got = 0;
        var state = new ProgressState { Total = total };
        try
        {
            await using var src = await resp.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            await using var dst = new FileStream(savePath, FileMode.Create, FileAccess.Write, FileShare.None, BufferSize);
            var buffer = new byte[BufferSize];
            int n;
            while ((n = await src.ReadAsync(buffer, ct).ConfigureAwait(false)) > 0)
            {
                await dst.WriteAsync(buffer.AsMemory(0, n), ct).ConfigureAwait(false);
                got += n;
                state.Received = got;
                Report(state, onProgress, force: false);
            }
            state.Received = got;
            Report(state, onProgress, force: true);
            return (true, null, total > 0 ? total : got);
        }
        catch (OperationCanceledException) { return (false, "已取消", total); }
        catch (Exception ex)
        {
            TryDelete(savePath);
            return (false, ex.Message, total);
        }
    }

    /// <summary>小文件路径：重新发一次不带 Range 的请求（探针那次的 1 字节响应不能复用）。</summary>
    private static async Task<(bool ok, string? error, long total)> SingleStreamAfterProbeAsync(
        HttpClient client, string url, string savePath, Action<long, long>? onProgress, CancellationToken ct)
    {
        try
        {
            using var resp = await client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode) return (false, $"HTTP {(int)resp.StatusCode}", -1);
            return await SingleStreamAsync(resp, savePath, onProgress, ct).ConfigureAwait(false);
        }
        catch (OperationCanceledException) { return (false, "已取消", -1); }
        catch (Exception ex) { return (false, ex.Message, -1); }
    }

    private static Task<HttpResponseMessage> SendAsync(
        HttpClient client, string url, long rangeStart, long rangeEnd, CancellationToken ct)
    {
        var req = new HttpRequestMessage(HttpMethod.Get, url);
        req.Headers.Range = new RangeHeaderValue(rangeStart, rangeEnd);
        // 🔴 分片请求必须显式声明 identity：调用方的 handler 开了 AutomaticDecompression，
        //    一旦服务端对响应做压缩，Content-Length 是压缩后的长度、而字节流是压缩后的内容 ——
        //    Range 的语义当场失效（切出来的偏移全对不上）。zip 本来就不会被 CF 压，
        //    但这里不能靠「本来不会」活着。
        req.Headers.TryAddWithoutValidation("Accept-Encoding", "identity");
        return client.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct);
    }

    private static long ParseTotalFromContentRange(ContentRangeHeaderValue? cr)
    {
        // 形如 "bytes 0-0/11305298"
        if (cr?.Length is { } len && len > 0) return len;
        return -1;
    }

    private sealed class ProgressState
    {
        public long Total = -1;
        public long Received;
        public long LastReportMs;
    }

    private static void Report(ProgressState state, Action<long, long>? onProgress, bool force)
    {
        if (onProgress == null) return;
        if (!force)
        {
            var now = Environment.TickCount64;
            var last = Interlocked.Read(ref state.LastReportMs);
            if (now - last < ReportIntervalMs) return;
            if (Interlocked.CompareExchange(ref state.LastReportMs, now, last) != last) return;
        }
        else
        {
            Interlocked.Exchange(ref state.LastReportMs, Environment.TickCount64);
        }
        onProgress(Interlocked.Read(ref state.Received), state.Total);
    }

    private static Result Fail(string error, Stopwatch sw, int? httpStatus = null) => new()
    {
        Success = false,
        Error = error,
        HttpStatus = httpStatus,
        Seconds = sw.Elapsed.TotalSeconds,
    };

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { }
    }
}
