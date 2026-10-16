using System;
using System.Collections.Generic;
using System.IO;

namespace KunFlashTool.Core.SwtEdl;

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/core/PartitionProtection.cpp`：
/// 集中式的分区保护策略。**槽位后缀会被忽略**，所以同一套策略同时覆盖 A/B 与非 A/B 机器。
/// </summary>
public static class PartitionProtection
{
    /// <summary>把分区名规范化：去目录、去最后一个扩展名、转小写、去掉末尾的 _a / _b。</summary>
    public static string NormalizedPartitionName(string? name)
    {
        // 对应 QFileInfo(name).completeBaseName()：只剥掉**最后一个**扩展名（a.tar.gz → a.tar）。
        var result = Path.GetFileNameWithoutExtension((name ?? string.Empty).Trim()).ToLowerInvariant();
        if (result.EndsWith("_a", StringComparison.Ordinal) || result.EndsWith("_b", StringComparison.Ordinal))
            result = result.Substring(0, result.Length - 2);
        return result;
    }

    /// <summary>
    /// Violet/SmartTool 的基带指纹与校准备份策略，外加它的 EDL 敏感镜像清单。
    /// </summary>
    public static IReadOnlyList<string> ProtectedPartitionNames() => new[]
    {
        "persist",
        "persistbak",
        "ocdt",
        "modemst1",
        "modemst2",
        "fsg",
        "fsc",
        "oplusdycnvbk",
        "oplusstanvbk",
        "oplusreserve4",
        "oplusreserve5",
        "param",
        "preisp_otp",
        "preisp_dt",
        "preisp_dt_bk",
    };

    private static readonly HashSet<string> NameSet =
        new(ProtectedPartitionNames(), StringComparer.Ordinal);

    public static bool IsProtectedPartitionName(string? name) =>
        NameSet.Contains(NormalizedPartitionName(name));

    public static bool IsProtectedPartitionMapping(string? label, string? filename = null) =>
        IsProtectedPartitionName(label) ||
        (!string.IsNullOrWhiteSpace(filename) && IsProtectedPartitionName(filename));
}
