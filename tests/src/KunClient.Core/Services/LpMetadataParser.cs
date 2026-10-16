using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace KunFlashTool.Core.Services;

/// <summary>
/// Android 动态分区（super 分区）LP 元数据解析器。
/// 纯托管实现，覆盖 lpunpack 的元数据解析能力，用于在无法调用外部 lpunpack 工具时，
/// 直接从设备/镜像定位 system、vendor、product 等逻辑分区的数据区间。
/// 结构参考 AOSP liblp/metadata_format.h。
/// </summary>
public static class LpMetadataParser
{
    // ── 常量 ──
    public const uint GeometryMagic = 0x616C4467;   // "gDla"
    public const uint HeaderMagic = 0x414C5030;     // "0PLA"
    public const int GeometrySize = 4096;           // LP_PARTITION_RESERVED_BYTES
    public const int SectorSize = 512;              // LP_SECTOR_SIZE

    public const uint TargetTypeLinear = 0;
    public const uint TargetTypeZero = 1;

    private const int GeometryStructSize = 52;
    private const int HeaderMinSize = 128;
    private const int PartitionMinEntrySize = 52;
    private const int ExtentMinEntrySize = 24;
    private const int BlockDeviceMinEntrySize = 64;

    /// <summary>super 元数据 geometry（位于 super 分区起始 4096 字节）。</summary>
    public sealed class LpGeometry
    {
        public uint StructSize { get; set; }
        public int MetadataMaxSize { get; set; }
        public int MetadataSlotCount { get; set; }
        public uint LogicalBlockSize { get; set; }
        public byte[] Checksum { get; } = new byte[32];
    }

    /// <summary>块设备表条目。</summary>
    public sealed class LpBlockDevice
    {
        public ulong FirstLogicalSector { get; set; }
        public uint Alignment { get; set; }
        public uint AlignmentOffset { get; set; }
        public ulong Size { get; set; }
        public string PartitionName { get; set; } = "";
        public uint Flags { get; set; }
    }

    /// <summary>分区表条目。</summary>
    public sealed class LpPartition
    {
        public string Name { get; set; } = "";
        public uint Attributes { get; set; }
        public uint FirstExtentIndex { get; set; }
        public uint NumExtents { get; set; }
        public uint GroupIndex { get; set; }
    }

    /// <summary>区间（extent）表条目。</summary>
    public sealed class LpExtent
    {
        public ulong NumSectors { get; set; }
        public uint TargetType { get; set; }
        /// <summary>线性区间：目标设备内的绝对逻辑扇区号（512 字节单位）。</summary>
        public ulong TargetData { get; set; }
        public uint TargetSource { get; set; }

        public bool IsLinear => TargetType == TargetTypeLinear;
        public bool IsZero => TargetType == TargetTypeZero;
        public ulong SizeBytes => NumSectors * (ulong)SectorSize;
    }

    /// <summary>逻辑分区在 super 中的一段数据区间。</summary>
    public sealed class LpRange
    {
        /// <summary>相对 super 分区起始的字节偏移。</summary>
        public ulong ByteOffset { get; set; }
        public ulong ByteLength { get; set; }
        /// <summary>true 表示该区间为全零填充（不含实际数据）。</summary>
        public bool IsZero { get; set; }

        public override string ToString() => $"{(IsZero ? "zero" : "data")} @{ByteOffset}+{ByteLength}";
    }

    /// <summary>解析后的 LP 元数据。</summary>
    public sealed class LpMetadata
    {
        public LpGeometry Geometry { get; set; } = new LpGeometry();
        public List<LpBlockDevice> BlockDevices { get; } = new List<LpBlockDevice>();
        public List<LpPartition> Partitions { get; } = new List<LpPartition>();
        public List<LpExtent> Extents { get; } = new List<LpExtent>();
        /// <summary>实际解析成功的元数据槽位（0 = primary，1 = backup）。</summary>
        public int UsedSlot { get; set; }

        /// <summary>兼容 "system" / "system_a" / "system_b" 的宽松查找。</summary>
        public LpPartition? FindPartition(string name)
        {
            if (string.IsNullOrWhiteSpace(name)) return null;

            var exact = Partitions.FirstOrDefault(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
            if (exact != null) return exact;

            var slotSuffixed = Partitions.FirstOrDefault(p =>
                p.Name.StartsWith(name + "_", StringComparison.OrdinalIgnoreCase));
            if (slotSuffixed != null) return slotSuffixed;

            // 反向兼容：用户传 "system_a"，元数据里只有 "system"
            var trimmed = Partitions.FirstOrDefault(p =>
                name.StartsWith(p.Name + "_", StringComparison.OrdinalIgnoreCase));
            return trimmed;
        }

        /// <summary>逻辑分区理论大小（各 extent 扇区数之和，512 字节单位）。</summary>
        public ulong GetPartitionSize(string name)
        {
            var part = FindPartition(name);
            if (part == null) return 0;

            ulong total = 0;
            for (uint i = 0; i < part.NumExtents; i++)
            {
                var idx = part.FirstExtentIndex + i;
                if (idx >= Extents.Count) break;
                total += Extents[(int)idx].SizeBytes;
            }
            return total;
        }

        /// <summary>逻辑分区在 super 内的数据区间（已解析为相对 super 起始的字节偏移）。</summary>
        public List<LpRange> GetPartitionRanges(string name)
        {
            var ranges = new List<LpRange>();
            var part = FindPartition(name);
            if (part == null) return ranges;

            for (uint i = 0; i < part.NumExtents; i++)
            {
                var idx = part.FirstExtentIndex + i;
                if (idx >= Extents.Count) break;
                var extent = Extents[(int)idx];

                if (extent.IsZero)
                {
                    ranges.Add(new LpRange { ByteOffset = 0, ByteLength = extent.SizeBytes, IsZero = true });
                    continue;
                }

                if (!extent.IsLinear)
                    continue; // 未知 target_type，跳过避免误读

                ranges.Add(new LpRange
                {
                    // target_data 为目标块设备内的绝对逻辑扇区（512B 单位）
                    ByteOffset = extent.TargetData * (ulong)SectorSize,
                    ByteLength = extent.SizeBytes,
                    IsZero = false
                });
            }

            return ranges;
        }
    }

    /// <summary>解析 geometry 区（super 起始 4096 字节）。</summary>
    public static bool TryParseGeometry(ReadOnlySpan<byte> buffer, out LpGeometry geometry, out string? error)
    {
        geometry = new LpGeometry();
        error = null;

        if (buffer.Length < GeometryStructSize)
        {
            error = $"geometry 数据长度不足（{buffer.Length} < {GeometryStructSize}）";
            return false;
        }

        var magic = ReadU32(buffer, 0);
        if (magic != GeometryMagic)
        {
            error = $"geometry magic 无效（0x{magic:X8}）";
            return false;
        }

        var structSize = ReadU32(buffer, 4);
        if (structSize < GeometryStructSize || structSize > (uint)buffer.Length)
        {
            error = $"geometry struct_size 异常（{structSize}）";
            return false;
        }

        var metadataMaxSize = ReadU32(buffer, 40);
        var slotCount = ReadU32(buffer, 44);
        var logicalBlockSize = ReadU32(buffer, 48);

        if (metadataMaxSize < HeaderMinSize || metadataMaxSize > 16u * 1024 * 1024)
        {
            error = $"metadata_max_size 异常（{metadataMaxSize}）";
            return false;
        }

        if (slotCount == 0 || slotCount > 4)
        {
            error = $"metadata_slot_count 异常（{slotCount}）";
            return false;
        }

        geometry.StructSize = structSize;
        geometry.MetadataMaxSize = (int)metadataMaxSize;
        geometry.MetadataSlotCount = (int)slotCount;
        geometry.LogicalBlockSize = logicalBlockSize;
        buffer.Slice(8, 32).CopyTo(geometry.Checksum);
        return true;
    }

    /// <summary>解析 metadata 区，得到分区表/区间表/块设备表。</summary>
    public static LpMetadata? Parse(ReadOnlySpan<byte> geometryBuffer, ReadOnlySpan<byte> metadataBuffer, out string? error)
    {
        error = null;

        if (!TryParseGeometry(geometryBuffer, out var geometry, out error))
            return null;

        if (metadataBuffer.Length < HeaderMinSize)
        {
            error = $"metadata 长度不足（{metadataBuffer.Length} < {HeaderMinSize}）";
            return null;
        }

        var magic = ReadU32(metadataBuffer, 0);
        if (magic != HeaderMagic)
        {
            error = $"metadata header magic 无效（0x{magic:X8}）";
            return null;
        }

        var headerSize = (int)ReadU32(metadataBuffer, 8);
        if (headerSize < HeaderMinSize || headerSize > metadataBuffer.Length)
        {
            error = $"metadata header_size 异常（{headerSize}）";
            return null;
        }

        var metadata = new LpMetadata { Geometry = geometry };

        // 表描述符：partitions@80, extents@92, groups@104, block_devices@116
        if (!ReadTable(metadataBuffer, headerSize, 80, PartitionMinEntrySize, out var partOff, out var partCount, out var partEntrySize, out error))
            return null;
        if (!ReadTable(metadataBuffer, headerSize, 92, ExtentMinEntrySize, out var extOff, out var extCount, out var extEntrySize, out error))
            return null;
        if (!ReadTable(metadataBuffer, headerSize, 116, BlockDeviceMinEntrySize, out var devOff, out var devCount, out var devEntrySize, out error))
            return null;

        var partBase = headerSize + partOff;
        for (var i = 0; i < partCount; i++)
        {
            var off = partBase + i * partEntrySize;
            metadata.Partitions.Add(new LpPartition
            {
                Name = ReadFixedString(metadataBuffer, off, 36),
                Attributes = ReadU32(metadataBuffer, off + 36),
                FirstExtentIndex = ReadU32(metadataBuffer, off + 40),
                NumExtents = ReadU32(metadataBuffer, off + 44),
                GroupIndex = ReadU32(metadataBuffer, off + 48)
            });
        }

        var extBase = headerSize + extOff;
        for (var i = 0; i < extCount; i++)
        {
            var off = extBase + i * extEntrySize;
            metadata.Extents.Add(new LpExtent
            {
                NumSectors = ReadU64(metadataBuffer, off),
                TargetType = ReadU32(metadataBuffer, off + 8),
                TargetData = ReadU64(metadataBuffer, off + 12),
                TargetSource = ReadU32(metadataBuffer, off + 20)
            });
        }

        var devBase = headerSize + devOff;
        for (var i = 0; i < devCount; i++)
        {
            var off = devBase + i * devEntrySize;
            metadata.BlockDevices.Add(new LpBlockDevice
            {
                FirstLogicalSector = ReadU64(metadataBuffer, off),
                Alignment = ReadU32(metadataBuffer, off + 8),
                AlignmentOffset = ReadU32(metadataBuffer, off + 12),
                Size = ReadU64(metadataBuffer, off + 16),
                PartitionName = ReadFixedString(metadataBuffer, off + 24, 36),
                Flags = ReadU32(metadataBuffer, off + 60)
            });
        }

        if (metadata.Partitions.Count == 0)
        {
            error = "metadata 中未找到任何逻辑分区";
            return null;
        }

        // 索引合法性校验：截断越界的 extent 引用，避免后续越界读取
        foreach (var part in metadata.Partitions)
        {
            if (part.FirstExtentIndex >= metadata.Extents.Count && part.NumExtents > 0)
            {
                error = $"分区 {part.Name} 的 extent 索引越界（{part.FirstExtentIndex} >= {metadata.Extents.Count}）";
                return null;
            }
        }

        return metadata;
    }

    /// <summary>指定槽位的 metadata 在 super 内的字节偏移。</summary>
    public static long GetMetadataOffset(int slot, int metadataMaxSize)
        => GeometrySize + (long)Math.Max(0, slot) * metadataMaxSize;

    /// <summary>数据区起始（相对 super 起始的字节偏移）。</summary>
    public static long GetDataStartOffset(int metadataMaxSize, int slotCount)
        => GeometrySize + (long)metadataMaxSize * Math.Max(1, slotCount);

    private static bool ReadTable(ReadOnlySpan<byte> buffer, int headerSize, int descriptorOffset, int minEntrySize,
        out int offset, out int count, out int entrySize, out string? error)
    {
        offset = (int)ReadU32(buffer, descriptorOffset);
        count = (int)ReadU32(buffer, descriptorOffset + 4);
        entrySize = (int)ReadU32(buffer, descriptorOffset + 8);
        error = null;

        if (count == 0)
            return true;

        if (entrySize < minEntrySize)
        {
            error = $"表条目尺寸异常（{entrySize} < {minEntrySize}）";
            return false;
        }

        var start = headerSize + offset;
        var end = start + (long)count * entrySize;
        if (start < 0 || end > buffer.Length)
        {
            error = $"表数据越界（{start}..{end} > {buffer.Length}）";
            return false;
        }

        return true;
    }

    private static string ReadFixedString(ReadOnlySpan<byte> buffer, int offset, int length)
    {
        if (offset < 0 || offset + length > buffer.Length)
            return "";

        var raw = buffer.Slice(offset, length);
        var end = raw.IndexOf((byte)0);
        if (end >= 0) raw = raw.Slice(0, end);
        return Encoding.ASCII.GetString(raw);
    }

    private static uint ReadU32(ReadOnlySpan<byte> buffer, int offset)
        => offset + 4 <= buffer.Length ? BinaryPrimitives.ReadUInt32LittleEndian(buffer.Slice(offset)) : 0u;

    private static ulong ReadU64(ReadOnlySpan<byte> buffer, int offset)
        => offset + 8 <= buffer.Length ? BinaryPrimitives.ReadUInt64LittleEndian(buffer.Slice(offset)) : 0ul;
}
