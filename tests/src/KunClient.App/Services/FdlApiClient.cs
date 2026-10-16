using System;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Services;

/// <summary>
/// 平台 FDL 计算接口客户端（对接文档 v1.0，2026-10-03）。
/// <para>
/// 两条链路：<c>tool_login</c> 用账号密码换 <c>T.</c> 开头的受限令牌（24 小时有效），
/// 再用 <c>tool_calc</c> 带 <c>Authorization: Bearer</c> 提交 blob 换回 FDL 签名（SIG）。
/// </para>
/// <para>
/// 🔴 上游 API 地址与密钥**只在平台服务端**，本客户端拿不到也不需要 ——
/// 所以 SIG 必须由这里算，工具端不内置任何签名算法。
/// </para>
/// </summary>
public sealed class FdlApiClient
{
    /// <summary>默认接口地址。<b>开源版已抹成占位符</b>，部署时替换成你自己的 FDL 计算服务地址；
    /// 也可用环境变量 <c>KUNCLIENT_FDL_URL</c> 覆盖（联调/换平台时用）。
    /// 🔴 不能写成 <c>const string</c>：const 的值落在元数据 **Constant 表**，而 Obfuscar 的
    ///    HideStrings 只处理方法体里的 ldstr、碰不到 Constant 表 —— 那个地址会以**明文**
    ///    留在产物里（实测复现）。改 static readonly 后它变成普通 ldstr，会被字符串加密覆盖。</summary>
    public static readonly string DefaultUrl =
        "https://fdl-endpoint.example.com/api.php";

    public string Endpoint { get; }

    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(40) };

    public FdlApiClient(string? endpoint = null)
    {
        var env = Environment.GetEnvironmentVariable("KUNCLIENT_FDL_URL");
        Endpoint = string.IsNullOrWhiteSpace(endpoint) ? (env ?? DefaultUrl) : endpoint!;
    }

    // ───────────────────────── 结果类型 ─────────────────────────

    public sealed class LoginResult
    {
        public bool Ok { get; init; }
        public string Token { get; init; } = "";
        public string Username { get; init; } = "";
        public double Balance { get; init; }
        /// <summary>令牌到期时间（UTC）。只用来**存会话**（不再存密码），不做本地授权判断。</summary>
        public DateTime TokenExpiryUtc { get; init; } = DateTime.MinValue;
        /// <summary>可直接展示给用户的原因（已按状态码翻译）。</summary>
        public string Error { get; init; } = "";
        public int StatusCode { get; init; }
    }

    public sealed class CalcResult
    {
        public bool Ok { get; init; }
        public string Sig { get; init; } = "";
        public double Cost { get; init; }
        public double Balance { get; init; }
        public double Elapsed { get; init; }
        public string Error { get; init; } = "";
        public int StatusCode { get; init; }
    }

    // ───────────────────────── 令牌缓存 ─────────────────────────

    private string? _token;
    private string _tokenUser = "";
    private DateTime _tokenExpiryUtc = DateTime.MinValue;

    /// <summary>令牌还有多久到期（小时）。没有令牌时返回 0。</summary>
    public double TokenHoursLeft
        => _token == null ? 0 : Math.Max(0, (_tokenExpiryUtc - DateTime.UtcNow).TotalHours);

    /// <summary>是否持有一枚还没过期的令牌。</summary>
    public bool HasUsableToken => _token != null && _tokenExpiryUtc > DateTime.UtcNow;

    /// <summary>令牌到期时间（UTC），供调用方持久化会话。</summary>
    public DateTime TokenExpiryUtc => _tokenExpiryUtc;

    /// <summary>当前令牌（没有则空串）。供调用方在**自动重登之后**把最新令牌存下来。</summary>
    public string Token => _token ?? "";

    /// <summary>
    /// 用**已保存的令牌**恢复会话（2026-10-09：密码不再落盘，重启后靠令牌续上）。
    /// <para>
    /// 只接受没过期的令牌：过期的塞进来会让后续 tool_calc 白跑一次 401，
    /// 且在没密码可自动重登的情况下只能报错，不如一开始就当作"没有会话"、提示用户重填密码。
    /// </para>
    /// </summary>
    public bool RestoreSession(string username, string token, DateTime expiryUtc)
    {
        if (string.IsNullOrWhiteSpace(username) || string.IsNullOrWhiteSpace(token)) return false;
        if (expiryUtc <= DateTime.UtcNow) return false;
        _token = token.Trim();
        _tokenUser = username.Trim();
        _tokenExpiryUtc = expiryUtc;
        return true;
    }

    // ───────────────────────── tool_login ─────────────────────────

    public async Task<LoginResult> LoginAsync(string username, string password, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(username) || string.IsNullOrWhiteSpace(password))
            return new LoginResult { Error = "请填写平台账号与密码" };

        var body = JsonBody.Of(("username", username.Trim()), ("password", password));
        var (ok, json, status) = await PostAsync("tool_login", body, null, ct).ConfigureAwait(false);

        if (!ok)
            return new LoginResult { Error = Explain(status, json), StatusCode = status };

        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (!r.TryGetProperty("success", out var s) || !s.GetBoolean())
                return new LoginResult { Error = Pick(r, "error", "登录失败"), StatusCode = status };

            var token = r.TryGetProperty("token", out var tk) ? tk.GetString() ?? "" : "";
            if (string.IsNullOrEmpty(token))
                return new LoginResult { Error = "平台没有返回令牌", StatusCode = status };

            // 文档：令牌有效期 24 小时。这里留 5 分钟余量，避免边界上拿着过期令牌去算。
            _token = token;
            _tokenUser = username.Trim();
            _tokenExpiryUtc = DateTime.UtcNow.AddHours(24).AddMinutes(-5);

            var bal = r.TryGetProperty("balance", out var b) && b.ValueKind == JsonValueKind.Number ? b.GetDouble() : 0;
            var usr = r.TryGetProperty("username", out var un) ? un.GetString() ?? _tokenUser : _tokenUser;
            return new LoginResult
            {
                Ok = true,
                Token = token,
                Username = usr,
                Balance = bal,
                TokenExpiryUtc = _tokenExpiryUtc,
            };
        }
        catch (Exception ex)
        {
            return new LoginResult { Error = "登录响应解析失败：" + ex.Message, StatusCode = status };
        }
    }

    // ───────────────────────── tool_calc ─────────────────────────

    /// <summary>
    /// 提交 blob 换 FDL 签名。
    /// <para>
    /// 🔴 401（令牌无效/过期）会自动**重新登录一次**再重试 —— 文档明确要求"工具端建议在收到 401 时
    /// 自动重新登录"，因为令牌还会在用户登录网页端时被**单点作废**。
    /// 重试只做一次，避免账号被停用时无限循环。
    /// </para>
    /// </summary>
    public async Task<CalcResult> CalcAsync(string blob, string? username, string? password, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(blob))
            return new CalcResult { Error = "blob 是空的，先从设备读取 token" };
        blob = blob.Trim();
        if (blob.Length > 4096)
            return new CalcResult { Error = $"blob 长度 {blob.Length} 超过接口上限 4096" };

        if (_token == null || _tokenExpiryUtc <= DateTime.UtcNow)
        {
            if (string.IsNullOrWhiteSpace(username) || string.IsNullOrWhiteSpace(password))
                return new CalcResult { Error = "还没有平台令牌，请先填写平台账号并点「登录」" };
            var lr = await LoginAsync(username!, password!, ct).ConfigureAwait(false);
            if (!lr.Ok) return new CalcResult { Error = lr.Error, StatusCode = lr.StatusCode };
        }

        var body = JsonBody.Of(("blob", blob), ("mode", "fdl"));
        var (ok, json, status) = await PostAsync("tool_calc", body, _token, ct).ConfigureAwait(false);

        // 401：令牌被作废/过期 → 重新登录后**只重试一次**
        if (status == 401 && !string.IsNullOrWhiteSpace(username) && !string.IsNullOrWhiteSpace(password))
        {
            _token = null;
            _tokenExpiryUtc = DateTime.MinValue;
            var lr = await LoginAsync(username!, password!, ct).ConfigureAwait(false);
            if (!lr.Ok) return new CalcResult { Error = "令牌已失效，重新登录失败：" + lr.Error, StatusCode = 401 };
            (ok, json, status) = await PostAsync("tool_calc", body, _token, ct).ConfigureAwait(false);
        }

        if (!ok)
            return new CalcResult { Error = Explain(status, json), StatusCode = status };

        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (!r.TryGetProperty("success", out var s) || !s.GetBoolean())
                return new CalcResult { Error = Pick(r, "error", "计算失败"), StatusCode = status };

            var sig = r.TryGetProperty("sig", out var sg) ? sg.GetString() ?? "" : "";
            if (string.IsNullOrEmpty(sig))
                return new CalcResult { Error = "平台没有返回签名值", StatusCode = status };

            return new CalcResult
            {
                Ok = true,
                Sig = sig,
                Cost = Num(r, "cost"),
                Balance = Num(r, "balance"),
                Elapsed = Num(r, "elapsed"),
                StatusCode = status,
            };
        }
        catch (Exception ex)
        {
            return new CalcResult { Error = "计算响应解析失败：" + ex.Message, StatusCode = status };
        }
    }

    // ───────────────────────── 传输 ─────────────────────────

    private async Task<(bool Ok, string Json, int Status)> PostAsync(
        string action, string body, string? token, CancellationToken ct)
    {
        try
        {
            using var req = new HttpRequestMessage(HttpMethod.Post, Endpoint + "?action=" + action)
            {
                Content = new StringContent(body, Encoding.UTF8),
            };
            req.Content.Headers.ContentType = new MediaTypeHeaderValue("application/json")
            {
                CharSet = "utf-8",
            };
            // 文档：工具端是无人值守场景，UA 写明自己是谁，方便平台侧排查限流/异常
            req.Headers.UserAgent.ParseAdd(ServerGate.UserAgent);
            req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
            if (!string.IsNullOrEmpty(token)) req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);

            using var resp = await Http.SendAsync(req, ct).ConfigureAwait(false);
            var text = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
            var code = (int)resp.StatusCode;
            if (!resp.IsSuccessStatusCode)
                return (false, text, code == 0 ? 599 : code);
            return (true, text, code);
        }
        catch (TaskCanceledException) when (!ct.IsCancellationRequested)
        {
            return (false, "{\"error\":\"请求超时\"}", 408);
        }
        catch (HttpRequestException ex)
        {
            return (false, "{\"error\":\"" + Escape(ex.Message) + "\"}", 0);
        }
    }

    // ───────────────────────── 错误翻译（按文档第 04 节的状态码表） ─────────────────────────

    /// <summary>把状态码 + <c>{"error": "..."}</c> 翻成一句能直接展示的话。</summary>
    private static string Explain(int status, string json)
    {
        var detail = TryPickError(json);
        var suffix = string.IsNullOrEmpty(detail) ? "" : $"（{detail}）";
        return status switch
        {
            400 => "参数错误或 blob 格式不对" + suffix,
            401 => "令牌无效或已过期" + suffix,
            402 => "点数不足，请联系平台方充值" + suffix,
            403 => "账号已被停用，请联系平台方" + suffix,
            404 => "接口不存在：确认 api.php 已部署，且服务器没有拦截 POST" + suffix,
            408 => "计算超时，可重试" + suffix,
            429 => "调用过于频繁，请降低频率后重试" + suffix,
            500 => "平台服务端异常" + suffix,
            0 => "连不上平台接口" + suffix,
            _ => $"平台返回 HTTP {status}" + suffix,
        };
    }

    private static string TryPickError(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            return Pick(doc.RootElement, "error", "");
        }
        catch { return ""; }
    }

    private static string Pick(JsonElement el, string name, string fallback)
        => el.ValueKind == JsonValueKind.Object && el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() ?? fallback : fallback;

    private static double Num(JsonElement el, string name)
        => el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;

    private static string Escape(string s)
        => s.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\n", " ").Replace("\r", " ");
}
