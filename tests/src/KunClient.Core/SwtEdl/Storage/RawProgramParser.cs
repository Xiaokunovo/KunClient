using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Xml;

namespace KunFlashTool.Core.SwtEdl;

public sealed class RawProgramEntry
{
    public string XmlPath { get; set; } = "";
    public string Filename { get; set; } = "";
    public string SourcePath { get; set; } = "";
    public string Label { get; set; } = "";
    public string StartSectorExpression { get; set; } = "";
    public uint Lun { get; set; }
    public uint SectorSize { get; set; }
    public ulong StartSector { get; set; }
    public ulong SectorCount { get; set; }
    public ulong FileSectorOffset { get; set; }
    public bool Sparse { get; set; }
    public bool StartSectorResolved { get; set; } = true;
    public bool DynamicSuper { get; set; }
}

public sealed class RawProgramPlan
{
    public List<string> XmlPaths { get; } = new();
    public List<RawProgramEntry> Entries { get; } = new();
    public List<string> Warnings { get; } = new();
    public ulong TotalSourceBytes { get; set; }
    public int SuperEntryCount { get; set; }
    public int DynamicSuperCount { get; set; }

    public bool HasSplitSuper() => SuperEntryCount > 1 || DynamicSuperCount > 0;
}

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/storage/RawProgramParser.cpp`：把
/// <c>rawprogram*.xml</c>（+ 同目录镜像文件）解析成一份可执行的刷写计划。
/// </summary>
public static class RawProgramParser
{
    private static bool ParseUnsigned(string? value, out ulong result)
    {
        result = 0;
        var text = (value ?? string.Empty).Trim();
        while (text.EndsWith(".", StringComparison.Ordinal)) text = text.Substring(0, text.Length - 1);
        var radix = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? 16 : 10;
        var digits = radix == 16 ? text.Substring(2) : text;
        return ulong.TryParse(digits, radix == 16 ? NumberStyles.HexNumber : NumberStyles.Integer,
                              CultureInfo.InvariantCulture, out result);
    }

    private static bool ParseU32(string? value, out uint result)
    {
        result = 0;
        if (!ParseUnsigned(value, out var parsed) || parsed > uint.MaxValue) return false;
        result = (uint)parsed;
        return true;
    }

    private static bool ParseBoolean(string? value) =>
        string.Equals(value, "true", StringComparison.OrdinalIgnoreCase) ||
        value == "1" ||
        string.Equals(value, "yes", StringComparison.OrdinalIgnoreCase);

    private static string ResolveImage(string xmlPath, string filename)
    {
        var normalized = (filename ?? string.Empty).Trim().Replace('\\', '/');
        if (normalized.Length == 0) return string.Empty;

        if (IsAbsolute(normalized) && File.Exists(normalized)) return Canonical(normalized);

        // 严格 rawprogram 语义：只认 filename 里写的**那一条**路径。
        // 加了前缀/装饰、项目专用的"看着像"的名字一律不认，交给调用方报"缺失"。
        var xmlDirectory = Path.GetDirectoryName(Path.GetFullPath(xmlPath)) ?? string.Empty;
        var direct = CleanPath(CombineSlash(xmlDirectory, normalized));
        if (File.Exists(direct)) return Canonical(direct);
        return direct;
    }

    private static bool IsAbsolute(string path)
    {
        if (path.StartsWith("/", StringComparison.Ordinal)) return true;
        return Path.IsPathFullyQualified(path);
    }

    private static string CombineSlash(string directory, string relative)
    {
        if (string.IsNullOrEmpty(directory)) return relative;
        return directory.Replace('\\', '/') + "/" + relative;
    }

    private static string CleanPath(string path)
    {
        try { return Path.GetFullPath(path.Replace('/', Path.DirectorySeparatorChar)); }
        catch { return path; }
    }

    private static string Canonical(string path)
    {
        try { return Path.GetFullPath(path); }
        catch { return path; }
    }

    public static bool Parse(IReadOnlyList<string> xmlPaths, RawProgramPlan plan, out string error)
    {
        error = string.Empty;
        if (plan is null)
        {
            error = "XML 刷机计划输出参数为空";
            return false;
        }
        if (xmlPaths.Count == 0)
        {
            error = "尚未选择 rawprogram XML";
            return false;
        }

        var seenXml = new HashSet<string>(StringComparer.Ordinal);
        foreach (var requestedPath in xmlPaths)
        {
            if (!File.Exists(requestedPath))
            {
                error = $"XML 文件不存在：{requestedPath}";
                return false;
            }
            var xmlPath = Canonical(requestedPath);
            if (!seenXml.Add(xmlPath.ToLowerInvariant())) continue;
            plan.XmlPaths.Add(xmlPath);

            var fileName = Path.GetFileName(xmlPath);
            var settings = new XmlReaderSettings
            {
                IgnoreComments = true,
                IgnoreWhitespace = true,
                IgnoreProcessingInstructions = true,
                DtdProcessing = DtdProcessing.Ignore,
            };

            try
            {
                using var stream = File.OpenRead(xmlPath);
                using var reader = XmlReader.Create(stream, settings);
                var lineInfo = reader as IXmlLineInfo;

                while (reader.Read())
                {
                    if (reader.NodeType != XmlNodeType.Element ||
                        !string.Equals(reader.Name, "program", StringComparison.OrdinalIgnoreCase))
                    {
                        continue;
                    }

                    var entry = new RawProgramEntry
                    {
                        XmlPath = xmlPath,
                        Filename = Attribute(reader, "filename"),
                        Label = Attribute(reader, "label"),
                        StartSectorExpression = Attribute(reader, "start_sector"),
                        Sparse = ParseBoolean(Attribute(reader, "sparse")),
                    };
                    if (entry.Filename.Length == 0) continue;

                    if (!ParseU32(Attribute(reader, "SECTOR_SIZE_IN_BYTES"), out var sectorSize) ||
                        sectorSize == 0)
                    {
                        error = $"{fileName}：program 的 SECTOR_SIZE_IN_BYTES 错误";
                        return false;
                    }
                    entry.SectorSize = sectorSize;
                    if (!ParseU32(Attribute(reader, "physical_partition_number"), out var lun))
                    {
                        error = $"{fileName}：program 的 physical_partition_number 错误";
                        return false;
                    }
                    entry.Lun = lun;
                    if (!ParseUnsigned(Attribute(reader, "num_partition_sectors"), out var sectorCount))
                    {
                        error = $"{fileName}：program 的 num_partition_sectors 错误";
                        return false;
                    }
                    entry.SectorCount = sectorCount;

                    entry.SourcePath = ResolveImage(xmlPath, entry.Filename);
                    if (entry.SectorCount == 0 && File.Exists(entry.SourcePath))
                    {
                        try
                        {
                            using var source = File.Open(entry.SourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                            if (entry.Sparse || SparseImage.HasSparseMagic(source))
                            {
                                if (!SparseImage.Inspect(source, out var sparseInfo, out var sparseError))
                                {
                                    error = $"{entry.Filename}：零扇区 sparse 镜像解析失败：{sparseError}";
                                    return false;
                                }
                                var expandedBytes = sparseInfo.ExpandedSize;
                                entry.SectorCount = expandedBytes / entry.SectorSize +
                                                    (expandedBytes % entry.SectorSize == 0 ? 0UL : 1UL);
                                entry.Sparse = true;
                                plan.Warnings.Add(
                                    $"零扇区 sparse 条目已按镜像头解析：" +
                                    $"{(entry.Label.Length == 0 ? entry.Filename : entry.Label)}，" +
                                    $"展开 {expandedBytes} 字节 / {entry.SectorCount} 扇区");
                            }
                        }
                        catch (IOException)
                        {
                            // 打不开就当普通文件继续；下面按 size 走
                        }
                    }

                    if (entry.SectorCount == 0)
                    {
                        plan.Warnings.Add($"跳过零扇区条目：{(entry.Label.Length == 0 ? entry.Filename : entry.Label)}");
                        continue;
                    }

                    var fileOffset = Attribute(reader, "file_sector_offset");
                    if (fileOffset.Length != 0)
                    {
                        if (!ParseUnsigned(fileOffset, out var parsedOffset))
                        {
                            error = $"{fileName}：program 的 file_sector_offset 错误";
                            return false;
                        }
                        entry.FileSectorOffset = parsedOffset;
                    }

                    if (!ParseUnsigned(entry.StartSectorExpression, out var startSector))
                    {
                        entry.StartSectorResolved = false;
                        plan.Warnings.Add(
                            $"动态起始扇区将在设备端解析：" +
                            $"{(entry.Label.Length == 0 ? entry.Filename : entry.Label)} ({entry.StartSectorExpression})");
                    }
                    else
                    {
                        entry.StartSector = startSector;
                    }

                    var sourceExists = File.Exists(entry.SourcePath);
                    if (!sourceExists)
                    {
                        var superEntry =
                            string.Equals(entry.Label, "super", StringComparison.OrdinalIgnoreCase) ||
                            string.Equals(Path.GetFileName(entry.Filename), "super.img", StringComparison.OrdinalIgnoreCase);

                        var dynamicPlan = new DynamicSuperPlan();
                        if (superEntry &&
                            DynamicSuperPlanner.Discover(xmlPath, out var definitionPath, out _) &&
                            DynamicSuperPlanner.Create(xmlPath, definitionPath, dynamicPlan, out _))
                        {
                            entry.DynamicSuper = true;
                            entry.SourcePath = definitionPath;
                            plan.TotalSourceBytes = SaturatingAdd(plan.TotalSourceBytes, dynamicPlan.TotalWriteBytes);
                            plan.SuperEntryCount++;
                            plan.DynamicSuperCount++;
                            plan.Warnings.Add(
                                $"super.img 为空：已启用 META 免合并计划（{dynamicPlan.Images.Count} 个动态分区）");
                            plan.Entries.Add(entry);
                            continue;
                        }

                        plan.Warnings.Add(
                            $"跳过缺失镜像：{(entry.Label.Length == 0 ? entry.Filename : entry.Label)} → {entry.Filename}");
                        continue;
                    }

                    if (string.Equals(entry.Label, "ocdt", StringComparison.OrdinalIgnoreCase))
                    {
                        var size = new FileInfo(entry.SourcePath).Length;
                        byte[] magic;
                        try
                        {
                            using var ocdt = File.Open(entry.SourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                            var buffer = new byte[4];
                            var read = ocdt.Read(buffer, 0, 4);
                            magic = read == 4 ? buffer : Array.Empty<byte>();
                        }
                        catch
                        {
                            magic = Array.Empty<byte>();
                        }
                        if (size < 68 || !magic.AsSpan().SequenceEqual(Encoding.ASCII.GetBytes("TDCO")))
                        {
                            plan.Warnings.Add(
                                $"跳过无效 OCDT 镜像：{Path.GetFileName(entry.SourcePath)}" +
                                "（要求 TDCO 文件头且长度至少 68 字节）");
                            continue;
                        }
                    }

                    if (entry.FileSectorOffset > ulong.MaxValue / entry.SectorSize)
                    {
                        error = $"镜像偏移数值溢出：{entry.Filename}";
                        return false;
                    }
                    var offsetBytes = entry.FileSectorOffset * entry.SectorSize;
                    var fileBytes = (ulong)new FileInfo(entry.SourcePath).Length;
                    if (offsetBytes >= fileBytes)
                    {
                        error = $"镜像偏移超出文件：{entry.Filename}";
                        return false;
                    }
                    var capacity = entry.SectorCount > ulong.MaxValue / entry.SectorSize
                        ? ulong.MaxValue
                        : entry.SectorCount * entry.SectorSize;
                    var sourceBytes = Math.Min(fileBytes - offsetBytes, capacity);
                    plan.TotalSourceBytes = SaturatingAdd(plan.TotalSourceBytes, sourceBytes);

                    var superProbe = entry.Label + " " + entry.Filename;
                    if (superProbe.Contains("super", StringComparison.OrdinalIgnoreCase))
                    {
                        plan.SuperEntryCount++;
                    }

                    plan.Entries.Add(entry);
                }
            }
            catch (XmlException ex)
            {
                error = $"XML 解析失败：{ex.Message}（第 {ex.LineNumber} 行，第 {ex.LinePosition} 列）";
                return false;
            }
            catch (IOException ex)
            {
                error = $"打开 XML 失败：{ex.Message}";
                return false;
            }
        }

        if (plan.Entries.Count == 0)
        {
            error = "所选 XML 中没有可刷写的 program 条目";
            return false;
        }
        return true;
    }

    private static ulong SaturatingAdd(ulong a, ulong b) =>
        b > ulong.MaxValue - a ? ulong.MaxValue : a + b;

    private static string Attribute(XmlReader reader, string name) =>
        (reader.GetAttribute(name) ?? string.Empty).Trim();
}
