using System;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace KunFlashTool.Core.Services
{
    /// <summary>
    /// 用户名校验的**唯一出口**（2026-09-29）。
    ///
    /// 与两条服务端规则**逐字对应**，别只改一边：
    ///   · server/server.js 的 <c>usernameError()</c> → 字符集与长度
    ///   · server/server.js 的 <c>isAllDigitsUsername()</c> → 「不能是纯数字」
    ///   · server/console/index.html 的 <c>usernameErr()</c> → 控制台那份镜像
    /// 这里是**即时反馈**，不是门槛：真正的判定始终在服务端（客户端能被绕过）。
    /// 两边不一致的后果是「前端放过去、后端打回来」，用户会觉得自己被莫名其妙拒绝了。
    ///
    /// 为什么按 <see cref="Rune"/> + UnicodeCategory 判，而不是 <c>char.IsDigit</c> / <c>\d</c>：
    ///   · <c>char.IsDigit</c> 只认 **Nd**（十进制数字）。罗马数字 <c>Ⅻ</c> 是 **Nl**、
    ///     上标 <c>²</c> 与分数 <c>½</c> 是 **No** —— 这几个在用户眼里同样是「数字」，
    ///     用 char.IsDigit 会全部放过去；而 JS 侧的 <c>\p{Nd}\p{Nl}\p{No}</c> 能拦住，
    ///     两边就不一致了（用户提这条要求的目的就是别让账号名看起来像编号）。
    ///   · 按 <c>char</c> 逐个遍历还有第二个坑：补充平面的数字（如 U+1D7CF 数学粗体 1）
    ///     是代理对，单个 char 会判成 Surrogate 而非数字，同样漏过去；按 Rune 就不会。
    /// </summary>
    public static class UsernameRule
    {
        /// <summary>服务端 USERNAME_RE 的同款字符集（中文 + 字母 + 数字 + _ . - @，2-24 位）。</summary>
        private static readonly Regex Charset =
            new(@"^[\u4e00-\u9fffA-Za-z0-9_.\-@]{2,24}$", RegexOptions.Compiled);

        public const string CharsetMessage = "用户名需 2-24 位，支持中文、字母、数字、下划线、中划线、点和 @";
        public const string AllDigitsMessage = "用户名不能是纯数字，请混入字母、中文或下划线（例如 kun123）";

        /// <summary>
        /// 校验用户名。<b>返回 null = 通过</b>，否则返回可直接展示给用户的中文文案。
        ///
        /// 🔴 顺序是「**先判纯数字**，再判字符集」，三段实现必须一致（server.js /
        /// console/index.html / 这里）。理由：全角数字 `１２３` 根本不在字符集白名单里，
        /// 若字符集优先，它会报「用户名需 2-24 位，支持中文、字母、数字…」（用户看着莫名其妙：
        /// 明明都是数字）。纯数字优先后，任何纯数字名在任何入口都得到同一句话。
        /// ⚠️ 2026-09-30：`POST /api/users`（管理员建号）原本自成一套（只有 `length &lt; 3`
        /// + 纯数字），现已统一到服务端的 `usernameError()` —— 五个入口一套规则。
        /// 文案也必须**逐字**一致（曾出现服务端「2-24 个字符」vs 本文件「2-24 位」的漂移）。
        /// </summary>
        public static string? Error(string? v)
        {
            var s = (v ?? string.Empty).Trim();
            if (IsAllDigits(s)) return AllDigitsMessage;
            if (!Charset.IsMatch(s)) return CharsetMessage;
            return null;
        }

        /// <summary>
        /// 是否「整串都是数字」。空串返回 false（空值属于「没填」，由各自表单的必填校验负责，
        /// 不该在这里报「不能是纯数字」这种误导性的错）。
        /// </summary>
        public static bool IsAllDigits(string? v)
        {
            var s = (v ?? string.Empty).Trim();
            if (s.Length == 0) return false;
            foreach (var rune in s.EnumerateRunes())
            {
                var cat = Rune.GetUnicodeCategory(rune);
                // Nd = 十进制数字（含全角 １２３ 与阿拉伯-印度数字 ١٢٣）
                // Nl = 字母式数字（罗马数字 Ⅻ）
                // No = 其它数字（上标 ²、分数 ½）
                if (cat != UnicodeCategory.DecimalDigitNumber &&
                    cat != UnicodeCategory.LetterNumber &&
                    cat != UnicodeCategory.OtherNumber)
                    return false;
            }
            return true;
        }
    }
}
