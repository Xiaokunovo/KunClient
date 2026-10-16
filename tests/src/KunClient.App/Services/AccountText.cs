using System;
using System.Globalization;
using System.Text.RegularExpressions;

namespace KunFlashTool.App.Services
{
    /// <summary>
    /// 账号文案（角色 / 会员等级）统一出口，同时也是**会员到期时间的唯一解析入口**。
    /// 工具端（客户端）、官网、控制台必须用同一套叫法，否则用户会看到
    /// 「控制台显示 特权会员、客户端显示 会员：永久」这种不一致。
    /// 对照服务端 server.js：
    ///   ROLE_LABEL      = { user:'普通用户', agent:'代理', admin:'管理员', developer:'开发者' }
    ///   VIP_LEVEL_LABEL = { normal:'普通用户', vip:'高级会员', svip:'特权会员' }
    /// 服务端还会在 /api/profile 与 /api/users 里直接下发 role / vipLevel / vipLevelLabel，
    /// 客户端优先用服务端给的 label，取不到时再用本表兜底。
    /// </summary>
    public static class AccountText
    {
        /// <summary>
        /// ISO 8601「扩展年份」写法（年份 5 位以上、带 + 前缀），如 <c>+010099-12-06T00:00:00.000Z</c>。
        /// 服务端的「永久」是用远期到期日表示的（PERMANENT_VIP_ISO = 9999-12-31），
        /// 若在**已经是永久**的账号上继续叠加天数，日期就会越过 9999 年，
        /// JS 的 <c>toISOString()</c> 便输出这种写法。
        /// .NET 的 DateTime 上限正是 9999 年 → <c>DateTime.TryParse</c> 必然失败，
        /// 表现为「永久会员既不显示『永久』、还被当成非会员限次」。
        /// </summary>
        private static readonly Regex ExpandedYear = new(@"^\+\d{5,}-", RegexOptions.Compiled);

        /// <summary>角色显示名（与官网/控制台一致）。</summary>
        public static string Role(string? role) => Normalize(role) switch
        {
            "developer" => "开发者",
            "admin" => "管理员",
            "agent" => "代理",
            _ => "普通用户",
        };

        /// <summary>会员等级显示名（与官网/控制台一致）。</summary>
        public static string VipLevel(string? level) => Normalize(level) switch
        {
            "svip" => "特权会员",
            "vip" => "高级会员",
            _ => "普通用户",
        };

        /// <summary>
        /// 生效等级 key（<c>normal</c> / <c>vip</c> / <c>svip</c>）—— **纯函数**，便于单测。
        /// <para>
        /// 🔴 2026-10-09 逆向报告 D4 之后改成 fail-closed。原来的写法是
        /// <c>!(lv=="vip") &amp;&amp; !(lv=="svip") ? "vip" : lv</c>（外加一个恒假的 <c>1 == 0</c>），
        /// 实测连服务端**显式**下发的 <c>"normal"</c> 都会被升成 <c>"vip"</c>；
        /// 配合 <c>+99999-</c> 这类扩展年份（解析成 <c>DateTime.MaxValue</c>），
        /// 服务端只要回一个认不出来的等级串就能让客户端显示成永久 VIP。
        /// </para>
        /// <para>
        /// 唯一保留的"宽松"分支是 <paramref name="fromServer"/> 为 false：那时服务端这次登录
        /// **根本没有下发等级字段**（老接口 / 只有 vip_expire_at 的历史账号），
        /// 只能按"有到期时间即会员"的老语义兜成 vip —— 这是兼容，不是授权。
        /// </para>
        /// </summary>
        public static string EffectiveLevel(string? vipLevel, bool fromServer)
        {
            if (!fromServer) return "vip";     // 历史账号：没有等级字段，只有到期时间
            var lv = Normalize(vipLevel);
            return lv is "vip" or "svip" ? lv : "normal";
        }

        /// <summary>
        /// 解析服务端下发的会员到期时间。
        /// 正常 ISO 串按本地时间返回；<see cref="ExpandedYear"/> 这种超出 DateTime 上限的
        /// 写法统一用 <see cref="DateTime.MaxValue"/> 表示（语义上就是「永久」）。
        /// 未下发 / 解析不出来返回 false，由调用方按「无会员信息」处理。
        /// </summary>
        public static bool TryParseExpire(string? vipExpire, out DateTime end)
        {
            end = default;
            if (string.IsNullOrWhiteSpace(vipExpire)) return false;

            if (DateTime.TryParse(vipExpire, CultureInfo.InvariantCulture,
                    DateTimeStyles.AdjustToUniversal, out var parsed))
            {
                end = parsed.ToLocalTime();
                return true;
            }

            // 五位以上年份解析必失败，但语义上就是「永久」——不能让它退化成「没有会员信息」。
            if (ExpandedYear.IsMatch(vipExpire.Trim()))
            {
                end = DateTime.MaxValue;
                return true;
            }
            return false;
        }

        /// <summary>会员是否仍在有效期内（未下发 / 解析失败 / 已过期 → false）。</summary>
        public static bool IsActiveVip(string? vipExpire)
            => TryParseExpire(vipExpire, out var end) && end > DateTime.Now;

        /// <summary>是否永久会员（远期到期日，含扩展年份写法）。</summary>
        public static bool IsPermanentVip(string? vipExpire)
            => TryParseExpire(vipExpire, out var end) && IsPermanentEnd(end);

        /// <summary>
        /// 窗口标题（任务栏悬停可见）里的会员段文案，固定写成
        /// 「会员类型：X · 到期时间：Y · 个性签名：Z」，让任务栏里一眼看清等级、到期情况与签名。
        /// 永久会员（到期日 ≥ 10 年后，含服务端越界产生的扩展年份写法）到期时间只显示
        /// 「永久」，不再出现 2054 / 9999 这类无意义年份。
        /// </summary>
        /// <param name="signature">
        /// 个性签名，可为空 —— 空则不追加签名段（**不显示「个性签名：」空标签**）。
        /// </param>
        public static string VipTitle(string? level, string? vipExpire, string? signature = null)
        {
            var text = VipTitleCore(level, vipExpire);
            var sig = NormalizeSignature(signature);
            return sig.Length == 0 ? text : $"{text} · 个性签名：{sig}";
        }

        /// <summary>
        /// 标题里个性签名最多显示多少字。
        /// <para>
        /// 服务端允许 100 字签名，而窗口标题栏是**单行**且宽度有限（约能放 30 来个汉字）。
        /// 原样拼进去的结果是 Windows 直接把尾部硬切掉 —— 用户看到的是「…到期时间：永」这种
        /// 半截字，比截断更难看。所以这里主动截断并补省略号。
        /// </para>
        /// </summary>
        public const int TitleSignatureMaxChars = 24;

        /// <summary>
        /// 标题栏可安全展示的签名：去掉换行/制表符（标题是单行控件，换行会渲染成方块），
        /// 超长则截断加省略号。
        /// </summary>
        private static string NormalizeSignature(string? signature)
        {
            var s = (signature ?? string.Empty).Trim();
            if (s.Length == 0) return string.Empty;

            // 签名来自网页 textarea，可能带 \r\n / 连续空格 —— 统一压成单个空格。
            s = Regex.Replace(s, @"\s+", " ").Trim();
            if (s.Length == 0) return string.Empty;

            return s.Length <= TitleSignatureMaxChars ? s : s[..TitleSignatureMaxChars] + "…";
        }

        /// <summary>会员段本体（不含签名），供 <see cref="VipTitle"/> 复用。</summary>
        private static string VipTitleCore(string? level, string? vipExpire)
        {
            var label = VipLevel(level);
            if (!TryParseExpire(vipExpire, out var end))
                return $"会员类型：{label} · 到期时间：未开通";

            if (end <= DateTime.Now)
                return $"会员类型：{label} · 到期时间：已过期";

            // 只给了到期时间、没给等级（老服务端/兼容路径）：有有效会员就按高级会员显示
            if (label == "普通用户") label = "高级会员";

            var expireText = IsPermanentEnd(end) ? "永久" : $"{end:yyyy-MM-dd}";
            return $"会员类型：{label} · 到期时间：{expireText}";
        }

        /// <summary>
        /// 设置页「会员到期时间」一栏的文案。与窗口标题共用同一套解析规则，
        /// 避免标题写「永久」而设置页写「2054-01-01」这种自相矛盾。
        /// </summary>
        public static string VipExpireText(string? vipExpire, bool permanent, int daysLeft)
        {
            if (!TryParseExpire(vipExpire, out var end)) return "—";
            if (permanent || IsPermanentEnd(end)) return "永久有效";
            if (end <= DateTime.Now) return $"已过期（{end:yyyy-MM-dd}）";

            var days = daysLeft > 0 ? daysLeft : (int)Math.Ceiling((end - DateTime.Now).TotalDays);
            return $"{end:yyyy-MM-dd}（剩余 {days} 天）";
        }

        /// <summary>远期到期日（≥ 10 年后，或超出 DateTime 上限）一律视为「永久」。</summary>
        private static bool IsPermanentEnd(DateTime end)
            => end == DateTime.MaxValue || end.Year >= DateTime.Now.Year + 10;

        private static string Normalize(string? v) => (v ?? string.Empty).Trim().ToLowerInvariant();
    }
}
