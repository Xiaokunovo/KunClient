using System;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace KunFlashTool.Core.Net;

/// <summary>
/// API v2 请求签名器（2026-10-11 服务端全量重构配套）。
///
/// 模型：先 POST <c>/api/v2/client/handshake</c> 换一次性会话 <c>{sid, secret}</c>
/// （secret 只经 HTTPS 下发一次、只存内存 —— 不落盘、不进程序集，符合凭据红线），
/// 之后每个受保护请求带四个头：
/// <code>
///   X-Sig-Sid    会话 id
///   X-Sig-Ts     毫秒时间戳（服务端允许 ±120s）
///   X-Sig-Nonce  每请求唯一随机串（服务端查重，重放直接 401）
///   X-Sig        base64(HMAC_SHA256(secret, ts \n nonce \n METHOD \n path \n sha256hex(body)))
/// </code>
/// 服务端 401 + code=SIG_SESSION_EXPIRED 时自动重新握手并重放一次（会话 24h 过期是常态）。
///
/// ⚠️ 只对**受保护端点**生效的机制由服务端裁决；客户端对受保护端点统一走本类
///（登录四入口、额度闸门、环境上报），漏接一路就会被 401 SIG_REQUIRED 拦下。
/// </summary>
public static class RequestSigner
{
    /// <summary>握手请求用的 UA。App 启动时由 ServerGate.ApplyDefaultHeaders 注入真实值
    ///（Core 不能反向引用 App，所以用属性而不是 const —— 也顺带避开版本号第 6 落点问题）。</summary>
    public static string HandshakeUserAgent { get; set; } = "KunClient";

    private static readonly SemaphoreSlim Gate = new(1, 1);
    private static string? _sid;
    private static string? _secret;
    private static DateTime _expireUtc = DateTime.MinValue;
    private static string? _sessionBaseUrl;

    /// <summary>
    /// 握手 HttpClient 工厂（测试注入点）。生产默认 null = 用内置实例（跟随系统代理，
    /// 与业务请求同策略）。沙箱/探针环境里系统代理会拦 loopback，须注入 UseProxy=false 的实例。
    /// </summary>
    public static Func<HttpClient>? HandshakeClientFactory { get; set; }

    private static HttpClient? _handshakeClient;
    private static HttpClient HandshakeClient => _handshakeClient ??= HandshakeClientFactory?.Invoke()
        ?? new HttpClient { Timeout = TimeSpan.FromSeconds(15) };

    /// <summary>退出登录 / 换服务端地址时清掉签名会话（下次请求自动重新握手）。</summary>
    public static void Reset()
    {
        _sid = null;
        _secret = null;
        _expireUtc = DateTime.MinValue;
        _sessionBaseUrl = null;
    }

    /// <summary>签名 POST JSON 的便捷入口。configure 用来补 Authorization 等额外头。</summary>
    public static Task<HttpResponseMessage> PostJsonAsync(
        HttpClient client, string url, string json, Action<HttpRequestMessage>? configure = null)
        => SendAsync(client, HttpMethod.Post, url, json, configure);

    public static async Task<HttpResponseMessage> SendAsync(
        HttpClient client, HttpMethod method, string url, string? json, Action<HttpRequestMessage>? configure = null)
    {
        for (var attempt = 0; ; attempt++)
        {
            await EnsureSessionAsyncAsync(BaseOf(url)).ConfigureAwait(false);
            using var req = new HttpRequestMessage(method, url);
            ApplySignature(req, method.Method, new Uri(url).AbsolutePath, json ?? "");
            if (json != null)
                req.Content = new StringContent(json, Encoding.UTF8, "application/json");
            configure?.Invoke(req);
            var resp = await client.SendAsync(req).ConfigureAwait(false);
            // 会话过期（24h 到点 / 服务端重启）是常态：重新握手后重放一次。
            // 只重放一次，防止服务端配置漂移时在这里打转。
            if (attempt == 0 && (int)resp.StatusCode == 401 && IsSessionExpired(resp))
            {
                Reset();
                continue;
            }
            return resp;
        }
    }

    private static bool IsSessionExpired(HttpResponseMessage resp)
    {
        try
        {
            var body = resp.Content.ReadAsStringAsync().ConfigureAwait(false).GetAwaiter().GetResult();
            return body.Contains("SIG_SESSION_EXPIRED", StringComparison.Ordinal)
                || body.Contains("SIG_REQUIRED", StringComparison.Ordinal);
        }
        catch { return false; }
    }

    private static string BaseOf(string url) => new Uri(url).GetLeftPart(UriPartial.Authority);

    private static void ApplySignature(HttpRequestMessage req, string method, string path, string body)
    {
        if (string.IsNullOrEmpty(_sid) || string.IsNullOrEmpty(_secret)) return;
        var ts = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds().ToString();
        var nonce = RandomNumberGenerator.GetBytes(12);
        var nonceHex = Convert.ToHexString(nonce).ToLowerInvariant();
        var bodyHash = Sha256Hex(body);
        var toSign = ts + "\n" + nonceHex + "\n" + method + "\n" + path + "\n" + bodyHash;
        var sig = Convert.ToBase64String(new HMACSHA256(Encoding.UTF8.GetBytes(_secret)).ComputeHash(Encoding.UTF8.GetBytes(toSign)));
        req.Headers.Add("X-Sig-Sid", _sid);
        req.Headers.Add("X-Sig-Ts", ts);
        req.Headers.Add("X-Sig-Nonce", nonceHex);
        req.Headers.Add("X-Sig", sig);
    }

    private static async Task EnsureSessionAsyncAsync(string baseUrl)
    {
        if (!string.IsNullOrEmpty(_sid) && _sessionBaseUrl == baseUrl && DateTime.UtcNow < _expireUtc) return;
        await Gate.WaitAsync().ConfigureAwait(false);
        try
        {
            // 双检：等锁期间别人可能已经换好了
            if (!string.IsNullOrEmpty(_sid) && _sessionBaseUrl == baseUrl && DateTime.UtcNow < _expireUtc) return;

            using var req = new HttpRequestMessage(HttpMethod.Post, baseUrl + "/api/v2/client/handshake")
            {
                Content = new StringContent("{}", Encoding.UTF8, "application/json"),
            };
            req.Headers.UserAgent.ParseAdd(HandshakeUserAgent);
            using var resp = await HandshakeClient.SendAsync(req).ConfigureAwait(false);
            var body = await resp.Content.ReadAsStringAsync().ConfigureAwait(false);
            if (!resp.IsSuccessStatusCode)
            {
                var snippet = (body ?? "");
                if (snippet.Length > 120) snippet = snippet[..120];
                throw new RequestSignerException($"签名握手失败（HTTP {(int)resp.StatusCode}）：{snippet}");
            }
            using var doc = JsonDocument.Parse(body);
            var data = doc.RootElement.GetProperty("data");
            var sid = data.GetProperty("sid").GetString();
            var secret = data.GetProperty("secret").GetString();
            var expiresIn = data.TryGetProperty("expiresIn", out var expEl) ? expEl.GetInt64() : 86400;
            if (string.IsNullOrEmpty(sid) || string.IsNullOrEmpty(secret))
                throw new RequestSignerException("签名握手返回数据不完整");
            _sid = sid;
            _secret = secret;
            _sessionBaseUrl = baseUrl;
            // 提前 5 分钟过期，避开临界点
            _expireUtc = DateTime.UtcNow.AddSeconds(expiresIn - 300);
        }
        catch (HttpRequestException e)
        {
            throw new RequestSignerException("无法连接服务器（签名握手）：" + e.Message);
        }
        finally
        {
            Gate.Release();
        }
    }

    private static string Sha256Hex(string s)
    {
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(s));
        return Convert.ToHexString(hash).ToLowerInvariant();
    }
}

/// <summary>签名链路自身的失败（握手不通 / 返回异常）。message 可直接展示。</summary>
public sealed class RequestSignerException : Exception
{
    public RequestSignerException(string message) : base(message) { }
}
