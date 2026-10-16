// ============================================================================
// EnvReportService —— 把环境检测结果上报给服务端（2026-09-30 / 2026-10-01 增强）
//
// 上报链路：**先取一次性 nonce + 防伪校验题 → 解防伪校验题 → 用内嵌密钥给 payload 算 HMAC
//           → nonce / payload / sig / answer 一起发过去**。
//
// 🔴 三个必须这么做的原因：
//   ① 没有 nonce = 上报可被重放/伪造，尤其能被用来**陷害别人**（伪造他人「环境异常」）。
//   ② payload 由客户端自己拼好**原文**再发（服务端不重新 JSON.stringify 后验签）:
//      键序/转义任何一处不一致就会**全体用户**签名校验失败 —— 那是我们的 bug，不是用户作弊。
//   ③ 防伪校验题（puzzle）要求客户端**真的算一遍**才答得出：普通「抓包回放」拿不到新题的答案，
//      把门槛从「复制一个包」抬到「得跑原版客户端」。算错 = challenge_fail（HIGH）。
//
// ⚠️ 能力边界：客户端侧的 HMAC 密钥 / 防伪校验题常量**必然能被提取**，所以它们不是安全边界。
//    它们挡的是「随手改包 / 随手伪造上报 / 回放旧包」，并用于检出「客户端与服务端版本失配」。
//    专业逆向者会直接把这段 NOP 掉 —— 别拿它去承诺防破解。
// ============================================================================
using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

namespace KunFlashTool.App.Services
{
    internal static class EnvReportService
    {
        /// <summary>上报签名密钥的种子（**乱序片段 + 索引表**，2027 轮换）。
        /// 片段故意乱序存放，真实拼合顺序由 SeedOrder 给出 —— 增量成本为零：
        /// 拆成片段能让"照着数组从上往下抄"失效，读 IL 还原的人还得多解一层索引。
        /// 🔴 必须与服务端 server.js 的 ENV_REPORT_SEED_STORE / _ORDER **逐字一致**，
        ///    否则全体用户 bad_hmac（服务端有熔断，不会自动封号，但功能失效）。</summary>
        private static readonly string[] SeedStore =
            { "t-En", "ort-", "KunC", "-Sig", "lien", "2027", "nKey", "vRep" };
        private static readonly int[] SeedOrder = { 2, 4, 0, 7, 1, 5, 3, 6 };

        /// <summary>防伪校验题答案的盐值（同样**乱序片段 + 索引表**）。
        /// 🔴 2026 版这里曾是 `private const string` —— `const` 的值落在元数据 Constant 表，
        ///    Obfuscar 的 HideStrings 只处理方法体的 ldstr、**碰不到 Constant 表**，
        ///    所以旧值 `Kun-Firehose-2026` 会以**明文**留在产物里（实测已复现）。
        ///    现在改成 static readonly + 片段拼合：既离开常量表，也不以完整字面量出现。
        /// 🔴 必须与服务端 ENV_PUZZLE_SALT_STORE / _ORDER **逐字一致**，否则全体 challenge_fail。</summary>
        private static readonly string[] SaltStore =
            { "-En", "reh", "Kun", "-Fi", "v20", "ose", "27" };
        private static readonly int[] SaltOrder = { 2, 3, 1, 5, 0, 4, 6 };

        private static byte[] _key = null!;
        private static byte[] Key()
        {
            if (_key != null) return _key;
            var sb = new StringBuilder();
            foreach (var i in SeedOrder) sb.Append(SeedStore[i]);   // 按索引表取，不是存放顺序
            var seed = Encoding.UTF8.GetBytes(sb.ToString());
            _key = SHA256.HashData(seed);
            Array.Clear(seed, 0, seed.Length);                      // 拼出来的种子是最值钱的几个字节，用完即抹
            return _key;
        }

        /// <summary>防伪校验题答案盐值（运行期按索引表拼出，不以完整字面量出现）。</summary>
        private static string PuzzleSalt()
        {
            var sb = new StringBuilder(SaltStore.Length * 3);
            foreach (var i in SaltOrder) sb.Append(SaltStore[i]);
            return sb.ToString();
        }

        private static string HmacHex(string nonce, string payload)
        {
            var mac = HMACSHA256.HashData(Key(), Encoding.UTF8.GetBytes(nonce + "|" + payload));
            return Convert.ToHexString(mac).ToLowerInvariant();
        }

        private static readonly HttpClient Http = ServerGate.CreateClient(TimeSpan.FromSeconds(15));

        /// <summary>上一次上报的时间，用来做节流（单次上报本身不用定时器）。</summary>
        private static long _lastReport;
        private static readonly TimeSpan MinInterval = TimeSpan.FromMinutes(30);

        // ── 运行期实时复检（2026-10-01）────────────────────────────────────
        // 登录时做一次，之后由这个后台定时器每 10 分钟静默复检一次：
        // 覆盖「运行期中途被注入 Hook / 加载可疑模块」这类启动时查不到的情况。
        private static Timer _recheckTimer = null!;
        private static volatile string _recheckToken = null!;
        private static readonly TimeSpan RecheckInterval = TimeSpan.FromMinutes(10);

        /// <summary>上报一次。失败**一律静默** —— 环境检测是附加功能，
        /// 网络不通 / 服务端没这个接口（旧版本服务端）都不许影响用户正常使用。</summary>
        public static bool ReportIfNeeded(string token, bool force = false)
        {
            if (string.IsNullOrEmpty(token)) return false;
            ArmRecheck(token);   // 记录 token 并（首次）启动运行期复检定时器

            if (!force && DateTime.UtcNow.Ticks - Interlocked.Read(ref _lastReport) < MinInterval.Ticks) return false;

            try
            {
                if (!FetchChallenge(token, out var nonce, out var puzzle)) return false;
                if (string.IsNullOrEmpty(nonce)) return false;
                var answer = SolvePuzzle(puzzle);

                var signals = EnvInspector.Collect();
                var payload = BuildPayload(signals);
                var sig = HmacHex(nonce, payload);

                // v1：env/report 是受保护端点，走 RequestSigner（外层信封不参与环境签名，
                // nonce + payload 才是被签的内容，与请求签名互不干扰）。
                using var resp = Core.Net.RequestSigner
                    .PostJsonAsync(Http, AppConfig.ServerUrl + "/api/v1/client/env/report",
                        JsonBody.Of(("nonce", nonce), ("payload", payload), ("sig", sig), ("answer", answer)),
                        r => r.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token))
                    .ConfigureAwait(false).GetAwaiter().GetResult();
                var json = resp.Content.ReadAsStringAsync().ConfigureAwait(false).GetAwaiter().GetResult();
                Interlocked.Exchange(ref _lastReport, DateTime.UtcNow.Ticks);

                // 被封禁（403 + code=BANNED）：上报自己触发了自动封禁。
                // 这里**不能**走 ForceLogout —— 那会强制重启客户端，用户可能正在刷机（既有红线）。
                // 只把结果交给调用方去提示，让当前操作有机会正常收尾。
                if ((int)resp.StatusCode == 403 && json != null && json.Contains("BANNED"))
                {
                    LastBanJson = json;
                    return true;
                }
                return resp.IsSuccessStatusCode;
            }
            catch
            {
                return false;   // 静默：环境检测绝不许把主流程带崩
            }
        }

        /// <summary>记录 token 并启动运行期复检定时器（幂等：只启动一次）。</summary>
        private static void ArmRecheck(string token)
        {
            _recheckToken = token;
            if (_recheckTimer != null) return;
            try
            {
                _recheckTimer = new Timer(_ =>
                {
                    try
                    {
                        var t = _recheckToken;
                        if (!string.IsNullOrEmpty(t)) ReportIfNeeded(t, force: true);
                    }
                    catch { }
                }, null, RecheckInterval, RecheckInterval);
            }
            catch { }
        }

        /// <summary>最近一次上报拿到的封禁响应原文（供 UI 提示用）。</summary>
        public static string LastBanJson { get; private set; } = "";

        // ══════════════════════════════════════════════════════════════════════
        // 启动时的**免登录**环境上报（2026-10-09）
        // 用户要求「工具一打开就开始检测」：登录前也要把命中结果送出去。
        // 没有账号可归因时，服务端按 IP 处置（封 IP + 邮件通知开发者，见 server.js）。
        // ══════════════════════════════════════════════════════════════════════

        /// <summary>
        /// 值得在登录前专门上报的信号 —— 与服务端 ENV_SIGNAL_LEVEL 的 mid/high 对齐。
        /// 弱信号（debugger / vm / known_tool / suspicious_module / remote_session）
        /// 不值得在登录前多发一趟：它们本就不参与自动封禁。
        /// 🔴 这份名单必须与服务端的等级表保持同步，否则「客户端以为严重、服务端当弱信号」
        ///    或反过来，都会让判据悄悄失效。
        /// </summary>
        private static readonly HashSet<string> SeriousSignals = new(StringComparer.Ordinal)
        {
            "integrity_fail", "debug_bypassed", "inline_hook", "injected_module",
            // 2026-10-09：known_tool 随服务端一起提级到 MID，所以未登录时也要送 ——
            // 否则「机器上跑着 IDA / x64dbg」这件事只在登录后才留痕，登录前完全看不见。
            "known_tool",
            "intercept_tool", "proxy_intercept", "root_ca_injected",
        };

        /// <summary>
        /// **启动时**跑一次环境检测；只有未登录、且命中 MID/HIGH 信号时才上报。
        ///
        /// 与 <see cref="ReportIfNeeded"/> 的唯一区别是没有 token。已登录时这里**直接返回**，
        /// 交给常规链路 —— 避免同一次启动发两遍，也避免把「有账号可归因」的情况降级成按 IP 处理。
        ///
        /// 🔴 全程静默：启动路径上任何异常都不许冒出来（网络不通、服务端还没有这个接口
        ///    —— 打到旧版服务端上 —— 都是正常情况，不该影响用户开工具）。
        /// </summary>
        public static bool ReportAnonymousIfNeeded()
        {
            try
            {
                if (!string.IsNullOrEmpty(AuthSession.ResolveToken())) return false;   // 已登录 → 走常规链路

                var signals = EnvInspector.Collect();
                var serious = 0;
                foreach (var s in signals)
                    if (SeriousSignals.Contains(s.Signal)) serious++;
                if (serious == 0) return false;      // 没有值得打扰服务端的信号

                if (!FetchAnonymousChallenge(out var nonce, out var puzzle)) return false;
                var answer = SolvePuzzle(puzzle);
                var payload = BuildPayload(signals);
                var sig = HmacHex(nonce, payload);

                // v1：env/anonymous 是受保护端点，走 RequestSigner
                using var resp = Core.Net.RequestSigner
                    .PostJsonAsync(Http, AppConfig.ServerUrl + "/api/v1/client/env/anonymous",
                        JsonBody.Of(("nonce", nonce), ("payload", payload), ("sig", sig), ("answer", answer)))
                    .ConfigureAwait(false).GetAwaiter().GetResult();
                return resp.IsSuccessStatusCode;
            }
            catch { return false; }
        }

        /// <summary>取免登录的一次性 nonce 与防伪校验题（服务端按 IP 签发）。取不到就放弃本次上报。</summary>
        private static bool FetchAnonymousChallenge(out string nonce, out string puzzle)
        {
            nonce = "";
            puzzle = "";
            try
            {
                // v1：challenge-anon 是受保护端点，走 RequestSigner
                using var resp = Core.Net.RequestSigner
                    .PostJsonAsync(Http, AppConfig.ServerUrl + "/api/v1/client/env/challenge-anon", "{}")
                    .ConfigureAwait(false).GetAwaiter().GetResult();
                if (!resp.IsSuccessStatusCode) return false;
                var json = resp.Content.ReadAsStringAsync().ConfigureAwait(false).GetAwaiter().GetResult();
                nonce = ExtractJsonString(json, "nonce");
                puzzle = ExtractJsonString(json, "puzzle");
                return !string.IsNullOrEmpty(nonce);
            }
            catch { return false; }
        }

        /// <summary>取一次性 nonce 与防伪校验题。返回 false = 拿不到（网络/旧服务端），调用方直接放弃本次上报。</summary>
        private static bool FetchChallenge(string token, out string nonce, out string puzzle)
        {
            nonce = "";
            puzzle = "";
            try
            {
                // v1：challenge 是受保护端点，走 RequestSigner
                using var resp = Core.Net.RequestSigner
                    .PostJsonAsync(Http, AppConfig.ServerUrl + "/api/v1/client/env/challenge", "{}",
                        r => r.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token))
                    .ConfigureAwait(false).GetAwaiter().GetResult();
                if (!resp.IsSuccessStatusCode) return false;
                var json = resp.Content.ReadAsStringAsync().ConfigureAwait(false).GetAwaiter().GetResult();
                nonce = ExtractJsonString(json, "nonce");
                puzzle = ExtractJsonString(json, "puzzle");
                return !string.IsNullOrEmpty(nonce);
            }
            catch { return false; }
        }

        /// <summary>解防伪校验题：puzzle 形如 "a op b"（op ∈ + - *）。
        /// 算出结果后拼上盐常量作为答案。解析失败返回空串（服务端对空答案不判 challenge_fail，
        /// 避免客户端小 bug 触发群体误封）。</summary>
        private static string SolvePuzzle(string puzzle)
        {
            try
            {
                if (string.IsNullOrWhiteSpace(puzzle)) return "";
                var parts = puzzle.Trim().Split(' ');
                if (parts.Length != 3) return "";
                if (!long.TryParse(parts[0], out var a)) return "";
                if (!long.TryParse(parts[2], out var b)) return "";
                long r;
                switch (parts[1])
                {
                    case "+": r = a + b; break;
                    case "-": r = a - b; break;
                    case "*": r = a * b; break;
                    default: return "";
                }
                return r.ToString() + PuzzleSalt();
            }
            catch { return ""; }
        }

        /// <summary>极简 JSON 字符串取值（不引第三方解析器，与本项目其它地方一致）。</summary>
        private static string ExtractJsonString(string json, string key)
        {
            if (string.IsNullOrEmpty(json)) return "";
            var i = json.IndexOf("\"" + key + "\"", StringComparison.Ordinal);
            if (i < 0) return "";
            var c = json.IndexOf(':', i);
            if (c < 0) return "";
            var s = json.IndexOf('"', c);
            if (s < 0) return "";
            var e = json.IndexOf('"', s + 1);
            if (e < 0) return "";
            return json.Substring(s + 1, e - s - 1);
        }

        /// <summary>被签名的 payload 原文。**键序固定**，服务端直接拿它验签。</summary>
        private static string BuildPayload(List<EnvSignal> signals)
        {
            var sb = new StringBuilder();
            sb.Append("{\"build\":\"").Append(UpdateService.CurrentVersion).Append("\"");
            sb.Append(",\"instanceId\":\"").Append(EnvInspector.InstanceId()).Append("\"");
            sb.Append(",\"signals\":[");
            for (var i = 0; i < signals.Count; i++)
            {
                if (i > 0) sb.Append(',');
                sb.Append("{\"signal\":\"").Append(Escape(signals[i].Signal)).Append("\"");
                sb.Append(",\"detail\":\"").Append(Escape(signals[i].Detail)).Append("\"}");
            }
            sb.Append("]}");
            return sb.ToString();
        }

        private static string Escape(string s)
        {
            if (string.IsNullOrEmpty(s)) return "";
            return s.Replace("\\", "\\\\").Replace("\"", "\\\"")
                    .Replace("\r", "").Replace("\n", " ");
        }
    }
}
