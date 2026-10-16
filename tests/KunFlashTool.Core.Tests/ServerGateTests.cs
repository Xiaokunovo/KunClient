using System.Net;
using System.Net.Http;
using KunFlashTool.App.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 用例②：401 JSON 不被 <see cref="ServerGate.IsEdgeBlock"/> 吞（组 B）。
///
/// 病因：原判据有一条 `CF-RAY && !IsSuccessStatusCode` —— 站点挂 Cloudflare 后
/// **任何** 4xx/5xx 都带 CF-RAY，于是服务端自己的 401/400/429 全被翻译成
/// 「服务器暂时不可用」，真因（token 失效 / 限流 / 参数错）用户永远看不到。
/// </summary>
public class ServerGateTests
{
    private static HttpResponseMessage Resp(HttpStatusCode code, string? cfRay = "8a1b2c3d4e5f6789-SIN")
    {
        var resp = new HttpResponseMessage(code);
        if (cfRay != null) resp.Headers.TryAddWithoutValidation("CF-RAY", cfRay);
        return resp;
    }

    [Fact]
    public void IsEdgeBlock_401Json_WithCfRay_IsNotAnEdgeBlock()
    {
        using var resp = Resp(HttpStatusCode.Unauthorized);
        // 真实服务端 401（E8 起带 code）——必须原样交给业务层解析
        var body = "{\"success\":false,\"code\":\"TOKEN_INVALID\",\"message\":\"Token无效\"}";
        Assert.False(ServerGate.IsEdgeBlock(resp, body));
    }

    [Fact]
    public void IsEdgeBlock_400Json_WithCfRay_IsNotAnEdgeBlock()
    {
        using var resp = Resp(HttpStatusCode.BadRequest);
        Assert.False(ServerGate.IsEdgeBlock(resp, "{\"success\":false,\"message\":\"请输入用户名和密码\"}"));
    }

    [Fact]
    public void IsEdgeBlock_429Json_WithCfRay_IsNotAnEdgeBlock()
    {
        // 限流必须能被看见，客户端才能展示「稍后再试」（配套 E7 的 Retry-After 文案）
        using var resp = Resp(HttpStatusCode.TooManyRequests);
        Assert.False(ServerGate.IsEdgeBlock(resp,
            "{\"success\":false,\"code\":\"RATE_LIMITED\",\"message\":\"登录尝试过于频繁\"}"));
    }

    [Fact]
    public void IsEdgeBlock_403Html_IsDetected()
    {
        // CF 的 Managed Challenge：403 + HTML 挑战页 —— 仍然要拦（客户端没有 JS 引擎）
        using var resp = Resp(HttpStatusCode.Forbidden);
        Assert.True(ServerGate.IsEdgeBlock(resp, "<!DOCTYPE html><html><title>Just a moment...</title></html>"));
    }

    [Fact]
    public void IsEdgeBlock_502Html_IsDetected()
    {
        // CF 自己的 5xx 错误页：不是 403/429，靠 `code >= 500` 兜住
        using var resp = Resp(HttpStatusCode.BadGateway);
        Assert.True(ServerGate.IsEdgeBlock(resp, "<html><body>502 Bad Gateway</body></html>"));
    }

    [Fact]
    public void IsEdgeBlock_CfMitigatedChallenge_IsDetected_WithoutHtmlBody()
    {
        using var resp = Resp(HttpStatusCode.Forbidden, cfRay: null);
        resp.Headers.TryAddWithoutValidation("Cf-Mitigated", "challenge");
        // 有 Cf-Mitigated 就是 CF 拦截，与 body 长什么样无关
        Assert.True(ServerGate.IsEdgeBlock(resp, "{\"unexpected\":\"shape\"}"));
    }

    [Fact]
    public void ThrowIfBlocked_401Json_DoesNotThrow()
    {
        using var resp = Resp(HttpStatusCode.Unauthorized);
        // 不抛 = 走到业务层解析，用户能看到服务端给的真实原因
        ServerGate.ThrowIfBlocked(resp, "{\"success\":false,\"message\":\"Token无效\"}");
    }
}