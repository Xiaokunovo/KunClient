using System.Text;

namespace KunFlashTool.Core.FileSystems;

/// <summary>
/// 从分区（物理分区或 super 内逻辑分区）读取并解析 build.prop 属性。
/// 按 my_manifest → odm → my_product → vendor → product → system_ext → system 的
/// 优先级合并（先到先得），移植自 VioletToolBox EdlVersionInfo/EdlBuildPropReader.cs。
/// </summary>
public sealed class BuildPropReader
{
    private static readonly string[] BuildPropPaths =
    {
        "build.prop",
        "etc/build.prop",
        "system/build.prop",
        "system/etc/build.prop"
    };

    private static readonly string[] PartitionPriority =
    {
        "my_manifest", "odm", "my_product", "vendor",
        "product", "system_ext", "system"
    };

    /// <summary>从 super 块设备读取全部版本分区的 build.prop。</summary>
    /// <param name="superReader">覆盖整个 super 分区的 IByteReader</param>
    /// <param name="logicalPartitions">各逻辑分区的 extent 映射（512B 逻辑扇区），
    /// 键为元数据里的分区名（含 _a/_b 槽位后缀）。</param>
    public BuildPropResult ReadFromSuper(
        IByteReader superReader,
        IReadOnlyDictionary<string, IReadOnlyList<LpExtentMapping>> logicalPartitions,
        Action<string>? onStatus = null)
    {
        var result = new BuildPropResult();

        var versionPartitions = logicalPartitions.Keys
            .Where(IsVersionPartition)
            .OrderBy(GetPartitionPriority)
            .ThenBy(n => n, StringComparer.OrdinalIgnoreCase)
            .ToList();

        foreach (var partitionName in versionPartitions)
        {
            try
            {
                string baseName = NormalizePartitionName(partitionName);
                if (result.PartitionProps.ContainsKey(baseName))
                    continue;

                onStatus?.Invoke($"解析 {partitionName} 版本信息...");
                var reader = new LogicalPartitionReader(superReader, logicalPartitions[partitionName], 512);
                var properties = ReadBuildProp(reader, result);
                if (properties == null || properties.Count == 0)
                    continue;

                result.PartitionProps[baseName] = properties;
                MergeProperties(result.Properties, properties);
            }
            catch (Exception)
            {
                // 单个分区解析失败不阻断：build.prop 本来就是尽力而为的汇总
            }
        }

        return result;
    }

    /// <summary>把一个物理分区的 build.prop 合并进结果（按槽位后缀归一化名称）。</summary>
    public void ReadFromPhysical(
        BuildPropResult result,
        string partitionName,
        IByteReader reader,
        Action<string>? onStatus = null)
    {
        string baseName = NormalizePartitionName(partitionName);
        if (result.PartitionProps.ContainsKey(baseName))
            return;

        onStatus?.Invoke($"解析 {partitionName} 版本信息...");
        var properties = ReadBuildProp(reader, result);
        if (properties == null || properties.Count == 0)
            return;

        result.PartitionProps[baseName] = properties;
        MergeProperties(result.Properties, properties);
    }

    private static Dictionary<string, string>? ReadBuildProp(IByteReader reader, BuildPropResult result)
    {
        byte[]? data = null;
        if (Ext4Reader.Detect(reader))
        {
            result.FileSystems.Add("EXT4");
            var ext4 = new Ext4Reader(reader);
            if (!ext4.IsValid)
                return null;
            foreach (string path in BuildPropPaths)
            {
                data = ext4.ReadFile(path);
                if (data is { Length: > 0 })
                    break;
            }
        }
        else if (ErofsReader.Detect(reader))
        {
            result.FileSystems.Add("EROFS");
            var erofs = new ErofsReader(reader);
            if (!erofs.IsValid)
                return null;
            foreach (string path in BuildPropPaths)
            {
                data = erofs.ReadFile(path);
                if (data is { Length: > 0 })
                    break;
            }
        }

        return data is { Length: > 0 } ? ParseProperties(data) : null;
    }

    private static Dictionary<string, string> ParseProperties(byte[] data)
    {
        var result = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (string rawLine in Encoding.UTF8.GetString(data).Split('\n'))
        {
            string line = rawLine.Trim();
            if (line.Length == 0 || line.StartsWith('#'))
                continue;
            int separator = line.IndexOf('=');
            if (separator <= 0)
                continue;
            result.TryAdd(line[..separator].Trim(), line[(separator + 1)..].Trim());
        }
        return result;
    }

    private static void MergeProperties(
        Dictionary<string, string> target,
        Dictionary<string, string> source)
    {
        foreach ((string key, string value) in source)
            target.TryAdd(key, value);
    }

    private static int GetPartitionPriority(string name)
    {
        string baseName = NormalizePartitionName(name);
        int index = Array.FindIndex(
            PartitionPriority,
            item => item.Equals(baseName, StringComparison.OrdinalIgnoreCase));
        return index >= 0 ? index : PartitionPriority.Length;
    }

    private static bool IsVersionPartition(string name)
    {
        string baseName = NormalizePartitionName(name);
        return PartitionPriority.Any(
            item => item.Equals(baseName, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>"vendor_a"/"vendor_b" → "vendor"。</summary>
    public static string NormalizePartitionName(string name)
    {
        if (name.EndsWith("_a", StringComparison.OrdinalIgnoreCase) ||
            name.EndsWith("_b", StringComparison.OrdinalIgnoreCase))
            return name[..^2];
        return name;
    }
}

public sealed class BuildPropResult
{
    public Dictionary<string, Dictionary<string, string>> PartitionProps { get; } =
        new(StringComparer.OrdinalIgnoreCase);

    /// <summary>按优先级合并后的全量属性（先到先得）。</summary>
    public Dictionary<string, string> Properties { get; } =
        new(StringComparer.OrdinalIgnoreCase);

    /// <summary>实际遇到并解析成功的文件系统类型（EXT4 / EROFS）。</summary>
    public HashSet<string> FileSystems { get; } =
        new(StringComparer.OrdinalIgnoreCase);

    public string? Get(params string[] keys)
    {
        foreach (string key in keys)
        {
            if (Properties.TryGetValue(key, out string? value) &&
                !string.IsNullOrWhiteSpace(value))
                return value;
        }
        return null;
    }
}
