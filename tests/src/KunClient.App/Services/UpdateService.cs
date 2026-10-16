using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;

namespace KunFlashTool.App.Services
{
    public class UpdateInfo
    {
        public string Version { get; set; } = "";
        public string BuildDate { get; set; } = "";
        public string DownloadUrl { get; set; } = "";
        public string Changelog { get; set; } = "";
        public bool ForceUpdate { get; set; }
    }

    public static class UpdateService
    {
        // D4（2026-10-07）：30s 对 7.43MB 的更新包完全不够（1 MB/s 链路下约 7 秒起步，
        // 波动/重试即超时）。改为 10 分钟，与 BackgroundDownloader 的口径一致。
        private static readonly HttpClient httpClient = ServerGate.CreateClient(TimeSpan.FromMinutes(10));
        private static string ServerUrl => KunFlashTool.App.Services.AppConfig.ServerUrl;
        public const string CurrentVersion = "1.1.4.1";

        public static Version CurrentAppVersion => new(CurrentVersion);

        public static async Task<UpdateInfo?> CheckForUpdateAsync()
        {
            try
            {
                using var response = await httpClient.GetAsync($"{ServerUrl}/api/v1/app/version");
                var json = await response.Content.ReadAsStringAsync();
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (!response.IsSuccessStatusCode || !result.GetProperty("success").GetBoolean())
                    return null;

                var data = result.GetProperty("data");
                var latestVersion = data.GetProperty("version").GetString() ?? "";
                var downloadUrl = ResolveDownloadUrl(data.GetProperty("downloadUrl").GetString() ?? "");
                var changelog = data.GetProperty("changelog").GetString() ?? "";
                var buildDate = data.GetProperty("buildDate").GetString() ?? "";
                bool forceUpdate = false;
                if (data.TryGetProperty("forceUpdate", out var forceEl) && forceEl.ValueKind == JsonValueKind.True)
                    forceUpdate = true;

                if (string.IsNullOrEmpty(latestVersion) || string.IsNullOrEmpty(downloadUrl))
                    return null;

                var latest = new Version(latestVersion);
                if (latest <= CurrentAppVersion)
                    return null;

                return new UpdateInfo
                {
                    Version = latestVersion,
                    BuildDate = buildDate,
                    DownloadUrl = downloadUrl,
                    Changelog = changelog,
                    ForceUpdate = forceUpdate
                };
            }
            catch
            {
                return null;
            }
        }

        /// <summary>
        /// 把服务端返回的下载地址补全为绝对 URL。
        /// <para>
        /// <c>/api/app/version</c> 返回的是站内相对路径（如 <c>/assets/KunClient.exe</c>），
        /// 而 <see cref="HttpClient"/> 只接受绝对 URI——直接使用会抛
        /// "相对 URI 不受支持"，表现为"检测到新版本但下载必然失败"。
        /// 无法补全时返回空串，让调用方按"无可用更新"处理，而不是拿坏地址去下载。
        /// </para>
        /// </summary>
        private static string ResolveDownloadUrl(string url)
        {
            if (string.IsNullOrWhiteSpace(url)) return "";

            if (Uri.TryCreate(url, UriKind.Absolute, out var absolute))
                return absolute.ToString();

            var combined = ServerUrl.TrimEnd('/') + "/" + url.TrimStart('/');
            return Uri.TryCreate(combined, UriKind.Absolute, out var full) ? full.ToString() : "";
        }

        public static async Task<bool> DownloadAndApplyUpdateAsync(UpdateInfo update, IProgress<string>? progress = null)
        {
            try
            {
                var tempDir = Path.Combine(Path.GetTempPath(), "KunClient_Update");
                if (Directory.Exists(tempDir))
                    Directory.Delete(tempDir, true);
                Directory.CreateDirectory(tempDir);

                // 按 URL 路径（忽略查询串）判断包类型，避免 "KunClient.exe?v=1" 被误判为 zip
                var isExe = IsExeUrl(update.DownloadUrl);

                // ★ tempExe 必须是"待安装的新程序"的最终落点，后面 File.Copy / 批处理 copy 都只认它。
                //   旧实现把 exe 下成 KunClient_download.exe，而 tempExe 写死是 …\KunClient.exe ——
                //   这个文件**从来没被创建过**：File.Copy(tempExe, …) 抛 FileNotFoundException 被
                //   catch 吞掉 → pendingCopy=true → 批处理里 `copy /Y "…\KunClient.exe" …` 也找不到源文件
                //   → 主程序一次都没被替换，重启后自然还是旧版本（用户报的「升级不生效」根因）。
                //   所以：exe 直接下到 KunClient.exe；只有 zip 才下成 KunClient_update.zip 再解压。
                var tempExe = Path.Combine(tempDir, "KunClient.exe");
                var downloadPath = isExe ? tempExe : Path.Combine(tempDir, "KunClient_update.zip");

                progress?.Report("正在下载更新包...");
                // ResponseHeadersRead：只等响应头就进读循环。原来整包缓冲完才返回，
                // 进度在整个下载期间恒为 0%，读完瞬间跳 100%。
                using (var response = await httpClient.GetAsync(
                    update.DownloadUrl, HttpCompletionOption.ResponseHeadersRead))
                {
                    response.EnsureSuccessStatusCode();
                    var totalBytes = response.Content.Headers.ContentLength ?? -1;
                    var totalRead = 0L;
                    // 8192 → 81920：小缓冲让每字节多一次往返，大包下吞吐明显吃亏。
                    var buffer = new byte[81920];

                    using var stream = await response.Content.ReadAsStreamAsync();
                    using var fileStream = File.Create(downloadPath);

                    while (true)
                    {
                        var read = await stream.ReadAsync(buffer);
                        if (read == 0) break;
                        await fileStream.WriteAsync(buffer.AsMemory(0, read));
                        totalRead += read;

                        if (totalBytes > 0)
                            progress?.Report($"下载进度: {totalRead * 100 / totalBytes}%");
                    }
                }

                if (!isExe)
                {
                    // 更新包为 zip：先解压再定位可执行文件。
                    // 旧实现无论链指向 .exe 还是 .zip 都直接按 exe 复制，zip 包会被原样写入
                    // 并覆盖主程序，导致更新后程序无法启动。
                    progress?.Report("正在解压更新包...");
                    ZipFile.ExtractToDirectory(downloadPath, tempDir, true);

                    var foundExe = string.Empty;
                    foreach (var f in Directory.EnumerateFiles(tempDir, "KunClient.exe", SearchOption.AllDirectories))
                    {
                        foundExe = f;
                        break;
                    }

                    if (string.IsNullOrEmpty(foundExe))
                        throw new Exception("更新包中未找到 KunClient.exe");

                    if (!string.Equals(foundExe, tempExe, StringComparison.OrdinalIgnoreCase))
                        File.Copy(foundExe, tempExe, true);
                }

                // 落盘后确认"它真的是个 Windows 程序"：服务器返回 200 + 错误页/登录页 HTML 时，
                // EnsureSuccessStatusCode() 拦不住，拿这种文件覆盖主程序会让客户端彻底打不开。
                // 校验 PE 头的 'MZ' 两个字节即可，够便宜也够有效。
                if (!File.Exists(tempExe) || new FileInfo(tempExe).Length == 0)
                    throw new Exception("更新包下载后为空，已放弃安装");
                if (!LooksLikeWindowsExe(tempExe))
                    throw new Exception(
                        $"下载到的文件不是有效的 Windows 程序（{new FileInfo(tempExe).Length} 字节）。" +
                        "服务器可能返回了错误页，请到官网手动下载安装包。");

                progress?.Report("正在安装更新...");
                var appDir = AppDomain.CurrentDomain.BaseDirectory;
                var currentExe = Process.GetCurrentProcess().MainModule?.FileName ?? "";

                if (string.IsNullOrEmpty(currentExe))
                    throw new Exception("无法获取当前程序路径");

                var pendingCopy = false;
                try
                {
                    File.Copy(tempExe, currentExe, true);
                }
                catch
                {
                    pendingCopy = true;
                }

                var batPath = Path.Combine(tempDir, "apply_update.bat");
                // 替换失败时留一份可排查的记录（放 LocalAppData，和应用自己的 crash.log 同处）
                var updateLogDir = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KunClient");
                var updateLogPath = Path.Combine(updateLogDir, "update_error.log");

                var sb = new System.Text.StringBuilder();
                sb.AppendLine("@echo off");
                sb.AppendLine("setlocal");
                // 用 ping 代替 timeout：以隐藏窗口 / 无标准输入方式启动批处理时，
                // timeout 会直接报“输入重定向错误”而不等待，导致旧进程未退出就覆盖文件失败。
                sb.AppendLine("ping -n 3 127.0.0.1 >nul");
                // 先按 PID 精确结束“我们自己这个进程”，再按映像名兜底（用户可能开了多个实例）。
                // 只按名字 taskkill 一旦漏杀，下面的 copy 必然失败、主程序就停在旧版本 ——
                // 这正是“更新完重启还是旧版本”的另一种成因。
                sb.AppendLine($"taskkill /f /pid {Environment.ProcessId} >nul 2>&1");
                sb.AppendLine("taskkill /f /im \"KunClient.exe\" >nul 2>&1");
                sb.AppendLine("ping -n 2 127.0.0.1 >nul");
                // 覆盖主程序：杀毒 / 索引器可能短暂占用文件，失败就重试（约 45 秒上限）。
                // 旧实现只 copy 一次、还把 stderr 丢掉，失败后照样 start 旧程序 ——
                // 用户看到的就是“更新了但版本没变”，且没有任何可查的痕迹。
                sb.AppendLine("set /a tries=0");
                sb.AppendLine(":retry");
                sb.AppendLine($"copy /Y \"{tempExe}\" \"{currentExe}\" >nul 2>&1 && goto copied");
                sb.AppendLine("set /a tries+=1");
                sb.AppendLine("if %tries% GEQ 15 goto failed");
                sb.AppendLine("ping -n 3 127.0.0.1 >nul");
                sb.AppendLine("goto retry");
                sb.AppendLine(":copied");
                sb.AppendLine($"cd /d \"{appDir}\"");
                sb.AppendLine($"start \"\" \"{currentExe}\"");
                sb.AppendLine("ping -n 3 127.0.0.1 >nul");
                sb.AppendLine($"rmdir /s /q \"{tempDir}\" >nul 2>&1");
                sb.AppendLine("del /f /q \"%~f0\" >nul 2>&1");
                sb.AppendLine("exit /b");
                sb.AppendLine(":failed");
                // 替换失败**不要**把旧程序删掉：直接把旧版本重启回来，并留一条记录。
                sb.AppendLine($"if not exist \"{updateLogDir}\" md \"{updateLogDir}\" >nul 2>&1");
                sb.AppendLine($"echo [%date% %time%] replace failed: \"{tempExe}\" to \"{currentExe}\" >> \"{updateLogPath}\"");
                sb.AppendLine($"cd /d \"{appDir}\"");
                sb.AppendLine($"start \"\" \"{currentExe}\"");
                sb.AppendLine("del /f /q \"%~f0\" >nul 2>&1");
                sb.AppendLine("exit /b");

                // 批处理按系统 ANSI 代码页写盘：.NET Core 起 Encoding.Default 为 UTF-8，
                // 而 cmd.exe 默认按 GBK 解析脚本，安装路径含中文时会导致路径乱码、更新失败。
                File.WriteAllText(batPath, sb.ToString(), ResolveBatEncoding());

                if (pendingCopy)
                {
                    // 通过 cmd /c 启动批处理（UseShellExecute=false），避免弹出控制台黑窗；
                    // 直接以 UseShellExecute=true 启动 .bat 会忽略 CreateNoWindow。
                    Process.Start(new ProcessStartInfo
                    {
                        FileName = "cmd.exe",
                        Arguments = $"/c \"\"{batPath}\"\"",
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        WindowStyle = ProcessWindowStyle.Hidden
                    });
                }

                progress?.Report("更新完成，正在重启...");
                await Task.Delay(800);

                if (!pendingCopy)
                {
                    Process.Start(new ProcessStartInfo
                    {
                        FileName = currentExe,
                        UseShellExecute = true
                    });
                }

                System.Windows.Application.Current.Dispatcher.Invoke(() =>
                {
                    System.Windows.Application.Current.Shutdown();
                });

                return true;
            }
            catch (Exception ex)
            {
                progress?.Report($"更新失败: {ex.Message}");
                return false;
            }
        }

        /// <summary>
        /// 粗校验：文件是否以 PE 镜像标志 <c>MZ</c> 开头。
        /// <para>
        /// 用于挡住“服务器返回 HTTP 200 + 错误页/登录页 HTML”被当成更新包 ——
        /// 这种文件一旦覆盖主程序，客户端就再也打不开了。只读两个字节，成本可忽略。
        /// </para>
        /// </summary>
        private static bool LooksLikeWindowsExe(string path)
        {
            try
            {
                using var fs = File.OpenRead(path);
                var head = new byte[2];
                if (fs.Read(head, 0, 2) != 2) return false;
                return head[0] == (byte)'M' && head[1] == (byte)'Z';
            }
            catch
            {
                return false;
            }
        }

        /// <summary>判断下载地址指向的是否为可执行文件（忽略 URL 查询串与锚点）。</summary>
        private static bool IsExeUrl(string url)
        {
            if (string.IsNullOrWhiteSpace(url)) return false;
            var path = url;
            var cut = path.IndexOfAny(new[] { '?', '#' });
            if (cut >= 0) path = path.Substring(0, cut);
            return path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>
        /// 批处理文件编码：优先使用系统 ANSI 代码页（与 cmd.exe 默认代码页一致，
        /// 保证含中文的安装路径可被正确解析），不支持时回退 UTF-8。
        /// </summary>
        private static System.Text.Encoding ResolveBatEncoding()
        {
            try
            {
                var ansiPage = System.Globalization.CultureInfo.CurrentCulture.TextInfo.ANSICodePage;
                if (ansiPage > 0)
                    return System.Text.Encoding.GetEncoding(ansiPage);
            }
            catch
            {
                // 运行时不支持该代码页（缺少 CodePages provider）时回退
            }
            return new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false);
        }
    }
}
