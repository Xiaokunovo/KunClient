using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace KunFlashTool.Core.SwtEdl;

public sealed class DynamicSuperExtent
{
    public ulong LogicalOffset { get; set; }
    public ulong PhysicalOffset { get; set; }
    public ulong ByteCount { get; set; }
}

public sealed class DynamicSuperImage
{
    public string PartitionName { get; set; } = "";
    public string SourcePath { get; set; } = "";
    public ulong ExpandedSize { get; set; }
    public ulong AllocatedSize { get; set; }
    public bool Sparse { get; set; }
    public List<DynamicSuperExtent> Extents { get; } = new();
}

public sealed class DynamicSuperPlan
{
    public string DefinitionPath { get; set; } = "";
    public string MetadataPath { get; set; } = "";
    public string MetadataFilename { get; set; } = "";
    public byte[] MetadataImage { get; set; } = Array.Empty<byte>();
    public ulong BlockDeviceSize { get; set; }
    public uint MetadataSlotCount { get; set; }
    public ushort MetadataMajorVersion { get; set; }
    public ushort MetadataMinorVersion { get; set; }
    public List<DynamicSuperImage> Images { get; } = new();

    /// <summary>对应 C++ 的 <c>DynamicSuperPlan::totalWriteBytes()</c>（饱和加法，防溢出回绕）。</summary>
    public ulong TotalWriteBytes
    {
        get
        {
            var total = (ulong)MetadataImage.Length;
            foreach (var image in Images)
            {
                if (image.AllocatedSize > ulong.MaxValue - total) return ulong.MaxValue;
                total += image.AllocatedSize;
            }
            return total;
        }
    }
}

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/storage/DynamicSuper.cpp`：
/// 解析 OPPO/高通 dynamic partitions 包里的 <c>META/super_def*.json</c> + <c>super_meta</c>，
/// 生成一份「不合并 super.img、直接按 extent 写各动态分区」的免合并计划。
/// </summary>
public static class DynamicSuperPlanner
{
    private const uint GeometryMagic = 0x616c4467;
    private const uint MetadataMagic = 0x414c5030;
    private const ulong LpSectorSize = 512;

    private sealed class LpExtent
    {
        public ulong Sectors;
        public uint Type;
        public ulong TargetData;
        public uint TargetSource;
    }

    private sealed class LpPartition
    {
        public string Name = "";
        public uint FirstExtent;
        public uint ExtentCount;
    }

    private sealed class VendorMetadata
    {
        public uint MetadataMaxSize;
        public uint MetadataSlotCount;
        public uint LogicalBlockSize;
        public ushort MajorVersion;
        public ushort MinorVersion;
        public ulong BlockDeviceSize;
        public byte[] Geometry = Array.Empty<byte>();
        public byte[] MetadataBlob = Array.Empty<byte>();
        public List<LpExtent> Extents = new();
        public Dictionary<string, LpPartition> Partitions = new(StringComparer.Ordinal);
    }

    private static uint U32(byte[] b, long o) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan((int)o));
    private static ushort U16(byte[] b, long o) => BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan((int)o));
    private static ulong U64(byte[] b, long o) => BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan((int)o));

    private static string FixedString(byte[] bytes, long offset, long length)
    {
        if (offset < 0 || length < 0 || offset + length > bytes.Length) return string.Empty;
        var slice = bytes.AsSpan((int)offset, (int)length);
        var zero = slice.IndexOf((byte)0);
        if (zero >= 0) slice = slice.Slice(0, zero);
        return Encoding.Latin1.GetString(slice).Trim();
    }

    private static bool JsonUnsigned(JsonElement value, out ulong result)
    {
        result = 0;
        bool ok;
        ulong parsed = 0;
        if (value.ValueKind == JsonValueKind.String)
        {
            var text = (value.GetString() ?? string.Empty).Trim();
            var radix = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? 16 : 10;
            var digits = radix == 16 ? text.Substring(2) : text;
            ok = ulong.TryParse(digits, radix == 16 ? NumberStyles.HexNumber : NumberStyles.Integer,
                                CultureInfo.InvariantCulture, out parsed);
        }
        else if (value.ValueKind == JsonValueKind.Number && value.TryGetDouble(out var d) && d >= 0.0)
        {
            parsed = (ulong)d;
            ok = (double)parsed == d;
        }
        else
        {
            ok = false;
        }

        if (ok) result = parsed;
        return ok;
    }

    private static string PackageRootFor(string programXmlPath, string? definitionPath = null)
    {
        definitionPath ??= string.Empty;
        if (definitionPath.Length != 0)
        {
            var definition = Path.GetFullPath(definitionPath);
            var definitionDir = Path.GetDirectoryName(definition) ?? string.Empty;
            if (string.Equals(Path.GetFileName(definitionDir), "META", StringComparison.OrdinalIgnoreCase))
            {
                return Path.GetDirectoryName(definitionDir) ?? string.Empty;
            }
        }

        var directory = Path.GetDirectoryName(Path.GetFullPath(programXmlPath)) ?? string.Empty;
        var name = Path.GetFileName(directory);
        if (string.Equals(name, "IMAGES", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(name, "META", StringComparison.OrdinalIgnoreCase))
        {
            directory = Path.GetDirectoryName(directory) ?? string.Empty;
        }
        return directory;
    }

    private static string ResolvePackageFile(string root, string definitionPath, string relativePath)
    {
        var normalized = (relativePath ?? string.Empty).Trim().Replace('\\', '/');
        var definitionDir = Path.GetDirectoryName(Path.GetFullPath(definitionPath)) ?? string.Empty;

        string[] candidates =
        {
            normalized,
            Path.Combine(root, normalized.Replace('/', Path.DirectorySeparatorChar)),
            Path.Combine(definitionDir, normalized.Replace('/', Path.DirectorySeparatorChar)),
            Path.Combine(root, "IMAGES", Path.GetFileName(normalized)),
            Path.Combine(definitionDir, Path.GetFileName(normalized)),
        };

        foreach (var candidate in candidates)
        {
            try
            {
                if (File.Exists(candidate)) return Path.GetFullPath(candidate);
            }
            catch
            {
                // 非法路径直接跳过（Qt 那边 QFileInfo::isFile 同样只是返回 false）
            }
        }
        return string.Empty;
    }

    private static bool VerifySha256Field(byte[] bytes, long dataOffset, long dataSize, long checksumOffset)
    {
        if (dataOffset < 0 || dataSize <= 0 || dataOffset + dataSize > bytes.Length ||
            checksumOffset < dataOffset || checksumOffset + 32 > dataOffset + dataSize)
        {
            return false;
        }

        var checkedBlock = bytes.AsSpan((int)dataOffset, (int)dataSize).ToArray();
        var expected = checkedBlock.AsSpan((int)(checksumOffset - dataOffset), 32).ToArray();
        checkedBlock.AsSpan((int)(checksumOffset - dataOffset), 32).Clear();
        return SHA256.HashData(checkedBlock).AsSpan().SequenceEqual(expected);
    }

    private static bool ParseVendorMetadata(string path, out VendorMetadata metadata, out string error)
    {
        metadata = new VendorMetadata();
        error = string.Empty;

        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(path);
        }
        catch (Exception ex)
        {
            error = $"打开 super_meta 失败：{ex.Message}";
            return false;
        }

        if (bytes.Length < 4096 + 128)
        {
            error = "super_meta 文件过小";
            return false;
        }

        var geometryMagic = U32(bytes, 0);
        var geometrySize = U32(bytes, 4);
        if (geometryMagic != GeometryMagic || geometrySize < 52 || geometrySize > 4096 ||
            !VerifySha256Field(bytes, 0, geometrySize, 8))
        {
            error = "super_meta geometry 校验失败";
            return false;
        }

        metadata.MetadataMaxSize = U32(bytes, 40);
        metadata.MetadataSlotCount = U32(bytes, 44);
        metadata.LogicalBlockSize = U32(bytes, 48);
        if (metadata.MetadataMaxSize < 128 || metadata.MetadataMaxSize % 512 != 0 ||
            metadata.MetadataSlotCount == 0 || metadata.MetadataSlotCount > 16 ||
            metadata.LogicalBlockSize == 0)
        {
            error = "super_meta geometry 参数错误";
            return false;
        }

        const long headerOffset = 4096;
        var magic = U32(bytes, headerOffset);
        metadata.MajorVersion = U16(bytes, headerOffset + 4);
        metadata.MinorVersion = U16(bytes, headerOffset + 6);
        var headerSize = U32(bytes, headerOffset + 8);
        var tablesSize = U32(bytes, headerOffset + 44);
        if (magic != MetadataMagic || metadata.MajorVersion != 10 ||
            (headerSize != 128 && headerSize != 256) ||
            headerOffset + headerSize + tablesSize > bytes.Length ||
            !VerifySha256Field(bytes, headerOffset, headerSize, headerOffset + 12))
        {
            error = "super_meta metadata header 校验失败";
            return false;
        }

        var expectedTablesHash = bytes.AsSpan((int)(headerOffset + 48), 32).ToArray();
        var tables = bytes.AsSpan((int)(headerOffset + headerSize), (int)tablesSize).ToArray();
        if (!SHA256.HashData(tables).AsSpan().SequenceEqual(expectedTablesHash))
        {
            error = "super_meta tables 校验失败";
            return false;
        }

        var partitionsOffset = U32(bytes, headerOffset + 80);
        var partitionsCount = U32(bytes, headerOffset + 84);
        var partitionEntrySize = U32(bytes, headerOffset + 88);
        var extentsOffset = U32(bytes, headerOffset + 92);
        var extentsCount = U32(bytes, headerOffset + 96);
        var extentEntrySize = U32(bytes, headerOffset + 100);
        var blockDevicesOffset = U32(bytes, headerOffset + 116);
        var blockDevicesCount = U32(bytes, headerOffset + 120);
        var blockDeviceEntrySize = U32(bytes, headerOffset + 124);
        if (partitionEntrySize < 52 || extentEntrySize < 24 || blockDeviceEntrySize < 64 ||
            blockDevicesCount != 1 ||
            (ulong)partitionsOffset + (ulong)partitionsCount * partitionEntrySize > tablesSize ||
            (ulong)extentsOffset + (ulong)extentsCount * extentEntrySize > tablesSize ||
            (ulong)blockDevicesOffset + blockDeviceEntrySize > tablesSize)
        {
            error = "super_meta 表描述符越界";
            return false;
        }

        var tablesBase = headerOffset + headerSize;
        for (uint index = 0; index < partitionsCount; ++index)
        {
            var offset = tablesBase + partitionsOffset + index * (long)partitionEntrySize;
            var partition = new LpPartition
            {
                Name = FixedString(bytes, offset, 36),
                FirstExtent = U32(bytes, offset + 40),
                ExtentCount = U32(bytes, offset + 44),
            };
            if (partition.Name.Length == 0 ||
                (ulong)partition.FirstExtent + partition.ExtentCount > extentsCount)
            {
                error = "super_meta 分区条目错误";
                return false;
            }
            metadata.Partitions[partition.Name.ToLowerInvariant()] = partition;
        }

        for (uint index = 0; index < extentsCount; ++index)
        {
            var offset = tablesBase + extentsOffset + index * (long)extentEntrySize;
            metadata.Extents.Add(new LpExtent
            {
                Sectors = U64(bytes, offset),
                Type = U32(bytes, offset + 8),
                TargetData = U64(bytes, offset + 12),
                TargetSource = U32(bytes, offset + 20),
            });
        }

        var blockOffset = tablesBase + blockDevicesOffset;
        metadata.BlockDeviceSize = U64(bytes, blockOffset + 16);

        metadata.Geometry = bytes.AsSpan(0, 4096).ToArray();
        metadata.MetadataBlob = bytes.AsSpan((int)headerOffset, (int)(headerSize + tablesSize)).ToArray();
        return true;
    }

    private static byte[] BuildMetadataImage(VendorMetadata metadata)
    {
        var result = new List<byte>(4096 + metadata.Geometry.Length * 2);
        result.AddRange(new byte[4096]);
        result.AddRange(metadata.Geometry);
        result.AddRange(metadata.Geometry);

        var padded = new byte[metadata.MetadataMaxSize];
        Array.Copy(metadata.MetadataBlob, padded,
                   Math.Min(padded.Length, metadata.MetadataBlob.Length));
        for (uint index = 0; index < metadata.MetadataSlotCount * 2U; ++index)
        {
            result.AddRange(padded);
        }
        return result.ToArray();
    }

    /// <summary>在包目录里定位 <c>META/super_def*.json</c>（优先按 <c>IMAGES/super_meta.raw</c> 指针里的 nv id 精确匹配）。</summary>
    public static bool Discover(string programXmlPath, out string definitionPath, out string error)
    {
        definitionPath = string.Empty;
        error = string.Empty;

        var root = PackageRootFor(programXmlPath);
        var nvId = string.Empty;
        var pointerPath = Path.Combine(root, "IMAGES", "super_meta.raw");
        if (File.Exists(pointerPath))
        {
            try
            {
                var name = Encoding.UTF8.GetString(File.ReadAllBytes(pointerPath)).Trim();
                const string prefix = "super_meta.";
                const string suffix = ".raw";
                if (name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) &&
                    name.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
                {
                    nvId = name.Substring(prefix.Length, name.Length - prefix.Length - suffix.Length);
                }
            }
            catch (IOException)
            {
                // 指针读不到就当没有 nv id，走下面的通配匹配
            }
        }

        var metaDirectory = Path.Combine(root, "META");
        if (nvId.Length != 0)
        {
            var exact = Path.Combine(metaDirectory, $"super_def.{nvId}.json");
            if (File.Exists(exact))
            {
                definitionPath = Path.GetFullPath(exact);
                return true;
            }
        }

        if (Directory.Exists(metaDirectory))
        {
            var definitions = Directory.GetFiles(metaDirectory, "super_def*.json")
                                       .OrderBy(p => p, StringComparer.Ordinal)
                                       .ToList();
            if (definitions.Count > 0)
            {
                definitionPath = Path.GetFullPath(definitions[0]);
                return true;
            }
        }

        error = "未找到 META/super_def*.json";
        return false;
    }

    public static bool Create(string programXmlPath, string definitionPath, DynamicSuperPlan plan, out string error)
    {
        error = string.Empty;
        if (plan is null)
        {
            error = "super 计划输出参数为空";
            return false;
        }

        byte[] definitionBytes;
        try
        {
            definitionBytes = File.ReadAllBytes(definitionPath);
        }
        catch (Exception ex)
        {
            error = $"打开 super_def 失败：{ex.Message}";
            return false;
        }

        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(definitionBytes);
        }
        catch (JsonException ex)
        {
            error = $"解析 super_def 失败：{ex.Message}";
            return false;
        }

        using (document)
        {
            var rootObject = document.RootElement;
            if (rootObject.ValueKind != JsonValueKind.Object)
            {
                error = "解析 super_def 失败：根不是 JSON 对象";
                return false;
            }

            var packageRoot = PackageRootFor(programXmlPath, definitionPath);
            var superMeta = rootObject.TryGetProperty("super_meta", out var sm) && sm.ValueKind == JsonValueKind.Object
                ? sm
                : default;
            var metadataRelative = superMeta.ValueKind == JsonValueKind.Object && superMeta.TryGetProperty("path", out var p)
                ? p.GetString() ?? string.Empty
                : string.Empty;
            var metadataPath = ResolvePackageFile(packageRoot, definitionPath, metadataRelative);
            if (metadataPath.Length == 0)
            {
                error = "super_def 引用的 super_meta 文件不存在";
                return false;
            }

            if (!ParseVendorMetadata(metadataPath, out var vendor, out error)) return false;

            var blockDevices = rootObject.TryGetProperty("block_devices", out var bd) && bd.ValueKind == JsonValueKind.Array
                ? bd
                : default;
            ulong definedBlockSize = 0;
            if (blockDevices.ValueKind != JsonValueKind.Array || blockDevices.GetArrayLength() != 1 ||
                !JsonUnsigned(blockDevices[0].TryGetProperty("size", out var sizeEl) ? sizeEl : default, out definedBlockSize) ||
                definedBlockSize == 0 ||
                vendor.BlockDeviceSize != definedBlockSize)
            {
                error = "super_def 与 super_meta 的块设备容量不一致";
                return false;
            }

            plan.DefinitionPath = Path.GetFullPath(definitionPath);
            plan.MetadataPath = metadataPath;
            plan.MetadataFilename = Path.GetFileName(metadataPath);
            plan.MetadataImage = BuildMetadataImage(vendor);
            plan.BlockDeviceSize = definedBlockSize;
            plan.MetadataSlotCount = vendor.MetadataSlotCount;
            plan.MetadataMajorVersion = vendor.MajorVersion;
            plan.MetadataMinorVersion = vendor.MinorVersion;

            if (!rootObject.TryGetProperty("partitions", out var partitions) ||
                partitions.ValueKind != JsonValueKind.Array)
            {
                error = "super_def 中没有带镜像的动态分区";
                return false;
            }

            foreach (var value in partitions.EnumerateArray())
            {
                if (value.ValueKind != JsonValueKind.Object) continue;
                var isDynamic = value.TryGetProperty("is_dynamic", out var dyn) && dyn.ValueKind == JsonValueKind.True;
                var pathText = value.TryGetProperty("path", out var pe) ? (pe.GetString() ?? string.Empty).Trim() : string.Empty;
                if (!isDynamic || pathText.Length == 0) continue;

                var image = new DynamicSuperImage
                {
                    PartitionName = value.TryGetProperty("name", out var ne) ? (ne.GetString() ?? string.Empty).Trim() : string.Empty,
                    SourcePath = ResolvePackageFile(packageRoot, definitionPath, pathText),
                };
                if (image.PartitionName.Length == 0 || image.SourcePath.Length == 0)
                {
                    error = $"动态分区镜像不存在：{image.PartitionName}";
                    return false;
                }

                try
                {
                    using var source = File.Open(image.SourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                    image.Sparse = SparseImage.HasSparseMagic(source);
                    if (image.Sparse)
                    {
                        if (!SparseImage.Inspect(source, out var sparseInfo, out error)) return false;
                        image.ExpandedSize = sparseInfo.ExpandedSize;
                    }
                    else
                    {
                        image.ExpandedSize = (ulong)source.Length;
                    }
                }
                catch (Exception ex)
                {
                    error = $"打开动态分区镜像失败：{ex.Message}";
                    return false;
                }

                if (!vendor.Partitions.TryGetValue(image.PartitionName.ToLowerInvariant(), out var partition))
                {
                    error = $"super_meta 中缺少动态分区：{image.PartitionName}";
                    return false;
                }

                ulong logicalOffset = 0;
                for (uint index = 0; index < partition.ExtentCount; ++index)
                {
                    var extent = vendor.Extents[(int)(partition.FirstExtent + index)];
                    if (extent.Type != 0 || extent.TargetSource != 0 ||
                        extent.Sectors > ulong.MaxValue / LpSectorSize ||
                        extent.TargetData > ulong.MaxValue / LpSectorSize)
                    {
                        error = $"动态分区 extent 类型错误：{image.PartitionName}";
                        return false;
                    }

                    var mapped = new DynamicSuperExtent
                    {
                        LogicalOffset = logicalOffset,
                        PhysicalOffset = extent.TargetData * LpSectorSize,
                        ByteCount = extent.Sectors * LpSectorSize,
                    };
                    if (mapped.ByteCount > plan.BlockDeviceSize ||
                        mapped.PhysicalOffset > plan.BlockDeviceSize - mapped.ByteCount)
                    {
                        error = $"动态分区 extent 超出 super：{image.PartitionName}";
                        return false;
                    }
                    logicalOffset += mapped.ByteCount;
                    image.Extents.Add(mapped);
                }

                image.AllocatedSize = logicalOffset;
                if (image.ExpandedSize > image.AllocatedSize || image.AllocatedSize == 0)
                {
                    error = $"动态分区镜像超出 extent：{image.PartitionName}" +
                            $"（{image.ExpandedSize} > {image.AllocatedSize}）";
                    return false;
                }
                plan.Images.Add(image);
            }

            if (plan.Images.Count == 0)
            {
                error = "super_def 中没有带镜像的动态分区";
                return false;
            }
            return true;
        }
    }
}
