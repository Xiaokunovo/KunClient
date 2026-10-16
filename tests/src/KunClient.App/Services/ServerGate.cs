using System;
using System.Net.Http;
using System.Net.Http.Headers;

namespace KunFlashTool.App.Services;

/// <summary>
/// 「请求根本没走到业务层」的统一识别与翻译。
///
/// 背景（2026-10-03 实测）：域名挂在 Cloudflare 后面，当某个出口 IP 的威胁评分升高时，
/// CF 会对它下发 <b>Managed Challenge</b> —— 响应是
/// <c>403</c> + <c>Cf-Mitigated: challenge</c> + 一页 HTML（标题「请稍候…」/「Just a moment...」）。
/// 浏览器会执行 JS 挑战后自动放行，而我们的 WPF 客户端**没有 JS 引擎，永远过不去**。
///
/// 以前的形态：登录代码直接 <c>JsonSerializer.Deserialize</c> 那页 HTML →
/// 抛 <c>JsonException</c> → 被 <c>catch (Exception)</c> 接住 →
/// 界面上只显示「登录失败: '&lt;' is an invalid start of a value...」。
/// 这句**完全指不出原因**：看着像客户端 bug，实际是网络侧被拦，排查要绕一大圈。
///
/// 所以：凡是"服务端返回的不是我们约定的 JSON"，都先过这里翻译成人话，
/// 并且**不要把挑战页原文当业务响应去解析**。
/// </summary>
public sealed class ServerGateException : Exception
{
    public ServerGateException(string message) : base(message) { }
}

public static class ServerGate
{
    /// <summary>客户端 UA。注意：改这个**不能**解掉 CF 的挑战（挑战是按 IP 威胁评分下的，
    /// 实测换浏览器 UA 一样 403），它的作用是消除"空 UA"这个额外的机器人信号。
    /// 🔴 版本号**派生**自 <see cref="UpdateService.CurrentVersion"/>（同为 const，编译期拼接）——
    /// 硬编码会成为第 6 个版本落点，改版本时必漏。</summary>
    public const string UserAgent =
        $"KunClient/{UpdateService.CurrentVersion} (Windows NT 10.0; Win64; x64) KunFlashTool";

    /// <summary>给 HttpClient 装上统一请求头。UA 为空在网关看来是强机器人信号，必须给一个。</summary>
    public static void ApplyDefaultHeaders(HttpClient client)
    {
        // v2 签名握手走 Core 层自己的 HttpClient，UA 从这里注入（Core 不能反向引用 App）。
        Core.Net.RequestSigner.HandshakeUserAgent = UserAgent;
        client.DefaultRequestHeaders.UserAgent.Clear();
        client.DefaultRequestHeaders.UserAgent.ParseAdd(UserAgent);
        client.DefaultRequestHeaders.Accept.Clear();
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        client.DefaultRequestHeaders.AcceptLanguage.ParseAdd("zh-CN,zh;q=0.9,en;q=0.8");
    }

    /// <summary>
    /// 统一创建「装了标准请求头」的 HttpClient —— **所有指向本服务端的业务请求都该走这里**。
    ///
    /// 2026-10 起 UserAgent 里的 <c>KunClient/&lt;版本&gt;</c> 不再只是反机器人信号：
    /// 服务端的**版本硬闸门**就靠它识别客户端版本（低于底线一律 426 拒绝）。
    /// 所以「某个请求忘了带 UA」不再是小事 —— 那一路上去的旧版本客户端会**漏过闸门**，
    /// 属于静默失效。新增请求点时请一律用本方法，不要自己 new HttpClient。
    /// </summary>
    public static HttpClient CreateClient(TimeSpan timeout)
    {
        var c = new HttpClient { Timeout = timeout };
        ApplyDefaultHeaders(c);
        return c;
    }

    /// <summary>是不是"网关/边缘层拦截"（CF 挑战、5xx 网关错误、跳登录页的 HTML）。</summary>
    public static bool IsEdgeBlock(HttpResponseMessage? resp, string? body)
    {
        if (resp == null) return false;
        if (resp.Headers.TryGetValues("Cf-Mitigated", out _)) return true;

        // 2026-10-07 修：原来还有一条 `CF-RAY && !IsSuccessStatusCode`。
        // 站点挂在 CF 后面，任何 4xx/5xx 都带 CF-RAY → 服务端自己的
        // 401(token 失效)/400(参数错)/429(限流) 全被翻译成「服务器暂时不可用」，
        // 真正原因用户永远看不到。
        // 保留 5xx 分支是必须的 —— CF 自己的 502/524 错误页不是 403/429/503。
        var code = (int)resp.StatusCode;
        if (LooksLikeHtml(body) && (code >= 500 || code is 403 or 429)) return true;
        return false;
    }

    /// <summary>
    /// 服务端**版本硬闸门**的识别（HTTP <c>426</c> + <c>code=VERSION_TOO_OLD</c>）。
    ///
    /// 2026-10 起服务端可以把 <c>min_client_version</c> 置起来，低于底线的客户端所有业务请求
    /// 一律被拒 —— 这**不是**网络故障、也不是账号问题，必须给出「去升级」这一条明确出路，
    /// 否则用户只会看到「登录失败 / 无法连接服务器」而反复重试。
    /// </summary>
    public static bool IsVersionTooOld(HttpResponseMessage? resp, string? body)
    {
        if (resp == null || (int)resp.StatusCode != 426) return false;
        // 只在确实是我们约定的 JSON 码时才算 —— 免得把 CF 或反向代理偶然的 426 误判成升级提示。
        return (body ?? "").Contains("VERSION_TOO_OLD", StringComparison.Ordinal);
    }

    /// <summary>
    /// 是拦截就抛一个**可以直接展示给用户**的异常；否则原样返回，交给业务解析。
    /// </summary>
    public static void ThrowIfBlocked(HttpResponseMessage? resp, string? body)
    {
        // 版本闸门要排在「边缘拦截」判定**之前**：426 是服务端业务层的明确结论，
        // 不是 CF/网关拦截，两者的处置动作完全相反（一个去升级，一个去换网络）。
        if (IsVersionTooOld(resp, body))
            throw new ServerGateException(
                "当前客户端版本过低，服务端已停止对旧版本提供服务。\n\n"
                + "请到官网下载最新版本后重试。\n"
                + "（若刚更新过，请确认后台没有残留的旧进程。）");

        if (!IsEdgeBlock(resp, body)) return;

        var code = resp == null ? 0 : (int)resp.StatusCode;
        var cfChallenge = resp?.Headers.TryGetValues("Cf-Mitigated", out var v) == true;
        var bodyHint = (body ?? "").Contains("Just a moment", StringComparison.OrdinalIgnoreCase)
                    || (body ?? "").Contains("请稍候", StringComparison.Ordinal);
        var challenge = cfChallenge || bodyHint;

        var msg = challenge
            ? $"当前网络被 Cloudflare 安全挑战拦截（HTTP {code}）。\n" +
              "浏览器能自动通过，客户端过不了。请：\n" +
              "① 换网络 / 重连路由器换一个出口 IP（家庭宽带重播 PPPoE 通常立刻生效）；\n" +
              "② 或让管理员在 Cloudflare 把 /api/ 路径从挑战里排除。"
            : $"服务器暂时不可用（HTTP {code}），请稍后重试。";
        throw new ServerGateException(msg);
    }

    private static bool LooksLikeHtml(string? body)
    {
        if (string.IsNullOrWhiteSpace(body)) return false;
        var t = body.TrimStart();
        return t.StartsWith("<!DOCTYPE", StringComparison.OrdinalIgnoreCase)
            || t.StartsWith("<html", StringComparison.OrdinalIgnoreCase)
            || t.StartsWith("<?xml", StringComparison.OrdinalIgnoreCase);
    }
}
