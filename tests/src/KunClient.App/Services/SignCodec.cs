using System;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace KunFlashTool.App.Services
{
    /// <summary>
    /// 签名数据（token / sig）的文本 ↔ 字节转换。
    ///
    /// 设备吐出来的 token、以及用户拿到的 sig，形式很杂：可能带 `0x` 前缀、可能按空格或冒号
    /// 每两个字符分一组、也可能是 Base64。这里统一做「清洗 + 解析」，页面和弹窗共用一份，
    /// 免得两边口径不一致（一边能解析一边报格式错）。
    /// </summary>
    public static class SignCodec
    {
        /// <summary>
        /// 清洗十六进制文本：去掉 `0x` 前缀、空白、冒号、连字符、换行。
        /// 返回只含 0-9a-fA-F 的串（可能是空串）。
        /// </summary>
        public static string CleanHex(string? text)
        {
            var s = Regex.Replace(text ?? "", @"0[xX]", "");
            s = Regex.Replace(s, @"[\s:\-]", "");
            return Regex.IsMatch(s, @"^[0-9a-fA-F]*$") ? s : "";
        }

        /// <summary>
        /// 把文本解析成字节：先按十六进制（先清洗），不像 hex 再按 Base64 试。都失败返回 null。
        /// </summary>
        public static byte[]? TryParseBytes(string? text)
        {
            var raw = (text ?? "").Trim();
            if (raw.Length == 0) return null;

            var hex = CleanHex(raw);
            if (hex.Length >= 2 && hex.Length % 2 == 0)
            {
                try
                {
                    var bytes = new byte[hex.Length / 2];
                    for (var i = 0; i < bytes.Length; i++)
                        bytes[i] = byte.Parse(hex.Substring(i * 2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
                    return bytes;
                }
                catch (FormatException) { }
            }

            var b64 = Regex.Replace(raw, @"[^A-Za-z0-9+/=]", "");
            if (b64.Length >= 4)
            {
                var mod = b64.Length % 4;
                if (mod != 0) b64 = b64.Substring(0, b64.Length - mod);
                try
                {
                    var bytes = Convert.FromBase64String(b64);
                    if (bytes.Length > 0) return bytes;
                }
                catch (FormatException) { }
            }

            return null;
        }

        /// <summary>字节数组 → 小写十六进制（超过 max 字节就截断并加省略号）。</summary>
        public static string ToHex(byte[] data, int max)
        {
            var n = Math.Min(max, data.Length);
            var sb = new StringBuilder(n * 2);
            for (var i = 0; i < n; i++) sb.Append(data[i].ToString("x2"));
            return sb.ToString() + (data.Length > n ? "…" : "");
        }

        /// <summary>文本是否看起来是纯文本（hex / base64 之类），用于判断导入的文件是不是文本签名。</summary>
        public static string? TryReadAsText(string path)
        {
            try
            {
                var text = Encoding.ASCII.GetString(System.IO.File.ReadAllBytes(path));
                if (text.Length >= 8 && Regex.IsMatch(text, @"^[\x09\x0a\x0d\x20-\x7e]+$"))
                    return text.Trim();
            }
            catch { }
            return null;
        }
    }
}
