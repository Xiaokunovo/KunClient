using System;
using System.Text;

namespace KunFlashTool.Core.Services
{
    /// <summary>
    /// `fastboot devices` / `adb devices` 回包解析的**唯一出口**（2026-09-29）。
    ///
    /// 🔴 存在的理由是一个**编译期看不出来**的坑：
    ///   写 <c>line.Split('\t', ' ', StringSplitOptions.RemoveEmptyEntries)</c>，
    ///   本意是「按 Tab 或空格拆」，C# 却把它绑到
    ///   <c>Split(char separator, int count, StringSplitOptions options)</c> ——
    ///   第二个参数那个 <c>' '</c> 被**隐式转成 int 32**，语义变成
    ///   「**只在 \t 上拆、最多 32 段**」。于是**空格分隔**的回包永远只拆出 1 段，
    ///   `parts.Length &gt;= 2` 不成立，设备明明在列表里却被判成「未检测到」。
    ///   0 错 0 警、肉眼完全看不出来 —— 必须靠单测钉住。
    ///   （实机现场：日志打着 `a1940a31    fastboot`，下一行却报「未检测到 Fastboot 设备」。）
    ///
    /// 正确写法是分隔符传 <c>null</c>：按**任意空白**（空格 / Tab / 换行）拆。
    ///
    /// 顺带把回包转成**可见转义**给日志用：排查「解析不出来」时，一眼就能分清
    /// 是**分隔符形态**不对（`\t` 还是空格）还是**设备真没回**。
    /// </summary>
    public static class FastbootOutput
    {
        /// <summary>
        /// 取第一台设备的序列号。取不到返回空串。
        ///
        /// 只认第二段恰好是 <c>fastboot</c> 的行 —— 这样能自动跳过
        /// <c>&lt; waiting for any device &gt;</c> 之类的噪声，
        /// 也不会把 `adb devices` 那种带状态的输出误当成 fastboot 设备。
        /// </summary>
        public static string FirstFastbootSerial(string? output)
        {
            foreach (var raw in (output ?? string.Empty).Split('\n', StringSplitOptions.RemoveEmptyEntries))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("<", StringComparison.Ordinal)) continue;

                // ⚠️ 分隔符必须是 null（= 任意空白），别写成两个 char + StringSplitOptions（见类型注释）
                var parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length >= 2 && parts[1].Equals("fastboot", StringComparison.OrdinalIgnoreCase))
                    return parts[0];
            }
            return string.Empty;
        }

        /// <summary>
        /// 把空白转成可见转义（`\t` `\r` `\n`），专供日志 —— 空回包给「（无输出）」。
        /// </summary>
        public static string Escaped(string? text)
        {
            if (string.IsNullOrEmpty(text)) return "（无输出）";

            var sb = new StringBuilder(text.Length + 8);
            foreach (var ch in text)
            {
                switch (ch)
                {
                    case '\t': sb.Append("\\t"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\n': sb.Append("\\n"); break;
                    default: sb.Append(ch); break;
                }
            }
            return sb.ToString();
        }
    }
}
