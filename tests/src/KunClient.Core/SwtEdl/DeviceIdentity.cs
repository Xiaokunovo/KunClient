using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace KunFlashTool.Core.SwtEdl;

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/core/DeviceIdentity.cpp`：
/// 从各种线索里认出高通型号（SM8250 之类），并给出带骁龙营销名的显示串。
/// </summary>
public static class DeviceIdentity
{
    /// <summary>匹配型号码的正则（与 C++ 侧同一个模式）。</summary>
    private static readonly Regex ChipPattern = new(
        @"(?:^|[^A-Z0-9])((?:SM|MSM|SDM|APQ)[\s_-]?[0-9]{4,5})(?=$|[^0-9])",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private static readonly Regex SeparatorPattern = new(@"[\s_-]", RegexOptions.Compiled);

    private static readonly Dictionary<uint, string> MsmIdNames = new()
    {
        [0x08b0e1] = "SDM845",
        [0x0a50e1] = "SM8150",
        [0x0a60e1] = "SM8150",
        [0x0c30e1] = "SM8250",
        [0x0ce0e1] = "SM8250",
        [0x1560e1] = "SM8250",
        [0x11e0e1] = "SM7250",
        [0x1430e1] = "SM7250",
        [0x12a0e1] = "SM7125",
        [0x12b0e1] = "SM7125",
        [0x1350e1] = "SM8350",
        [0x1420e1] = "SM8350",
        [0x1520e1] = "SM8350",
        [0x19e0e1] = "SM8350",
        [0x28c0e1] = "SM8550",
    };

    private static readonly Dictionary<string, string> DisplayNames = new()
    {
        ["SDM845"] = "SDM845 (Snapdragon 845)",
        ["SM7125"] = "SM7125 (Snapdragon 720G)",
        ["SM7250"] = "SM7250 (Snapdragon 765G)",
        ["SM8150"] = "SM8150 (Snapdragon 855)",
        ["SM8250"] = "SM8250 (Snapdragon 865)",
        ["SM8350"] = "SM8350 (Snapdragon 888)",
        ["SM8450"] = "SM8450 (Snapdragon 8 Gen 1)",
        ["SM8475"] = "SM8475 (Snapdragon 8+ Gen 1)",
        ["SM8550"] = "SM8550 (Snapdragon 8 Gen 2)",
        ["SM8650"] = "SM8650 (Snapdragon 8 Gen 3)",
        ["SM8750"] = "SM8750 (Snapdragon 8 Elite)",
    };

    private static string ChipCodeFromText(string? text)
    {
        if (string.IsNullOrEmpty(text)) return string.Empty;
        var match = ChipPattern.Match(text);
        if (!match.Success) return string.Empty;
        return SeparatorPattern.Replace(match.Groups[1].Value.ToUpperInvariant(), string.Empty);
    }

    private static uint NormalizedMsmId(string? text, out bool ok)
    {
        var value = (text ?? string.Empty).Trim();
        if (value.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) value = value.Substring(2);
        ok = ulong.TryParse(value, System.Globalization.NumberStyles.HexNumber,
                            System.Globalization.CultureInfo.InvariantCulture, out var parsed);
        // Sahara V1/V2 通常给 24 位 MSM_ID 前面补 00；有些设备回更宽的值，
        // 而公开的 EDL 表是按低 24 位索引的。
        return (uint)(parsed & 0x00ffffffUL);
    }

    /// <summary>从任意文本里抽出并规范化高通型号码（如 SM8250）。</summary>
    public static string NormalizeQualcommChipName(string? text) => ChipCodeFromText(text);

    /// <summary>把 Sahara 的 MSM_ID 转成已知的高通型号码。</summary>
    public static string QualcommChipFromMsmId(string? msmId)
    {
        var id = NormalizedMsmId(msmId, out var ok);
        if (!ok) return string.Empty;
        return MsmIdNames.TryGetValue(id, out var name) ? name : string.Empty;
    }

    /// <summary>已知的话补上骁龙家族名。</summary>
    public static string QualcommChipDisplayName(string? chipName)
    {
        var chip = NormalizeQualcommChipName(chipName);
        if (chip.Length == 0) return (chipName ?? string.Empty).Trim();
        return DisplayNames.TryGetValue(chip, out var display) ? display : chip;
    }

    /// <summary>
    /// 先用目录/文件名线索，再扫 programmer 里内嵌的 ASCII 元数据。
    /// <para>VIP 会话里用得上：进命令态会吃掉 programmer 的看门狗窗口，所以只能靠文件本身认型号。</para>
    /// </summary>
    public static string DetectProgrammerChip(string programmerPath, IReadOnlyList<string>? pathHints = null)
    {
        if (pathHints != null)
        {
            foreach (var hint in pathHints)
            {
                var chip = ChipCodeFromText(hint);
                if (chip.Length != 0) return chip;
            }
        }

        var pathChip = ChipCodeFromText(programmerPath);
        if (pathChip.Length != 0) return pathChip;

        byte[] bytes;
        try
        {
            using var fs = new FileStream(programmerPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            // Firehose programmer 一般几 MiB。8 MiB 覆盖它的 ELF 元数据，
            // 同时把 UI 侧这次包检查的开销封顶。
            var take = (int)Math.Min(8L * 1024 * 1024, fs.Length);
            bytes = new byte[take];
            var read = 0;
            while (read < take)
            {
                var n = fs.Read(bytes, read, take - read);
                if (n <= 0) break;
                read += n;
            }
            if (read != take) Array.Resize(ref bytes, read);
        }
        catch
        {
            return string.Empty;
        }

        var binaryText = Encoding.Latin1.GetString(bytes);

        var counts = new Dictionary<string, int>(StringComparer.Ordinal);
        var first = string.Empty;
        foreach (Match m in ChipPattern.Matches(binaryText))
        {
            var chip = SeparatorPattern.Replace(m.Groups[1].Value.ToUpperInvariant(), string.Empty);
            if (first.Length == 0) first = chip;
            counts[chip] = counts.TryGetValue(chip, out var c) ? c + 1 : 1;
        }

        // ⚠️ C++ 侧这里是「先 best=第一个，只有严格更大才替换」，所以**并列时保留先出现的那个**。
        //    这里必须保持同一语义 —— 遍历顺序变了（Dictionary 的枚举顺序与 QHash 不同）也会得出同一结果。
        var best = first;
        var bestCount = counts.TryGetValue(best, out var bc) ? bc : 0;
        foreach (var kv in counts)
        {
            if (kv.Value > bestCount)
            {
                best = kv.Key;
                bestCount = kv.Value;
            }
        }
        return best;
    }
}
