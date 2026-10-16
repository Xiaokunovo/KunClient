using System;
using System.Collections.Generic;
using System.IO;
using KunFlashTool.Core.Configuration;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 服务器地址回归护栏。
///
/// 血泪史（2026-09-26）：注册全线失败，最后查出根因是**域名选错** ——
/// <c>kunawa.xyz</c> 被 Cloudflare 挂上了 Managed Challenge（人机校验），
/// 客户端（HttpClient / urllib）**不执行 JS** → 所有接口返回 403 挑战页 → 注册/登录全挂。
/// 表现与「服务器宕机」完全一样，极易误判。
///
/// 所以这里用源码级断言把两条红线钉住：
///   1. 兜底地址必须是真实可用的域名（不能再是 http://127.0.0.1:3000，那只会静默连不上）；
///   2. 内嵌配置 **禁止** 出现 kunawa.xyz。
/// </summary>
public sealed class ServerUrlGuardTests
{
    [Fact]
    public void DefaultServerUrl_IsRealHttpsDomain_NotLocalhost()
    {
        var url = ClientConfig.DefaultServerUrl;
        Assert.StartsWith("https://", url, StringComparison.Ordinal);
        Assert.DoesNotContain("127.0.0.1", url, StringComparison.Ordinal);
        Assert.DoesNotContain("localhost", url, StringComparison.Ordinal);
        // 裸 IP + 非标端口（3000）在家宽/公司网/运营商常被封，禁止作为兜底
        Assert.DoesNotContain(":3000", url, StringComparison.Ordinal);
    }

    [Fact]
    public void EmbeddedConfig_MustNotUseCloudflareChallengedDomain()
    {
        var root = FindRepoRoot();
        var embedded = Path.Combine(root, "tests", "src", "KunClient.App", "KunClient.config.json");
        Assert.True(File.Exists(embedded), $"内嵌配置不存在: {embedded}");
        var text = File.ReadAllText(embedded);

        Assert.False(
            text.Contains("kunawa.xyz", StringComparison.OrdinalIgnoreCase),
            "内嵌配置禁止使用 kunawa.xyz —— 该域被 Cloudflare Managed Challenge 拦截，" +
            "客户端不执行 JS 会收到 403，注册/登录全部失败。");
        Assert.Contains("https://", text, StringComparison.Ordinal);
        Assert.DoesNotContain("127.0.0.1", text, StringComparison.Ordinal);
    }

    /// <summary>
    /// 红线要覆盖**所有活跃面**：C#（客户端 / 核心）、服务端 JS、官网 TS/JSON。
    ///
    /// 原来这条断言盯的是 <c>package/python/kunflash/client_config.py</c>，但 PySide6 前端已在
    /// 2026-09 冻结、整个 <c>package/</c> 目录已从仓库移除 → 路径不存在 → 断言永远失败。
    /// **常年红的护栏比没有护栏更糟**：它会掩盖真正的回归（这次就是靠上面 119 条才发现它）。
    /// 所以改成「扫活跃源码树里还有没有 kunawa.xyz」，与具体文件解耦，路径怎么变都不会烂。
    /// </summary>
    [Fact]
    public void NoActiveSourceSurface_UsesCloudflareChallengedDomain()
    {
        var root = FindRepoRoot();
        var offenders = new List<string>();

        foreach (var dir in new[] { Path.Combine("tests", "src"), "server", "website", "package" })
        {
            var full = Path.Combine(root, dir);
            if (!Directory.Exists(full)) continue;

            foreach (var file in Directory.EnumerateFiles(full, "*", SearchOption.AllDirectories))
            {
                if (IsBuildOutput(file) || !IsSourceLike(file)) continue;

                foreach (var (line, no) in ReadCodeLines(file))
                {
                    if (line.Contains("kunawa.xyz", StringComparison.OrdinalIgnoreCase))
                        offenders.Add($"{Path.GetRelativePath(root, file).Replace('\\', '/')}:{no}  {line.Trim()}");
                }
            }
        }

        Assert.True(offenders.Count == 0,
            "以下文件仍在使用 kunawa.xyz —— 该域被 Cloudflare Managed Challenge 拦截，" +
            "客户端不执行 JS 会收到 403（表现与宕机一模一样）：\n  " +
            string.Join("\n  ", offenders));
    }

    // ══════════════════════════════════════════════════════════════════
    // 主机白名单（2026-10-09 逆向报告 D6 之后加）
    //
    // 原状：任意 https 主机都会被接受。而 %LOCALAPPDATA% 下的 KunClient.config.json
    //       **任何以当前用户身份运行的进程都能改**，/api/login 提交的又是明文密码、
    //       之后每个请求都带 Bearer token ⇒ 无需改 exe 就能把凭据静默导向别处。
    // 判据：不写死"应当被拒的具体域名字符串"之外的东西，全部走 ClientConfig.Accept，
    //       这样以后换官方域名只需改白名单一处，护栏不会先烂。
    // ══════════════════════════════════════════════════════════════════

    [Theory]
    [InlineData("https://evil.example.com")]
    [InlineData("https://kunawa.cn.evil.com")]      // 后缀伪造：只写 EndsWith 会误放行，必须挡住
    [InlineData("https://evil-kunawa.cn")]          // 前缀伪造
    [InlineData("https://attacker.io")]
    public void Accept_RejectsUntrustedRemoteHost(string url)
        => Assert.Null(ClientConfig.Accept(url));

    [Theory]
    [InlineData("https://kunawa.cn")]
    [InlineData("https://www.kunawa.cn")]
    [InlineData("https://api.kunawa.cn")]
    public void Accept_AllowsOfficialDomainAndSubdomains(string url)
        => Assert.Equal(url, ClientConfig.Accept(url));

    [Theory]
    [InlineData("http://127.0.0.1:3000")]           // 本地 mock：必须仍然可用（联调/单测依赖它）
    [InlineData("http://localhost:3000")]
    [InlineData("https://10.0.0.5")]
    [InlineData("http://192.168.1.7")]
    public void Accept_AllowsLoopbackAndPrivateLan(string url)
        => Assert.Equal(url, ClientConfig.Accept(url));

    [Theory]
    [InlineData("http://evil.example.com")]         // 远程明文 → 拒绝
    [InlineData("http://kunawa.cn")]                // 既是官方域也不许走明文
    public void Accept_RejectsPlaintextHttpForRemoteHost(string url)
        => Assert.Null(ClientConfig.Accept(url));

    /// <summary>内网后缀按设计放行（自建内网服务 / 联调要用），即便走 http。</summary>
    [Theory]
    [InlineData("http://plain-http-no-tls.local")]
    [InlineData("http://box.lan:8080")]
    public void Accept_AllowsPlaintextHttpForLanSuffix(string url)
        => Assert.Equal(url, ClientConfig.Accept(url));

    /// <summary>被拒时必须留痕：否则 UI 显示"保存成功"、实际静默回落默认地址，用户无从排查。</summary>
    [Fact]
    public void Accept_RecordsRejectedUrl()
    {
        _ = ClientConfig.Accept("https://evil.example.com");
        Assert.Equal("https://evil.example.com", ClientConfig.LastRejectedServerUrl);
    }

    /// <summary>构建产物 / 依赖目录不算「活跃源码面」（它们只是镜像了源码里的内容）。</summary>
    private static bool IsBuildOutput(string path)
    {
        var p = path.Replace('\\', '/');
        return p.Contains("/obj/") || p.Contains("/bin/") || p.Contains("/node_modules/")
            || p.Contains("/.next/") || p.Contains("/out/") || p.Contains("/publish");
    }

    private static bool IsSourceLike(string path) =>
        Path.GetExtension(path).ToLowerInvariant() is
            ".cs" or ".ts" or ".tsx" or ".js" or ".jsx" or ".mjs" or ".json" or ".py"
            or ".csproj" or ".html" or ".css";

    /// <summary>
    /// 只取「代码行」，跳过整行注释。
    /// ⚠️ 必须跳过 —— 这类红线域的**说明文字**本身就会写出那个域名
    /// （<c>ClientConfig.cs</c> 的文档注释里就写着「不能用 kunawa.xyz」），
    /// 不跳过会把讲道理的注释判成违规，护栏立刻变成噪声。
    /// </summary>
    private static IEnumerable<(string Line, int Number)> ReadCodeLines(string path)
    {
        var lines = File.ReadAllLines(path);
        for (var i = 0; i < lines.Length; i++)
        {
            var t = lines[i].TrimStart();
            if (t.StartsWith("//", StringComparison.Ordinal) ||
                t.StartsWith("/*", StringComparison.Ordinal) ||
                t.StartsWith("*", StringComparison.Ordinal) ||
                t.StartsWith("#", StringComparison.Ordinal) ||
                // .csproj / .xaml / .html / .css 的注释形式；少了这一条，
                // 项目文件里解释"为什么不敢用这个域"的注释会被判成违规。
                t.StartsWith("<!--", StringComparison.Ordinal))
                continue;
            yield return (lines[i], i + 1);
        }
    }

    private static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "KunFlashTool.slnx")))
                return dir.FullName;
            dir = dir.Parent;
        }
        throw new FileNotFoundException("找不到仓库根（KunFlashTool.slnx）");
    }
}
