using System.Diagnostics;
using KunFlashTool.Core.PlatformTools;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 「小米线刷」Core 能力测试：
/// ① ExecuteScriptStreamingAsync 流式回调 + 退出码；
/// ② 取消令牌终止整棵进程树（cmd → ping 子进程一并死）；
/// ③ ExtractTarGzAsync 用系统 tar 走 真实打包→解压 往返。
/// 全部用临时目录，跑完即清。
/// </summary>
public class PlatformScriptExecutionTests
{
    private static string TempDir()
    {
        var dir = Path.Combine(Path.GetTempPath(), "KunClientTests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        return dir;
    }

    [Fact]
    public async Task Script_StreamsLines_AndReturnsExitCode()
    {
        var dir = TempDir();
        try
        {
            var script = Path.Combine(dir, "flash_all.bat");
            await File.WriteAllTextAsync(script,
                "@echo off\r\necho step1\r\necho Sending 'boot' (128 MB)\r\nexit /b 0\r\n");

            var lines = new List<string>();
            var mgr = new PlatformToolsManager();
            var (exitCode, cancelled) = await mgr.ExecuteScriptStreamingAsync(script, dir, lines.Add, CancellationToken.None);

            Assert.False(cancelled);
            Assert.Equal(0, exitCode);
            Assert.Contains(lines, l => l.Contains("step1"));
            Assert.Contains(lines, l => l.Contains("Sending 'boot'"));
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public async Task Script_NonZeroExitCode_Propagates()
    {
        var dir = TempDir();
        try
        {
            var script = Path.Combine(dir, "flash_all.bat");
            await File.WriteAllTextAsync(script, "@echo off\r\nexit /b 7\r\n");

            var mgr = new PlatformToolsManager();
            var (exitCode, cancelled) = await mgr.ExecuteScriptStreamingAsync(script, dir, _ => { }, CancellationToken.None);

            Assert.False(cancelled);
            Assert.Equal(7, exitCode);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public async Task Script_Cancel_KillsWholeTree()
    {
        var dir = TempDir();
        try
        {
            // ping -n 30 撑 30 秒：cmd 是父、ping 是子，取消后两者都必须死
            var script = Path.Combine(dir, "long.bat");
            await File.WriteAllTextAsync(script, "@echo off\r\nping -n 30 127.0.0.1 > nul\r\n");

            var mgr = new PlatformToolsManager();
            using var cts = new CancellationTokenSource(1200);
            var sw = Stopwatch.StartNew();
            var (exitCode, cancelled) = await mgr.ExecuteScriptStreamingAsync(script, dir, _ => { }, cts.Token);

            Assert.True(cancelled);
            Assert.True(sw.ElapsedMilliseconds < 10000, $"取消应立即生效，实际 {sw.ElapsedMilliseconds}ms");
            await Task.Delay(500); // 给进程树死亡留点时间
            Assert.Equal(0, Process.GetProcessesByName("ping").Count(p =>
            {
                try { return p.MainModule?.FileName?.Contains("System32", StringComparison.OrdinalIgnoreCase) == true; }
                catch { return false; }
            }));
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { }
        }
    }

    [Fact]
    public async Task MissingScript_ReturnsMinusOne()
    {
        var mgr = new PlatformToolsManager();
        var (exitCode, cancelled) = await mgr.ExecuteScriptStreamingAsync(
            Path.Combine(Path.GetTempPath(), "no_such_flash_all.bat"), null, _ => { }, CancellationToken.None);
        Assert.Equal(-1, exitCode);
        Assert.False(cancelled);
    }

    [Fact]
    public async Task TarGz_ExtractRoundTrip()
    {
        var src = TempDir();
        var dest = TempDir();
        try
        {
            var payload = Path.Combine(src, "images");
            Directory.CreateDirectory(payload);
            await File.WriteAllTextAsync(Path.Combine(payload, "boot.img"), "fake-boot");
            await File.WriteAllTextAsync(Path.Combine(src, "flash_all.bat"), "@echo off\r\n");

            var tar = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "tar.exe");
            var archive = Path.Combine(src, "rom.tgz");
            var pack = Process.Start(new ProcessStartInfo
            {
                FileName = tar,
                ArgumentList = { "-czf", archive, "-C", src, "images", "flash_all.bat" },
                CreateNoWindow = true,
                UseShellExecute = false,
            });
            pack!.WaitForExit();
            Assert.Equal(0, pack.ExitCode);

            var mgr = new PlatformToolsManager();
            var (ok, error) = await mgr.ExtractTarGzAsync(archive, dest, null, CancellationToken.None);

            Assert.True(ok, error);
            Assert.Equal("fake-boot", await File.ReadAllTextAsync(Path.Combine(dest, "images", "boot.img")));
            Assert.True(File.Exists(Path.Combine(dest, "flash_all.bat")));
        }
        finally
        {
            try { Directory.Delete(src, recursive: true); } catch { }
            try { Directory.Delete(dest, recursive: true); } catch { }
        }
    }
}
