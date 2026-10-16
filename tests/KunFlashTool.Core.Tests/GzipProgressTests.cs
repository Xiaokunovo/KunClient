using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using KunFlashTool.App.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 用例④：gzip 响应下进度不超 100%（组 D / D1+D2）。
///
/// 病因：D1 打开 AutomaticDecompression 后，<c>Content-Length</c> 变成**压缩后**字节数，
/// 而流里读出来的是**解压后**字节数 —— 不改进度口径的话 downloaded 会远超 totalBytes，
/// 进度条爆表（本用例用「1MB 内容压到 &lt;100KB」把差距拉到 10 倍以上，必现）。
///
/// 服务端用裸 TcpListener 手写一条 HTTP 响应（不经 HttpListener，避免 URL ACL 依赖）。
/// </summary>
public class GzipProgressTests
{
    private static async Task ServeOneGzipAsync(TcpListener listener, byte[] gzipBody)
    {
        using var client = await listener.AcceptTcpClientAsync();
        await using var stream = client.GetStream();

        // 读完请求头即可（只测响应侧进度，不需要请求体）
        var buf = new byte[8192];
        var req = new StringBuilder();
        while (!req.ToString().Contains("\r\n\r\n", StringComparison.Ordinal))
        {
            var n = await stream.ReadAsync(buf);
            if (n == 0) break;
            req.Append(Encoding.ASCII.GetString(buf, 0, n));
        }

        var header = "HTTP/1.1 200 OK\r\n" +
                     "Content-Type: application/octet-stream\r\n" +
                     "Content-Encoding: gzip\r\n" +
                     $"Content-Length: {gzipBody.Length}\r\n" +
                     "Connection: close\r\n\r\n";
        await stream.WriteAsync(Encoding.ASCII.GetBytes(header));
        await stream.WriteAsync(gzipBody);
        await stream.FlushAsync();
    }

    [Fact]
    public async Task GzipResponse_ProgressNeverExceeds100Percent()
    {
        // 可压缩内容：1MB，重复模式 → gzip 后远小于 1/10
        var payload = new byte[1024 * 1024];
        for (var i = 0; i < payload.Length; i++) payload[i] = (byte)(i % 7);

        byte[] gz;
        using (var ms = new MemoryStream())
        {
            using (var gzStream = new GZipStream(ms, CompressionLevel.Fastest, leaveOpen: true))
                gzStream.Write(payload, 0, payload.Length);
            gz = ms.ToArray();
        }
        Assert.True(gz.Length < payload.Length / 10,
            $"gzip 体（{gz.Length}B）应显著小于原始内容（{payload.Length}B），否则测不到爆表");

        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var server = Task.Run(() => ServeOneGzipAsync(listener, gz));
        // 客户端没连上时 server 会卡在 Accept —— 用 faulted 观察者兜住，避免未观察异常
        _ = server.ContinueWith(t => _ = t.Exception, TaskContinuationOptions.OnlyOnFaulted);

        var savePath = Path.Combine(Path.GetTempPath(),
            "kungzip_" + Guid.NewGuid().ToString("N") + ".bin");
        var snapshots = new List<DownloadProgress>();

        try
        {
            using var downloader = new BackgroundDownloader();
            downloader.ProgressChanged += p =>
            {
                // ProgressChanged 复用同一个可变对象 —— 必须拷贝字段
                lock (snapshots)
                {
                    snapshots.Add(new DownloadProgress
                    {
                        BytesReceived = p.BytesReceived,
                        TotalBytes = p.TotalBytes,
                        IsComplete = p.IsComplete,
                        IsError = p.IsError,
                    });
                }
            };

            var ok = await downloader.DownloadFileAsync(new DownloadTask
            {
                Url = $"http://127.0.0.1:{port}/gzip-test",
                SavePath = savePath,
                FileName = "gzip-test.bin",
                Description = "gzip 进度用例",
                Overwrite = true,
                MaxRetries = 0,
            });

            Assert.True(ok, "下载本应成功（服务端已回 200 + gzip 体）");

            // 解压生效：落盘的是解压后的内容，而不是 gzip 字节
            var written = await File.ReadAllBytesAsync(savePath);
            Assert.Equal(payload.Length, written.Length);

            lock (snapshots)
            {
                Assert.NotEmpty(snapshots);
                foreach (var s in snapshots)
                {
                    // D2 口径：压缩响应下 totalBytes 恒为 -1（未知），Percent 随之恒为 -1；
                    // 只有拿到真实总数时才可能有百分比 —— 那种情况下绝不能超 100%。
                    var percent = s.TotalBytes > 0
                        ? (double)s.BytesReceived / s.TotalBytes * 100
                        : -1;
                    Assert.True(percent <= 100, $"进度爆表：{percent:F1}%（received={s.BytesReceived}, total={s.TotalBytes}）");
                    if (s.TotalBytes > 0)
                    {
                        Assert.True(s.BytesReceived <= s.TotalBytes,
                            $"进度爆表：received={s.BytesReceived} > total={s.TotalBytes}");
                    }
                }
                // 压缩响应必须走「总数未知」口径 —— 否则 Content-Length（压缩后）
                // 会远小于解压后已读字节，进度必然爆表（这正是 D2 要修的点）。
                Assert.Contains(snapshots, s => s.TotalBytes <= 0);
            }
        }
        finally
        {
            try { listener.Stop(); } catch { }
            try { if (File.Exists(savePath)) File.Delete(savePath); } catch { }
        }
    }
}