using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using System.Reflection;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KunFlashTool.Core.Configuration;
using KunFlashTool.Core.Net;

namespace KunFlashTool.Core.PlatformTools
{
    public class PlatformToolsManager
    {
        private readonly Action<string>? _log;
        private readonly string _toolsDirectory;
        // D7（2026-10-07）：tools.zip 走 base64 JSON 时体积 ~6.8MB，2 分钟在慢链路上会超时 → 10 分钟。
        private static readonly HttpClient httpClient = CreateHttpClient();

        /// <summary>
        /// 与 BackgroundDownloader.CreateSharedClient 同款（D1/D3 配套）：
        /// <c>/api/packages/tools</c> 返回的是 JSON，CF 只在客户端发 <c>Accept-Encoding</c> 时才压缩
        /// （实测 -24.4%）。不打开压缩，D3 的进度守卫永远不会触发，也拿不到这份收益。
        /// </summary>
        private static HttpClient CreateHttpClient()
        {
            var handler = new SocketsHttpHandler
            {
                AutomaticDecompression = System.Net.DecompressionMethods.GZip
                                       | System.Net.DecompressionMethods.Deflate,
                PooledConnectionLifetime = TimeSpan.FromMinutes(5),
                ConnectTimeout = TimeSpan.FromSeconds(15),
                MaxConnectionsPerServer = 8,
            };
            return new HttpClient(handler) { Timeout = TimeSpan.FromMinutes(10) };
        }
        /// <summary>工具包下载地址来自软配置（环境变量 KUNCLIENT_SERVER_URL > KunClient.config.local.json > KunClient.config.json > 默认值）</summary>
        private static string ServerUrl => Configuration.ClientConfig.ServerUrl;

        public PlatformToolsManager(Action<string>? log = null)
        {
            _log = log;
            _toolsDirectory = ResolveToolsDirectory();
            EnsureToolsDirectory();
        }

        /// <summary>
        /// 工具目录 = <c>%LOCALAPPDATA%\KunClientState\cache\platform-tools</c>（**跨会话保留**）。
        /// <para>🔴 2026-10-08：原来在 <c>%TEMP%\KunClient\tools</c>，而 <c>App.OnExit</c> 会把
        /// <c>%TEMP%\KunClient</c> 整目录删掉 —— 于是 adb/fastboot 每次启动都要重下 5.1MB。
        /// 现在放状态目录下，并用 <see cref="AppStatePaths.ResolveCache"/> 把老位置已有的工具
        /// 一次性搬过来，升级用户不用为这次改动白下一次。</para>
        /// </summary>
        private static string ResolveToolsDirectory()
            => AppStatePaths.ResolveCache("platform-tools", "tools", "adb.exe");

        public void EnsureToolsDirectory()
        {
            if (!Directory.Exists(_toolsDirectory))
            {
                Directory.CreateDirectory(_toolsDirectory);
                _log?.Invoke($"创建工具目录: {_toolsDirectory}");
            }
        }

        public string GetAdbPath() => Path.Combine(_toolsDirectory, "adb.exe");
        public string GetFastbootPath() => Path.Combine(_toolsDirectory, "fastboot.exe");

        public bool CheckToolsExist()
        {
            var adbExists = File.Exists(GetAdbPath());
            var fastbootExists = File.Exists(GetFastbootPath());
            _log?.Invoke($"ADB: {adbExists}, Fastboot: {fastbootExists}");
            return adbExists && fastbootExists;
        }

        /// <summary>
        /// 确保平台工具（adb / fastboot）存在，必要时从服务器下载并解压。
        /// </summary>
        /// <param name="onProgress">
        /// 下载进度回调：(<b>已下载字节</b>, <b>总字节</b>；服务端没给 Content-Length 时总数为 -1）。
        /// 🔴 这个参数是**必需**的：原实现用 <c>GetStringAsync</c> 一次性把整个包读成字符串，
        ///    界面上在整个下载+解压期间只能显示 0%，成功瞬间跳 100% —— 那不是"进度慢"，
        ///    是**根本没有进度可报**（用户反馈"工具加载的进度条不真实"就是这条）。
        /// </param>
        /// <remarks>
        /// 2026-10-08（用户反馈「下载速度太慢」）：单流 → <see cref="ChunkedDownloader"/> 默认 6 连接分片，
        /// 并把端点顺序改成「<c>/assets/tools.zip</c>（CDN 边缘缓存）→ <c>/api/packages/tools/file</c>
        /// → base64 JSON」，逐级回落。实测 11.3MB 的 scrcpy 包：单连接 31~44s → 分片 8~20s。
        /// </remarks>
        public async Task<bool> EnsureToolsExistAsync(Action<long, long>? onProgress = null)
        {
            if (CheckToolsExist()) return true;

            _log?.Invoke("辅助工具未找到，正在从服务器下载...");
            var zipPath = Path.Combine(_toolsDirectory, "tools.zip");
            try
            {
                long received = 0;
                long total = -1;

                // ── ① 边缘缓存静态路径（CF 按 .zip 扩展名缓存 → 命中边缘）──
                var raw = await ChunkedDownloader.DownloadAsync(
                    httpClient, $"{ServerUrl}/assets/tools.zip", zipPath,
                    ChunkedDownloader.DefaultParallelism, onProgress, CancellationToken.None).ConfigureAwait(false);
                total = raw.TotalBytes;
                received = raw.TotalBytes;

                // ── ② 二进制 API 端点（老服务端 /assets 下没放 zip 时）──
                if (!raw.Success)
                {
                    _log?.Invoke($"静态路径不可用（{raw.Error}），改用二进制端点…");
                    raw = await ChunkedDownloader.DownloadAsync(
                        httpClient, $"{ServerUrl}/api/v2/packages/tools/file", zipPath,
                        ChunkedDownloader.DefaultParallelism, onProgress, CancellationToken.None).ConfigureAwait(false);
                    total = raw.TotalBytes;
                    received = raw.TotalBytes;
                }

                // ── ③ base64 JSON 回落（更老的服务端只有这一种形态）──
                if (!raw.Success)
                {
                    _log?.Invoke("二进制端点不可用，回落到 base64 端点…");
                    var rawPath = Path.Combine(_toolsDirectory, "tools.payload");
                    long got = 0;
                    long respTotal;
                    using (var resp = await httpClient.GetAsync($"{ServerUrl}/api/v2/packages/tools",
                                   HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false))
                    {
                        resp.EnsureSuccessStatusCode();
                        // D3（同 D2）：压缩响应下 Content-Length 是压缩后字节数，而读出来的是解压后的
                        // JSON 字节 —— 拿它当总数会让进度超过 100%。这时按「总数未知」(-1) 上报。
                        var respCe = resp.Content.Headers.ContentEncoding;
                        var respCompressed = respCe != null && respCe.Count > 0
                              && !string.Equals(respCe.First(), "identity", StringComparison.OrdinalIgnoreCase);
                        respTotal = respCompressed ? -1L : (resp.Content.Headers.ContentLength ?? -1L);
                        total = respTotal;
                        onProgress?.Invoke(0, total);

                        await using var src = await resp.Content.ReadAsStreamAsync().ConfigureAwait(false);
                        await using (var dst = File.Create(rawPath))
                        {
                            var buffer = new byte[81920];
                            int n;
                            while ((n = await src.ReadAsync(buffer, 0, buffer.Length).ConfigureAwait(false)) > 0)
                            {
                                await dst.WriteAsync(buffer, 0, n).ConfigureAwait(false);
                                got += n;
                                onProgress?.Invoke(got, total);
                            }
                        }
                    }
                    received = got;

                    // 服务端这个端点返回的是「JSON 里塞 base64 的 zip」，体积膨胀约 33%。
                    // 真正的二进制端点还没上线时只能走这里；网络读取已是流式（进度真实）。
                    _log?.Invoke("正在解码 base64 包...");
                    using var fs = File.OpenRead(rawPath);
                    using var ms = new MemoryStream();
                    await fs.CopyToAsync(ms).ConfigureAwait(false);
                    var json = JsonSerializer.Deserialize<JsonElement>(System.Text.Encoding.UTF8.GetString(ms.ToArray()));
                    if (!json.GetProperty("success").GetBoolean())
                    {
                        _log?.Invoke("服务器返回失败");
                        TryDelete(rawPath);
                        return false;
                    }

                    var base64Data = json.GetProperty("data").GetString()!;
                    var zipBytes = Convert.FromBase64String(base64Data);
                    await File.WriteAllBytesAsync(zipPath, zipBytes).ConfigureAwait(false);
                    TryDelete(rawPath);
                }

                _log?.Invoke("正在解压工具...");
                ZipFile.ExtractToDirectory(zipPath, _toolsDirectory, true);
                TryDelete(zipPath);

                // 解压阶段也补一报：让进度条走满而不是停在 100% 之前不动
                onProgress?.Invoke(total > 0 ? total : received, total > 0 ? total : received);

                _log?.Invoke("工具下载完成");
                return CheckToolsExist();
            }
            catch (Exception ex)
            {
                _log?.Invoke($"下载工具失败: {ex.Message}");
                TryDelete(zipPath);
                TryDelete(Path.Combine(_toolsDirectory, "tools.payload"));
                return false;
            }
        }

        /// <summary>删文件失败一律静默：清理是尽力而为，不能因为清不掉就让下载判失败。</summary>
        private static void TryDelete(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); } catch { }
        }

        public void KillAdbServer(int timeoutMs = 4000)
        {
            try
            {
                if (File.Exists(GetAdbPath()))
                {
                    using var process = new Process
                    {
                        StartInfo = new ProcessStartInfo
                        {
                            FileName = GetAdbPath(),
                            Arguments = "kill-server",
                            UseShellExecute = false,
                            CreateNoWindow = true,
                        },
                    };
                    process.Start();
                    if (!process.WaitForExit(timeoutMs))
                    {
                        try { process.Kill(); } catch { }
                    }
                }
            }
            catch (Exception ex)
            {
                _log?.Invoke($"关闭 ADB 服务失败: {ex.Message}");
            }

            try
            {
                var ourAdb = GetAdbPath();
                foreach (var process in Process.GetProcessesByName("adb"))
                {
                    try
                    {
                        var path = process.MainModule?.FileName;
                        if (string.Equals(path, ourAdb, StringComparison.OrdinalIgnoreCase))
                        {
                            process.Kill();
                            _log?.Invoke("已关闭随工具启动的 adb 服务进程。");
                        }
                    }
                    catch { }
                    finally { process.Dispose(); }
                }
            }
            catch { }
        }

        public async Task<(bool success, string output, string error)> ExecuteAdbAsync(string arguments, int timeoutMs = 30000, string? deviceSerial = null)
        {
            try
            {
                var adbPath = GetAdbPath();
                if (!File.Exists(adbPath))
                    return (false, "", $"ADB工具不存在: {adbPath}");

                _log?.Invoke($"执行ADB命令: adb {arguments}");

                using var process = new Process
                {
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = adbPath,
                        Arguments = deviceSerial != null ? $"-s {deviceSerial} {arguments}" : arguments,
                        UseShellExecute = false,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        CreateNoWindow = true,
                        WorkingDirectory = _toolsDirectory
                    }
                };

                process.Start();
                var outputTask = process.StandardOutput.ReadToEndAsync();
                var errorTask = process.StandardError.ReadToEndAsync();

                using var cts = new CancellationTokenSource(timeoutMs > 0 ? timeoutMs : Timeout.Infinite);
                try
                {
                    await process.WaitForExitAsync(cts.Token);
                }
                catch (OperationCanceledException)
                {
                    TryKill(process);
                    _log?.Invoke($"ADB命令超时（{timeoutMs}ms）: adb {arguments}");
                    return (false, "", $"ADB命令执行超时（{timeoutMs}ms）");
                }

                var output = await outputTask;
                var error = await errorTask;
                _log?.Invoke($"ADB 命令完成，退出码: {process.ExitCode}");
                return (process.ExitCode == 0, output, error);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"ADB执行异常: {ex.Message}");
                return (false, "", ex.Message);
            }
        }

        public async Task<(bool success, string output, string error)> ExecuteFastbootAsync(string arguments, int timeoutMs = 30000, string? deviceSerial = null)
        {
            try
            {
                var fastbootPath = GetFastbootPath();
                if (!File.Exists(fastbootPath))
                    return (false, "", $"Fastboot工具不存在: {fastbootPath}");

                _log?.Invoke($"执行Fastboot命令: fastboot {arguments}");

                using var process = new Process
                {
                    StartInfo = new ProcessStartInfo
                    {
                        FileName = fastbootPath,
                        Arguments = deviceSerial != null ? $"-s {deviceSerial} {arguments}" : arguments,
                        UseShellExecute = false,
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        CreateNoWindow = true,
                        WorkingDirectory = _toolsDirectory
                    }
                };

                process.Start();
                var outputTask = process.StandardOutput.ReadToEndAsync();
                var errorTask = process.StandardError.ReadToEndAsync();

                using var cts = new CancellationTokenSource(timeoutMs > 0 ? timeoutMs : Timeout.Infinite);
                try
                {
                    await process.WaitForExitAsync(cts.Token);
                }
                catch (OperationCanceledException)
                {
                    TryKill(process);
                    _log?.Invoke($"Fastboot命令超时（{timeoutMs}ms）: fastboot {arguments}");
                    return (false, "", $"Fastboot命令执行超时（{timeoutMs}ms）");
                }

                var output = await outputTask;
                var error = await errorTask;
                _log?.Invoke($"Fastboot 命令完成，退出码: {process.ExitCode}");
                return (process.ExitCode == 0, output, error);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"Fastboot执行异常: {ex.Message}");
                return (false, "", ex.Message);
            }
        }

        /// <summary>安全终止进程，失败时静默忽略。</summary>
        private void TryKill(Process process)
        {
            try
            {
                if (!process.HasExited)
                {
                    process.Kill();
                }
            }
            catch
            {
                // 进程可能已退出或无权终止，忽略
            }
        }

        /// <summary>
        /// 流式执行一个 Windows 批处理脚本（cmd /c call），每行输出实时回调。
        /// 为「小米线刷」服务：把平台工具目录前置进 PATH，脚本里的 fastboot/adb 才会命中我们自带的工具，
        /// 而不是用户机器上参差不齐的环境变量版本。
        /// </summary>
        /// <param name="scriptPath">bat 脚本完整路径（如 ROM 目录下的 flash_all.bat）</param>
        /// <param name="workingDirectory">工作目录（= ROM 目录，脚本用 %~dp0 定位镜像，此参数是双保险）</param>
        /// <param name="onLine">每行输出回调（在线程池线程触发，UI 侧自行调度）</param>
        /// <param name="ct">取消令牌（取消时终止整棵进程树）</param>
        /// <param name="timeoutMs">总超时，0 = 不限</param>
        public async Task<(int exitCode, bool cancelled)> ExecuteScriptStreamingAsync(
            string scriptPath, string workingDirectory, Action<string>? onLine, CancellationToken ct, int timeoutMs = 0)
        {
            if (!File.Exists(scriptPath))
                return (-1, false);

            var psi = new ProcessStartInfo
            {
                // 用 call 而不是直接引号包脚本路径：cmd /c 对带空格路径的引号裁剪规则很怪，call 语义最稳
                FileName = "cmd.exe",
                Arguments = $"/c call \"{scriptPath}\"",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                WorkingDirectory = string.IsNullOrEmpty(workingDirectory) ? _toolsDirectory : workingDirectory,
            };
            // fastboot/adb 前置：脚本里裸调 fastboot 时优先解析到我们自带的版本
            var path = Environment.GetEnvironmentVariable("PATH") ?? "";
            psi.EnvironmentVariables["PATH"] = _toolsDirectory + Path.PathSeparator + path;

            _log?.Invoke($"执行脚本: {scriptPath}");
            using var process = new Process { StartInfo = psi };
            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                _log?.Invoke($"脚本启动失败: {ex.Message}");
                return (-1, false);
            }

            using var timeoutCts = timeoutMs > 0 ? new CancellationTokenSource(timeoutMs) : new CancellationTokenSource();
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(ct, timeoutCts.Token);

            var outTask = PumpLinesAsync(process.StandardOutput, onLine);
            var errTask = PumpLinesAsync(process.StandardError, onLine);

            var cancelled = false;
            try
            {
                await process.WaitForExitAsync(linked.Token);
            }
            catch (OperationCanceledException)
            {
                cancelled = true;
                TryKillTree(process);
            }

            try { await Task.WhenAll(outTask, errTask); } catch { /* 进程被杀时读取可能抛异常，忽略 */ }

            var exitCode = SafeExitCode(process);
            _log?.Invoke($"脚本执行结束，退出码: {exitCode}" + (cancelled ? "（已取消）" : ""));
            return (exitCode, cancelled);
        }

        /// <summary>
        /// 用系统自带 tar.exe（Windows 10 1803+）解压 .tgz / .tar.gz 线刷包。
        /// </summary>
        public async Task<(bool success, string error)> ExtractTarGzAsync(
            string archivePath, string destDir, Action<string>? onLine, CancellationToken ct, int timeoutMs = 15 * 60_000)
        {
            var tar = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
            if (!File.Exists(tar))
                return (false, "未找到系统 tar.exe（需要 Windows 10 1803+），请手动解压后选择目录");
            if (!File.Exists(archivePath))
                return (false, $"压缩包不存在: {archivePath}");

            try
            {
                Directory.CreateDirectory(destDir);
            }
            catch (Exception ex)
            {
                return (false, $"创建解压目录失败: {ex.Message}");
            }

            var psi = new ProcessStartInfo
            {
                FileName = tar,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                WorkingDirectory = destDir,
            };
            psi.ArgumentList.Add("-xzf");
            psi.ArgumentList.Add(archivePath);
            psi.ArgumentList.Add("-C");
            psi.ArgumentList.Add(destDir);

            _log?.Invoke($"解压线刷包: {archivePath}");
            using var process = new Process { StartInfo = psi };
            try
            {
                process.Start();
            }
            catch (Exception ex)
            {
                return (false, $"tar 启动失败: {ex.Message}");
            }

            using var timeoutCts = new CancellationTokenSource(timeoutMs);
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(ct, timeoutCts.Token);
            var outTask = PumpLinesAsync(process.StandardOutput, onLine);
            var errTask = PumpLinesAsync(process.StandardError, onLine);

            var cancelled = false;
            try
            {
                await process.WaitForExitAsync(linked.Token);
            }
            catch (OperationCanceledException)
            {
                cancelled = true;
                TryKillTree(process);
            }
            try { await Task.WhenAll(outTask, errTask); } catch { }

            if (cancelled)
                return (false, "解压已取消");
            var code = SafeExitCode(process);
            return code == 0
                ? (true, "")
                : (false, $"tar 退出码 {code}");
        }

        /// <summary>逐行泵出子进程输出，行尾即回调；进程退出后自然结束。</summary>
        private static async Task PumpLinesAsync(StreamReader reader, Action<string>? onLine)
        {
            try
            {
                while (await reader.ReadLineAsync() is { } line)
                {
                    if (line.Length > 0) onLine?.Invoke(line);
                }
            }
            catch
            {
                // 进程被终止时流可能中断，静默收尾
            }
        }

        /// <summary>终止整棵进程树（cmd → fastboot 子进程一并杀掉）。</summary>
        private void TryKillTree(Process process)
        {
            try
            {
                if (!process.HasExited)
                    process.Kill(entireProcessTree: true);
            }
            catch (Exception ex)
            {
                _log?.Invoke($"终止进程树失败: {ex.Message}");
            }
        }

        /// <summary>读退出码兜底：进程尚未退出（刚被杀）时返回 -1。</summary>
        private int SafeExitCode(Process process)
        {
            try
            {
                if (!process.HasExited)
                    process.WaitForExit(3000);
                return process.HasExited ? process.ExitCode : -1;
            }
            catch
            {
                return -1;
            }
        }

        public async Task<bool> CheckAdbDeviceAsync()
        {
            var (success, output, _) = await ExecuteAdbAsync("devices");
            if (success && output.Contains("device")) { _log?.Invoke("检测到ADB设备连接"); return true; }
            _log?.Invoke("未检测到ADB设备连接");
            return false;
        }

        public async Task<bool> CheckFastbootDeviceAsync()
        {
            var (success, output, _) = await ExecuteFastbootAsync("devices");
            if (success && output.Contains("fastboot")) { _log?.Invoke("检测到Fastboot设备连接"); return true; }
            _log?.Invoke("未检测到Fastboot设备连接");
            return false;
        }

        public async Task<List<string>> GetAdbDevicesAsync()
        {
            var (success, output, _) = await ExecuteAdbAsync("devices");
            var devices = new List<string>();
            if (success) { foreach (var line in output.Split('\n')) { if (line.Contains("device") && !line.Contains("List")) devices.Add(line.Trim()); } }
            return devices;
        }

        public async Task<List<string>> GetFastbootDevicesAsync()
        {
            var (success, output, _) = await ExecuteFastbootAsync("devices");
            var devices = new List<string>();
            if (success) { foreach (var line in output.Split('\n')) { if (line.Contains("fastboot") && !line.Contains("List")) devices.Add(line.Trim()); } }
            return devices;
        }
    }
}
