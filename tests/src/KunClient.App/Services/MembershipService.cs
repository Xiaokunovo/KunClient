using System;
using System.Collections.Generic;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Input;

namespace KunFlashTool.App.Services;

/// <summary>
/// 会员等级 + 各等级额度（每日次数 / 功能是否开放）。
/// <para>
/// 等级：普通 <c>normal</c> / <c>vip</c> / <c>svip</c>。登录后服务端下发 vipExpireAt 与 vipLevel，
/// 未过期即按下发等级；控制台把永久会员表示成远期到期日，所以判定写成「到期时间 &gt; 现在」。
/// </para>
/// <para>
/// <b>额度裁决权在服务端（2026-09-25 反破解改造）。</b>每次动作前都会向
/// <c>POST /api/app/check</c>（只校验）或 <c>POST /api/app/consume</c>（校验并扣费）要一个结论，
/// 计数落在服务端的 <c>feature_usage</c> 表里。
/// </para>
/// <para>
/// 为什么改成这样：改造前「今天用了多少次」只写在本机 <c>%LOCALAPPDATA%\KunClient\quota.json</c>，
/// 而 <c>limits.json</c> 是明文服务端响应 —— 用户**删一个文件**或**用记事本把 open 改成 ["*"]**
/// 就能无限使用，连反编译都不需要。现在这两个文件只是展示缓存，改了也没用。
/// </para>
/// <para>
/// <b>威胁模型说明（别误以为这样就能"绝对不可破解"）：</b>EDL 刷写这类动作执行在用户自己的
/// 机器与设备上，不消耗任何服务端资源，所以**一个被完全逆向改造过的客户端仍然可以放行**。
/// 这一层能彻底挡住的是「改文件 / 删文件」这种零技术手段，并把额度结论从"客户端说了算"
/// 变成"服务端说了算"；对付二进制层面的篡改，靠的是编译期的混淆与加壳。
/// </para>
/// </summary>
public static class MembershipService
{
    // 这三个路径故意写成 static readonly 而**不是 const**：
    // const 的值会进元数据的 Constant 表，混淆器的「字符串加密」只处理方法体里的 ldstr，
    // 碰不到 Constant 表 → 路径会以明文留在产物里（实测：改成 const 时 /api/app/consume
    // 仍能在 exe 里被 grep 到）。static readonly 会走 ldstr，能被一起加密。
    private static readonly string LimitsPath = "/api/v2/app/limits";
    private static readonly string GateCheckPath = "/api/v2/app/check";
    private static readonly string GateConsumePath = "/api/v2/app/consume";

    private static readonly string StoreDir = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KunClient");
    private static readonly string QuotaPath = Path.Combine(StoreDir, "quota.json");
    private static readonly string LimitsCachePath = Path.Combine(StoreDir, "limits.json");

    private static readonly HttpClient Http = ServerGate.CreateClient(TimeSpan.FromSeconds(12));
    /// <summary>额度裁决专用：超时必须短，因为它是在 UI 线程上同步等待的。</summary>
    private static readonly HttpClient GateHttp = ServerGate.CreateClient(TimeSpan.FromSeconds(8));

    private static string? _vipExpire;
    /// <summary>服务端下发的等级 key（未做过期回落；取生效等级请用 <see cref="Level"/>）。</summary>
    private static string _vipLevel = "normal";

    /// <summary>
    /// 服务端**这次登录**是否真的下发过等级字段。
    /// <para>
    /// 🔴 没有这个标志就只能"猜"：原来 <see cref="Level"/> 把任何非 vip/svip 的等级
    /// —— 包括服务端**显式**下发的 <c>normal</c>、以及任何认不出来的字符串 ——
    /// 一律升成 vip（只要到期时间在未来）。逆向实测：连 <c>"normal"</c> 都变 <c>vip</c>。
    /// 兼容老账号（响应里只有 vip_expire_at、没有 vipLevel）又要求"没下发时按 vip 兜"，
    /// 所以必须把「没下发」和「下发了但我看不懂」分开——后者一律**回落 normal**（fail-closed）。
    /// </para>
    /// </summary>
    private static bool _vipLevelFromServer;

    /// <summary>本地用量镜像，**只用于展示**（「今日剩余 n 次」）；真值在服务端。</summary>
    private static Dictionary<string, QuotaEntry> _quota = LoadQuota();
    private static LimitsConfig _limits = LoadLimitsCache();

    private static int _limitsFetching;   // 0/1，Interlocked 互斥
    private static long _lastFetchTicks;

    /// <summary>
    /// 功能 key → 中文名。由 <see cref="EnsureAllowed"/> / <see cref="GuardAndConsume"/> 顺手记下，
    /// 供 <see cref="Consume"/> 在扣费失败时拼出一致的提示（Consume 只拿到 key，没有标签）。
    /// 所有闸门调用都在 UI 线程上同步发生，所以这个字典不需要加锁。
    /// </summary>
    private static readonly Dictionary<string, string> _labelByKey = new();

    public static string? UserName { get; private set; }

    /// <summary>生效等级：会员未过期时用下发等级，否则回落 normal。</summary>
    public static string Level
    {
        get
        {
            // 解析统一走 AccountText：服务端的「永久」是远期到期日，在已是永久的账号上
            // 再叠加天数会越过 9999 年，下发成 +010099-12-06 这种 ISO 扩展年份写法，
            // DateTime.TryParse 会失败——那样永久会员就会被当成非会员限次。
            if (!AccountText.IsActiveVip(_vipExpire)) return "normal";

            // 映射规则集中在 AccountText.EffectiveLevel（纯函数、有单测）；
            // fail-closed：服务端下发了等级时，认不出的一律回落 normal。
            return AccountText.EffectiveLevel(_vipLevel, _vipLevelFromServer);
        }
    }

    public static bool IsVip => Level != "normal";

    /// <summary>登录后同步会员身份与等级；退出登录时 username / vipExpire 传 null 即可。</summary>
    public static void SetUser(string? username, string? vipExpire, string? vipLevel = null)
    {
        UserName = username;
        _vipExpire = vipExpire;

        // 退出登录（username / vipExpire 都传 null）：必须把"这次登录下发过等级"这个标志一起清掉，
        // 否则下一次登录若走了老接口（不下发等级），会沿用上一次残留的标志与等级值。
        if (string.IsNullOrWhiteSpace(username))
        {
            _vipLevel = "normal";
            _vipLevelFromServer = false;
            RefreshLimits();
            return;
        }

        // 等级字段缺失（老接口 / 异常响应）时**保持原值**，绝不用 null 覆盖本地已有状态
        if (!string.IsNullOrWhiteSpace(vipLevel))
        {
            _vipLevel = vipLevel.Trim().ToLowerInvariant();
            _vipLevelFromServer = true;
        }
        RefreshLimits();
    }

    /// <summary>
    /// 拉取服务端额度配置（异步、不阻塞 UI）。成功后写本地展示缓存。
    /// 失败会被放行到下一次调用（登录、轮询刷新等级时都会触发），不会长期卡在默认值。
    /// </summary>
    public static void RefreshLimits()
    {
        // 60 秒内不重复请求，避免轮询把它变成高频调用
        if (Interlocked.CompareExchange(ref _limitsFetching, 1, 0) != 0) return;
        if (DateTime.UtcNow.Ticks - Interlocked.Read(ref _lastFetchTicks) < TimeSpan.TicksPerMinute)
        {
            Interlocked.Exchange(ref _limitsFetching, 0);
            return;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                using var resp = await Http.GetAsync($"{AppConfig.ServerUrl}{LimitsPath}");
                if (!resp.IsSuccessStatusCode) return;
                var json = await resp.Content.ReadAsStringAsync();
                var parsed = LimitsConfig.Parse(json);
                if (parsed == null) return;
                _limits = parsed;
                SaveLimitsCache(json);
                Interlocked.Exchange(ref _lastFetchTicks, DateTime.UtcNow.Ticks);
            }
            catch { /* 网络异常：继续用缓存 / 兜底默认值 */ }
            finally { Interlocked.Exchange(ref _limitsFetching, 0); }
        });
    }

    /// <summary>
    /// 该等级是否开放某功能；未开放时 <paramref name="requiredLevelLabel"/> 给出所需等级名。
    /// <b>这只是本地展示口径</b>，真正的放行权在服务端（见 <see cref="EnsureAllowed"/>）。
    /// </summary>
    public static bool IsFeatureOpen(string featureKey, out string requiredLevelLabel)
    {
        requiredLevelLabel = "";
        if (_limits.For(Level).Opens(featureKey)) return true;

        // 找一个开放该功能的最低等级，用于「需要 XXX」的提示文案
        foreach (var cand in new[] { "normal", "vip", "svip" })
        {
            if (_limits.For(cand).Opens(featureKey))
            {
                requiredLevelLabel = AccountText.VipLevel(cand);
                return false;
            }
        }
        return false;   // 没有任何等级开放 → 视为未开放
    }

    /// <summary>当前等级的每日次数上限；0 = 不限次。仅用于展示。</summary>
    public static int DailyLimit(string featureKey) => _limits.For(Level).Limit(featureKey);

    /// <summary>
    /// 功能准入检查：未开放则提示不可用；开放但当日次数用尽则拦下。
    /// <para>
    /// <b>结论来自服务端</b>（<c>POST /api/app/check</c>，只校验不扣费）——
    /// 调用方确认用户真的要执行后再调 <see cref="Consume"/> 扣费，这样「弹了确认框但用户取消」
    /// 不会白吃一次额度。
    /// </para>
    /// </summary>
    public static bool EnsureAllowed(string featureLabel, string featureKey)
    {
        _labelByKey[featureKey] = featureLabel;

        // ① 本地快速预判：明显未开放就别浪费一次网络往返（本地缓存被改成「更严」只会误拦自己，是安全方向）
        if (!IsFeatureOpen(featureKey, out var need))
        {
            ShowLocked(featureLabel, need);
            return false;
        }

        // ② 服务端裁决（权威）
        var gate = RequestGate(GateCheckPath, featureKey);
        if (gate.Allowed)
        {
            NoteUsage(featureKey, gate);
            return true;
        }

        ShowGateDenied(featureLabel, featureKey, gate, need);
        return false;
    }

    /// <summary>
    /// 准入 + 立即扣费（服务端一次调用里原子完成「校验 + 计数」）。
    /// 适用于「按钮点击本身就是确认、检查通过就直接开工」的场景
    /// （刷写某分区、擦除、导出、镜像等单次动作）。
    /// 如果流程里还有二次确认对话框，请仍用 <see cref="EnsureAllowed"/> 先校验、
    /// 等用户确认后再 <see cref="Consume"/>，避免用户取消也吃掉额度。
    /// </summary>
    public static bool GuardAndConsume(string featureLabel, string featureKey)
    {
        _labelByKey[featureKey] = featureLabel;

        if (!IsFeatureOpen(featureKey, out var need))
        {
            ShowLocked(featureLabel, need);
            return false;
        }

        var gate = RequestGate(GateConsumePath, featureKey);
        if (gate.Allowed)
        {
            NoteUsage(featureKey, gate);
            return true;
        }

        ShowGateDenied(featureLabel, featureKey, gate, need);
        return false;
    }

    /// <summary>
    /// 真正开始执行前调用：在服务端扣掉一次额度。<b>扣费失败返回 false，调用方必须中断动作。</b>
    /// <para>
    /// 为什么必须 fail-closed：30 处 <c>Consume</c> 全都位于「用户已确认、动作马上开始」的位置，
    /// 它们前面只有一个 <see cref="EnsureAllowed"/>（只校验、不扣费）。如果这里失败还继续放行，
    /// 那么用一个本地 HTTP 代理放行 <c>/api/app/check</c>、再按路径拦掉 <c>/api/app/consume</c>，
    /// 普通账号就能一直刷而不被记账 —— 服务端记账也就白做了。
    /// </para>
    /// <para>
    /// 返回值从 void 改成 bool 时，C# 允许调用点忽略返回值，所以这是**渐进式**改造：
    /// 没有显式检查的调用点会照旧编译通过，但要靠 <c>if (!… ) return;</c> 才真正起到拦截作用。
    /// </para>
    /// </summary>
    public static bool Consume(string featureKey)
    {
        var gate = RequestGate(GateConsumePath, featureKey);
        if (gate.Allowed)
        {
            NoteUsage(featureKey, gate);
            return true;
        }

        var label = _labelByKey.TryGetValue(featureKey, out var known) ? known : featureKey;
        SessionLogFile.Append(DateTime.Now, "WARN",
            $"[额度] 服务端记账失败，已中断动作：{featureKey}（{DescribeFailure(gate)}）");
        ShowGateDenied(label, featureKey, gate, NeedLabelFor(featureKey));
        return false;
    }

    /// <summary>该功能的「需要 XXX 等级」提示文案；本地缓存口径，仅用于拼提示。</summary>
    private static string NeedLabelFor(string featureKey)
    {
        IsFeatureOpen(featureKey, out var need);
        return need;
    }

    /// <summary>今天还剩几次（不限次返回 int.MaxValue）。展示用，真值以服务端为准。</summary>
    public static int Remaining(string featureKey)
    {
        if (!IsFeatureOpen(featureKey, out _)) return 0;
        var cap = DailyLimit(featureKey);
        if (cap <= 0) return int.MaxValue;
        return Math.Max(0, cap - UsedToday(featureKey));
    }

    // ===================== 服务端裁决 =====================

    /// <summary>一次裁决的结果。<c>Online=false</c> 表示没拿到结论（网络/鉴权/响应异常）。</summary>
    private sealed class GateResult
    {
        public bool Online;
        public bool Allowed;
        /// <summary>
        /// locked（该等级未开放）/ <b>module（增值模块未开通）</b> / exhausted（当日用尽）/
        /// pending / offline / noauth / badresp / <b>kicked（被管理员强制下线，程序正在退出）</b> /
        /// <b>banned（本机 IP 被管理员封禁）</b>。
        /// </summary>
        public string Reason = "offline";
        public string Level = "";
        public int Cap;
        public int Used;
        public int Remaining;   // -1 = 不限次

        /// <summary>
        /// 功能所属的增值模块（服务端在被拒时下发）。<b>必须与 locked 分开处理</b>：
        /// 等级不够可以靠升级会员解决，模块没开通只能单独开通 —— 混成一句提示会让用户白买一个会员。
        /// </summary>
        public string ModuleName = "";
        public string ModuleTitle = "";
        public string ModulePriceNote = "";

        /// <summary>
        /// 服务端原样下发的说明文案。目前只有 IP 封禁用它 —— 封禁原因与到期时间只有服务端知道，
        /// 客户端自己拼不出「封到哪天」这种信息。
        /// </summary>
        public string ServerMessage = "";
    }

    /// <summary>
    /// 向服务端要一次额度裁决，**同步阻塞**。
    /// <para>
    /// 之所以敢在 UI 线程上阻塞：这个判定被 53 个事件处理器调用，全是同步签名；
    /// 为了它把 53 处改成 async，回归面远大于这点延迟。实测一次往返在几十毫秒级，
    /// 配合 8 秒超时 + 等待光标，观感上与本地判断没有区别。
    /// </para>
    /// <para>
    /// <b>拿不到结论时一律不放行</b>（fail-closed）。工具登录本身就需要服务器可达，
    /// 所以「连不上」不是正常使用状态；放行才是给「拔网线绕过额度」开后门。
    /// </para>
    /// </summary>
    private static GateResult RequestGate(string path, string featureKey)
    {
        var result = new GateResult();
        var waitCursor = false;
        try
        {
            // 内存优先、磁盘兜底：不勾选「记住密码」时磁盘上没有登录态，
            // token 仍在 AuthSession 内存里 —— 闸门/EDL 必须走这里，否则 noauth → 引导不发。
            var token = AuthSession.ResolveToken();
            if (string.IsNullOrEmpty(token))
            {
                result.Reason = "noauth";
                return result;
            }

            if (Application.Current?.Dispatcher.CheckAccess() == true)
            {
                Mouse.OverrideCursor = Cursors.Wait;
                waitCursor = true;
            }

            for (var attempt = 0; attempt < 2; attempt++)
            {
                try
                {
                    // v2：app/check 与 app/consume 是受保护端点，走 RequestSigner（Bearer 照旧）
                    // ConfigureAwait(false) 很关键：避免 await 续体回到 UI 线程 —— 那样
                    // 在 UI 线程上 GetResult() 会直接死锁。这里必须让续体跑在线程池上。
                    using var resp = Core.Net.RequestSigner
                        .PostJsonAsync(GateHttp, AppConfig.ServerUrl + path,
                            // 不能用 `Serialize(new { featureKey })`：匿名类型只有带参构造函数，
                            // 混淆器会清空参数名 → STJ 抛 NotSupportedException（详见 JsonBody 的注释）
                            JsonBody.Of(("featureKey", featureKey)),
                            r => r.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token))
                        .ConfigureAwait(false).GetAwaiter().GetResult();
                    var json = resp.Content.ReadAsStringAsync().ConfigureAwait(false).GetAwaiter().GetResult();

                    // 被管理员强制下线（401 + code=KICKED）：与「登录态自然过期」必须分开 ——
                    // 普通 401 只返回 noauth 让调用方提示，不能顺手把用户踢出程序。
                    // 这里不再重试（第二次请求必然同样被拒）。
                    if (ForceLogout.IsKicked((int)resp.StatusCode, json, out var kickMsg))
                    {
                        ForceLogout.Trigger(kickMsg);
                        result.Reason = "kicked";
                        return result;
                    }

                    // IP 被封禁（403 + code=IP_BANNED）：服务端在 message 里给了封禁原因/到期时间，
                    // 原样带出来展示。Online 置 true 是有意的 —— 我们**拿到了明确结论**，
                    // 不是「连不上服务器」，提示语完全不同（别让用户去检查网络）。
                    if ((int)resp.StatusCode == 403)
                    {
                        ParseError(json, out var code, out var msg);
                        if (code == "IP_BANNED")
                        {
                            result.Online = true;
                            result.Reason = "banned";
                            result.ServerMessage = msg;
                            return result;
                        }
                    }

                    // 版本硬闸门（426 + code=VERSION_TOO_OLD）：服务端已关停旧版本客户端。
                    // 🔴 必须有这一支：少了它就会掉进 ParseGate 的失败分支 → Reason=offline →
                    //    用户看到「离线状态下不能放行」跑去查网络，而真正该做的是升级。
                    //    Online 置 true 是有意的 —— 我们**拿到了明确结论**，不是连不上。
                    if ((int)resp.StatusCode == 426)
                    {
                        ParseError(json, out var vcode, out var vmsg);
                        if (vcode == "VERSION_TOO_OLD")
                        {
                            result.Online = true;
                            result.Reason = "version";
                            result.ServerMessage = vmsg;
                            return result;
                        }
                    }

                    var parsed = ParseGate(json);
                    if (parsed != null) return parsed;

                    // 401/403 = token 失效/未登录 → 必须归 noauth（提示"请重新登录"）。
                    // 原来落 badresp 会让用户看到"无法连接服务器"，完全指错方向。
                    // ⚠️ `msg` 只在上面 403 IP_BANNED 分支的作用域里，这里重新取一次。
                    var httpCode = (int)resp.StatusCode;
                    if (httpCode is 401 or 403)
                    {
                        ParseError(json, out var errCode, out var errMsg);
                        // 账号被封（authMiddleware 的 403 code=BANNED）不是"没登录"：
                        // 归 banned 才能把服务端给的封禁原因/到期时间原样展示，
                        // 否则用户会陷入"提示重新登录 → 重新登录还是不行"的死循环。
                        if (errCode == "BANNED")
                        {
                            result.Online = true;
                            result.Reason = "banned";
                            result.ServerMessage = errMsg;
                        }
                        else
                        {
                            // 🔴 401/403 且不是封禁 = 服务端判定这枚 token 无效 → **把它丢掉**。
                            // 只返回一个 noauth 的话，磁盘里的死 token 会被 ResolveToken 读回来，
                            // UI 一直显示"已登录"，用户陷在「提示重新登录 → 界面仍显示已登录」的循环里。
                            // 只在真有 token 时才清：本来就没登录的情况不该去动磁盘文件。
                            if (httpCode == 401 && !string.IsNullOrEmpty(token))
                            {
                                try { AuthSession.Drop(); } catch { }
                            }
                            result.Reason = "noauth";
                            result.ServerMessage = errMsg;
                        }
                        return result;
                    }

                    // 响应能收到但结构不对（例如被中间人改过）→ 不重试，直接判失败
                    result.Reason = "badresp";
                    return result;
                }
                catch (Exception ex) when (attempt == 0)
                {
                    // 第一次失败（超时 / 连接被重置）重试一次，仍失败则走下面统一返回 offline。
                    // 2026-10-09：补一行日志 —— 这里原来是**空体**，于是「重试后成功」的那些
                    // 请求把那一次失败完全吞掉了（而超时/被重置往往是网络劣化的早期征兆，
                    // 也是唯一能在用户报障前留下线索的地方）。加 try 是因为日志本身不该
                    // 影响重试语义。
                    try
                    {
                        SessionLogFile.Append(DateTime.Now, "WARN",
                            $"[额度] 首次请求失败，将重试一次：{path} - {ex.GetType().Name}: {ex.Message}");
                    }
                    catch { }
                }
            }
        }
        catch { /* 任何异常都按"没拿到结论"处理 */ }
        finally
        {
            if (waitCursor)
                try { Mouse.OverrideCursor = null; } catch { }
        }
        return result;
    }

    /// <summary>从**失败**响应里取 code / message（成功响应不走这里）。</summary>
    private static void ParseError(string json, out string code, out string message)
    {
        code = ""; message = "";
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.TryGetProperty("code", out var c) && c.ValueKind == JsonValueKind.String) code = c.GetString() ?? "";
            if (root.TryGetProperty("message", out var m) && m.ValueKind == JsonValueKind.String) message = m.GetString() ?? "";
        }
        catch { }
    }

    private static GateResult? ParseGate(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (!root.TryGetProperty("success", out var ok) || ok.ValueKind != JsonValueKind.True) return null;
            if (!root.TryGetProperty("data", out var data) || data.ValueKind != JsonValueKind.Object) return null;

            var r = new GateResult { Online = true, Reason = "" };
            if (data.TryGetProperty("allowed", out var a) && a.ValueKind == JsonValueKind.True) r.Allowed = true;
            if (data.TryGetProperty("reason", out var rs) && rs.ValueKind == JsonValueKind.String) r.Reason = rs.GetString() ?? "";
            if (data.TryGetProperty("level", out var lv) && lv.ValueKind == JsonValueKind.String) r.Level = lv.GetString() ?? "";
            if (data.TryGetProperty("cap", out var cp) && cp.ValueKind == JsonValueKind.Number) r.Cap = cp.GetInt32();
            if (data.TryGetProperty("used", out var us) && us.ValueKind == JsonValueKind.Number) r.Used = us.GetInt32();
            if (data.TryGetProperty("remaining", out var rm) && rm.ValueKind == JsonValueKind.Number) r.Remaining = rm.GetInt32();
            // 模块信息只在「因模块未开通被拒」时才有值；缺字段一律留空，别让提示出现 undefined。
            if (data.TryGetProperty("module", out var md) && md.ValueKind == JsonValueKind.Object)
            {
                if (md.TryGetProperty("name", out var mn) && mn.ValueKind == JsonValueKind.String) r.ModuleName = mn.GetString() ?? "";
                if (md.TryGetProperty("title", out var mt) && mt.ValueKind == JsonValueKind.String) r.ModuleTitle = mt.GetString() ?? "";
                if (md.TryGetProperty("priceNote", out var mp) && mp.ValueKind == JsonValueKind.String) r.ModulePriceNote = mp.GetString() ?? "";
            }
            return r;
        }
        catch { return null; }
    }

    /// <summary>把服务端回来的用量写进本地镜像，纯粹为了界面显示。</summary>
    private static void NoteUsage(string featureKey, GateResult gate)
    {
        var today = DateTime.Now.ToString("yyyy-MM-dd");
        _quota[featureKey] = new QuotaEntry { Day = today, Count = Math.Max(0, gate.Used) };
        SaveQuota();
    }

    private static string DescribeFailure(GateResult gate) => gate.Online
        ? (gate.Reason == "version" ? "客户端版本过低，需升级"
           : string.IsNullOrEmpty(gate.Reason) ? "被拒绝" : gate.Reason)
        : (gate.Reason == "noauth" ? "登录状态已失效"
           : gate.Reason == "kicked" ? "账号已被管理员强制下线"
           : gate.Reason == "banned" ? "本机 IP 已被封禁"
           : "无法连接服务器");

    private static void ShowLocked(string featureLabel, string need)
    {
        var msg = string.IsNullOrEmpty(need)
            ? $"「{featureLabel}」当前未开放。"
            : $"「{featureLabel}」需要 {need} 及以上等级才能使用。\n\n可在控制台「会员中心」查看开通方式。";
        MessageBox.Show(msg, "需要会员", MessageBoxButton.OK, MessageBoxImage.Information);
    }

    /// <summary>服务端拒绝 / 没拿到结论时的提示。文案要能指导用户下一步怎么做。</summary>
    private static void ShowGateDenied(string featureLabel, string featureKey, GateResult gate, string need)
    {
        // 版本硬闸门：服务端已停止对旧版本提供服务。这既不是网络问题、也不是额度问题，
        // 唯一出路是**升级**，必须放在最前面说清楚 —— 否则用户会反复重试 / 去查网络。
        if (gate.Reason == "version")
        {
            MessageBox.Show(
                (string.IsNullOrEmpty(gate.ServerMessage)
                    ? "当前客户端版本过低，服务端已停止对旧版本提供服务。"
                    : gate.ServerMessage)
                    + "\n\n请到官网下载最新版本后再使用。",
                "需要升级客户端", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        // IP 被封禁：状态是「这台机器整体访问不了服务端」，原因/到期只有服务端知道 —— 原样转达。
        // 必须放在 !Online 之前判：它是 Online=true 的明确结论，不能走到「请检查网络」那段去。
        if (gate.Reason == "banned")
        {
            MessageBox.Show(
                string.IsNullOrEmpty(gate.ServerMessage)
                    ? "你的 IP 已被管理员封禁，暂时无法使用联网功能。"
                    : gate.ServerMessage,
                "IP 已被封禁", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (!gate.Online)
        {
            var msg = gate.Reason == "noauth"
                ? $"登录状态已失效，无法校验「{featureLabel}」的使用额度。\n\n请重新登录后再试。"
                : $"无法连接服务器校验「{featureLabel}」的使用额度。\n\n请检查网络后重试；" +
                  "额度由服务器统一记账，离线状态下不能放行。";
            MessageBox.Show(msg, "无法校验额度", MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }

        if (gate.Reason == "exhausted")
        {
            var levelLabel = string.IsNullOrEmpty(gate.Level) ? AccountText.VipLevel(Level) : AccountText.VipLevel(gate.Level);
            MessageBox.Show(
                $"「{featureLabel}」当前等级（{levelLabel}）每天可使用 {gate.Cap} 次，今日额度已用完，明天自动恢复；\n升级会员可获得更高额度。",
                "额度已用完", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        // 增值模块未开通：与「等级不够」是两件事 —— 升级会员解决不了，必须单独开通该模块。
        // 混成一句「需要 XX 及以上等级」会让用户白买一个会员，是明确的错误引导。
        if (gate.Reason == "module")
        {
            var modName = string.IsNullOrEmpty(gate.ModuleName) ? "对应的增值模块" : gate.ModuleName;
            var title = string.IsNullOrEmpty(gate.ModuleTitle) ? "" : $"（{gate.ModuleTitle}）";
            var price = string.IsNullOrEmpty(gate.ModulePriceNote) ? "" : $"\n{gate.ModulePriceNote}";
            MessageBox.Show(
                $"「{featureLabel}」属于 {modName}{title}，当前账号尚未开通该模块。\n\n" +
                "模块需单独开通（不随会员等级赠送），开通后本模块内全部功能都可用。\n" +
                "可在控制台「会员中心」查看开通方式。" + price,
                "需要开通模块", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        // locked 或原因不明：按「未开放」提示（文案与本地判定一致）
        ShowLocked(featureLabel, need);
    }

    private static int UsedToday(string featureKey)
    {
        var today = DateTime.Now.ToString("yyyy-MM-dd");
        return (!_quota.TryGetValue(featureKey, out var e) || e.Day != today) ? 0 : e.Count;
    }

    // ===================== 额度配置模型（仅展示） =====================

    private sealed class QuotaEntry
    {
        public string Day { get; set; } = "";
        public int Count { get; set; }
    }

    private sealed class LevelQuota
    {
        /// <summary>该等级可用的功能 key；含 "*" 表示全部开放（含以后新增的功能）。</summary>
        public List<string> Open { get; } = new();
        /// <summary>功能 key → 每日次数上限（0 或缺失 = 不限次）。</summary>
        public Dictionary<string, int> Limits { get; } = new();

        public bool Opens(string key) => Open.Contains("*") || Open.Contains(key);

        public int Limit(string key) => Limits.TryGetValue(key, out var v) && v > 0 ? v : 0;
    }

    private sealed class LimitsConfig
    {
        public Dictionary<string, LevelQuota> Levels { get; set; } = Default();

        /// <summary>
        /// 兜底默认：必须与 server.js 的 DEFAULT_MEMBER_LIMITS 保持一致。
        /// 只在「服务端不可达且本地没有缓存」时生效 —— 而且它<b>不再决定放行</b>
        /// （放行要服务端 check 通过），所以这里配置漂移最多让提示文案不准，不会漏放。
        ///
        /// 2026-09-28：每日次数限制已整体停用（用户要求「把会员额度删除」）。服务端
        /// getMemberLimits() 现在固定返回「全开放 + 不限次」，这里的次数表也必须一并清空 ——
        /// 否则离线时界面会显示「今日剩余 2 次」这种早就废弃的说法，用户会以为额度还在生效。
        /// </summary>
        private static Dictionary<string, LevelQuota> Default()
        {
            // Make("*", null)：全部功能开放、不设任何次数上限。
            return new Dictionary<string, LevelQuota>
            {
                ["normal"] = Make("*", null),
                ["vip"] = Make("*", null),
                ["svip"] = Make("*", null),
            };
        }

        private static LevelQuota Make(string openKey, Dictionary<string, int>? limits)
        {
            var q = new LevelQuota();
            q.Open.Add(openKey);
            if (limits != null)
                foreach (var kv in limits) q.Limits[kv.Key] = kv.Value;
            return q;
        }

        public LevelQuota For(string level)
            => Levels.TryGetValue(level, out var c) ? c : (Levels.TryGetValue("normal", out var n) ? n : new LevelQuota());

        public static LimitsConfig? Parse(string json)
        {
            try
            {
                using var doc = JsonDocument.Parse(json);
                if (!doc.RootElement.TryGetProperty("data", out var data)) return null;
                if (!data.TryGetProperty("levels", out var levels) || levels.ValueKind != JsonValueKind.Object) return null;

                var cfg = new LimitsConfig { Levels = new Dictionary<string, LevelQuota>() };
                foreach (var lv in new[] { "normal", "vip", "svip" })
                {
                    // 三个等级必须齐全：缺一个就当作响应不完整，整体回落默认值，
                    // 否则缺失的等级会变成「什么都不开放」，把用户全拦下来。
                    if (!levels.TryGetProperty(lv, out var el) || el.ValueKind != JsonValueKind.Object) return null;

                    var q = new LevelQuota();
                    if (el.TryGetProperty("open", out var openEl) && openEl.ValueKind == JsonValueKind.Array)
                    {
                        foreach (var o in openEl.EnumerateArray())
                            if (o.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(o.GetString()))
                                q.Open.Add(o.GetString()!.Trim());
                    }
                    if (el.TryGetProperty("limits", out var limEl) && limEl.ValueKind == JsonValueKind.Object)
                    {
                        foreach (var p in limEl.EnumerateObject())
                        {
                            var n = p.Value.ValueKind == JsonValueKind.Number ? p.Value.GetInt32() : 0;
                            q.Limits[p.Name] = n > 0 ? n : 0;
                        }
                    }
                    cfg.Levels[lv] = q;
                }
                return cfg;
            }
            catch { return null; }
        }
    }

    // ===================== 本地展示缓存 =====================
    // ⚠️ 这两个文件**不参与放行判定**，删掉或手改都只会让「剩余次数」显示不准。
    //    放行权在服务端的 check / consume 上。

    // ===================== 本地缓存的保护（2026-10-09） =====================
    // 🔴 这两个文件原本是**无签名可编辑的明文 JSON**：删掉它、或用记事本把 open 改成 ["*"]，
    //    就能改变界面认为的额度与所需等级。拿不到服务端能力（裁决在 /api/app/check），
    //    但足够制造「我明明有额度却被拦」的错觉与大量无效报障。
    //    现在统一套一层 DPAPI(CurrentUser) + 独立 entropy：
    //      · 挡得住「记事本改」「删掉再塞一份假的」这类零技术手段；
    //      · **挡不住**任何能在这台机器上以该用户身份跑代码的东西 —— DPAPI 的 CurrentUser
    //        作用域对同用户进程是透明的。它抬高的只是门槛，不是安全边界。
    //    老版本的明文缓存会在读取时解密失败 → 回落默认值 → 下次拉取自动重建，无需迁移。
    private static readonly byte[] CacheEntropy = Encoding.Unicode.GetBytes("KunClientQuotaCache2026");

    private static string ProtectCache(string plain) =>
        Convert.ToBase64String(ProtectedData.Protect(
            Encoding.UTF8.GetBytes(plain), CacheEntropy, DataProtectionScope.CurrentUser));

    private static string? UnprotectCache(string stored)
    {
        var raw = Convert.FromBase64String(stored);
        return Encoding.UTF8.GetString(ProtectedData.Unprotect(raw, CacheEntropy, DataProtectionScope.CurrentUser));
    }

    private static Dictionary<string, QuotaEntry> LoadQuota()
    {
        try
        {
            if (File.Exists(QuotaPath))
                return JsonSerializer.Deserialize<Dictionary<string, QuotaEntry>>(
                           UnprotectCache(File.ReadAllText(QuotaPath)) ?? "{}")
                       ?? new Dictionary<string, QuotaEntry>();
        }
        catch { /* 含：老版本明文缓存 / 被改过的文件 → 解密失败 → 当作没有 */ }
        return new Dictionary<string, QuotaEntry>();
    }

    private static void SaveQuota()
    {
        try
        {
            Directory.CreateDirectory(StoreDir);
            File.WriteAllText(QuotaPath, ProtectCache(JsonSerializer.Serialize(_quota)));
        }
        catch { }
    }

    private static LimitsConfig LoadLimitsCache()
    {
        try
        {
            if (File.Exists(LimitsCachePath))
                return LimitsConfig.Parse(UnprotectCache(File.ReadAllText(LimitsCachePath)) ?? "") ?? new LimitsConfig();
        }
        catch { /* 同上：解不出来就回落默认，等下一次拉取重建 */ }
        return new LimitsConfig();
    }

    private static void SaveLimitsCache(string json)
    {
        try
        {
            Directory.CreateDirectory(StoreDir);
            File.WriteAllText(LimitsCachePath, ProtectCache(json));
        }
        catch { }
    }
}
