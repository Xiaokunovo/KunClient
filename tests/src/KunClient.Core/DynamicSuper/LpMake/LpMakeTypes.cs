using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace KunFlashTool.Core.DynamicSuper.LpMake;

/// <summary>
/// LP（Logical Partition）元数据格式常量，与 AOSP liblp metadata_format.h 对齐。
/// 移植自 VioletToolBox EdlDynamicSuper/LpMake。
/// </summary>
public static class LpMakeConstants
{
    public const uint LpMetadataGeometryMagic = 0x616c4467;
    public const int LpMetadataGeometrySize = 4096;

    public const uint LpMetadataHeaderMagic = 0x414C5030;

    public const ushort LpMetadataMajorVersion = 10;
    public const ushort LpMetadataMinorVersionMin = 0;
    public const ushort LpMetadataMinorVersionMax = 2;
    public const ushort LpMetadataVersionForUpdatedAttr = 1;
    public const ushort LpMetadataVersionForExpandedHeader = 2;

    public const uint LpPartitionAttrNone = 0x0;
    public const uint LpPartitionAttrReadonly = 1u << 0;
    public const uint LpPartitionAttrSlotSuffixed = 1u << 1;
    public const uint LpPartitionAttrUpdated = 1u << 2;
    public const uint LpPartitionAttrDisabled = 1u << 3;

    public const uint LpPartitionAttributeMaskV0 = LpPartitionAttrReadonly | LpPartitionAttrSlotSuffixed;
    public const uint LpPartitionAttributeMaskV1 = LpPartitionAttrUpdated | LpPartitionAttrDisabled;
    public const uint LpPartitionAttributeMask = LpPartitionAttributeMaskV0 | LpPartitionAttributeMaskV1;

    public const string LpMetadataDefaultPartitionName = "super";
    public const int LpSectorSize = 512;
    public const int LpPartitionReservedBytes = 4096;

    public const uint LpHeaderFlagVirtualAbDevice = 0x1;

    public const uint LpTargetTypeLinear = 0;
    public const uint LpTargetTypeZero = 1;

    public const uint LpGroupSlotSuffixed = 1u << 0;
    public const uint LpBlockDeviceSlotSuffixed = 1u << 0;

    public const string KDefaultGroup = "default";
    public const uint KDefaultPartitionAlignment = 1024 * 1024; // 1 MiB
    public const uint KDefaultBlockSize = 4096;

    // 序列化后的 struct 尺寸（与 AOSP 二进制兼容）
    public const int SizeOfGeometry = 52;            // 实际写入 4096
    public const int SizeOfHeaderV10 = 128;
    public const int SizeOfHeaderV12 = 256;
    public const int SizeOfPartition = 52;
    public const int SizeOfExtent = 24;
    public const int SizeOfPartitionGroup = 48;
    public const int SizeOfBlockDevice = 64;
    public const int SizeOfTableDescriptor = 12;
    public const int PartitionNameLen = 36;
}

/// <summary>表描述符 - 描述某张 metadata 表的位置/数量/单条尺寸。</summary>
public sealed class LpMetadataTableDescriptor
{
    public uint Offset;
    public uint NumEntries;
    public uint EntrySize;
}

/// <summary>LP 几何信息（4096 字节区块）。</summary>
public sealed class LpMetadataGeometry
{
    public uint Magic = LpMakeConstants.LpMetadataGeometryMagic;
    public uint StructSize = (uint)LpMakeConstants.SizeOfGeometry;
    public byte[] Checksum = new byte[32];
    public uint MetadataMaxSize;
    public uint MetadataSlotCount;
    public uint LogicalBlockSize;
}

/// <summary>LP 元数据头部。</summary>
public sealed class LpMetadataHeader
{
    public uint Magic = LpMakeConstants.LpMetadataHeaderMagic;
    public ushort MajorVersion = LpMakeConstants.LpMetadataMajorVersion;
    public ushort MinorVersion = LpMakeConstants.LpMetadataMinorVersionMin;
    public uint HeaderSize = LpMakeConstants.SizeOfHeaderV10;
    public byte[] HeaderChecksum = new byte[32];
    public uint TablesSize;
    public byte[] TablesChecksum = new byte[32];

    public LpMetadataTableDescriptor Partitions = new() { EntrySize = LpMakeConstants.SizeOfPartition };
    public LpMetadataTableDescriptor Extents = new() { EntrySize = LpMakeConstants.SizeOfExtent };
    public LpMetadataTableDescriptor Groups = new() { EntrySize = LpMakeConstants.SizeOfPartitionGroup };
    public LpMetadataTableDescriptor BlockDevices = new() { EntrySize = LpMakeConstants.SizeOfBlockDevice };

    // V1.2+
    public uint Flags;
    public byte[] Reserved = new byte[124];
}

/// <summary>分区条目（52 字节）。</summary>
public sealed class LpMetadataPartition
{
    public string Name = string.Empty;
    public uint Attributes;
    public uint FirstExtentIndex;
    public uint NumExtents;
    public uint GroupIndex;
}

/// <summary>分区 extent（24 字节）。</summary>
public sealed class LpMetadataExtent
{
    public ulong NumSectors;
    public uint TargetType;
    public ulong TargetData;
    public uint TargetSource;
}

/// <summary>分区组（48 字节）。</summary>
public sealed class LpMetadataPartitionGroup
{
    public string Name = string.Empty;
    public uint Flags;
    public ulong MaximumSize;
}

/// <summary>块设备条目（64 字节）。</summary>
public sealed class LpMetadataBlockDevice
{
    public ulong FirstLogicalSector;
    public uint Alignment;
    public uint AlignmentOffset;
    public ulong Size;
    public string PartitionName = string.Empty;
    public uint Flags;
}

/// <summary>LP 元数据平坦容器 - 由 <see cref="LpMetadataBuilder.Export"/> 产生，可直接序列化到二进制。</summary>
public sealed class LpMakeMetadata
{
    public LpMetadataGeometry Geometry { get; set; } = new();
    public LpMetadataHeader Header { get; set; } = new();
    public List<LpMetadataPartition> Partitions { get; } = new();
    public List<LpMetadataExtent> Extents { get; } = new();
    public List<LpMetadataPartitionGroup> Groups { get; } = new();
    public List<LpMetadataBlockDevice> BlockDevices { get; } = new();
}

/// <summary>块设备输入信息（用于 Builder 初始化，区别于序列化用的 <see cref="LpMetadataBlockDevice"/>）。</summary>
public sealed class BlockDeviceInfo
{
    public string PartitionName { get; init; } = LpMakeConstants.LpMetadataDefaultPartitionName;
    public ulong Size { get; init; }
    public uint Alignment { get; init; }
    public uint AlignmentOffset { get; init; }
    public uint LogicalBlockSize { get; init; } = LpMakeConstants.KDefaultBlockSize;
}

/// <summary>dm-target 抽象，可被编码进 logical partition 表。</summary>
public abstract class LpExtentBase
{
    public ulong NumSectors { get; set; }

    protected LpExtentBase(ulong numSectors) { NumSectors = numSectors; }

    /// <summary>追加到 metadata.extents 表。</summary>
    public abstract bool AddTo(LpMakeMetadata output);
}

/// <summary>dm-linear extent（实际占用 super 上的连续扇区）。</summary>
public sealed class LinearExtent : LpExtentBase
{
    public uint DeviceIndex { get; }
    public ulong PhysicalSector { get; }

    public LinearExtent(ulong numSectors, uint deviceIndex, ulong physicalSector)
        : base(numSectors)
    {
        DeviceIndex = deviceIndex;
        PhysicalSector = physicalSector;
    }

    public ulong EndSector => PhysicalSector + NumSectors;

    public override bool AddTo(LpMakeMetadata output)
    {
        if (DeviceIndex >= output.BlockDevices.Count)
            return false;
        output.Extents.Add(new LpMetadataExtent
        {
            NumSectors = NumSectors,
            TargetType = LpMakeConstants.LpTargetTypeLinear,
            TargetData = PhysicalSector,
            TargetSource = DeviceIndex
        });
        return true;
    }

    public bool OverlapsWith(LinearExtent other) =>
        DeviceIndex == other.DeviceIndex &&
        PhysicalSector < other.EndSector && other.PhysicalSector < EndSector;

    public LpInterval AsInterval() => new(DeviceIndex, PhysicalSector, EndSector);
}

/// <summary>dm-zero extent（虚拟全零，不占 super 实际空间）。</summary>
public sealed class ZeroExtent : LpExtentBase
{
    public ZeroExtent(ulong numSectors) : base(numSectors) { }

    public override bool AddTo(LpMakeMetadata output)
    {
        output.Extents.Add(new LpMetadataExtent
        {
            NumSectors = NumSectors,
            TargetType = LpMakeConstants.LpTargetTypeZero,
            TargetData = 0,
            TargetSource = 0
        });
        return true;
    }
}

/// <summary>自由/已用区间（按 device 内的扇区计数）。</summary>
public readonly record struct LpInterval(uint DeviceIndex, ulong Start, ulong End) : IComparable<LpInterval>
{
    public ulong Length => End - Start;

    public int CompareTo(LpInterval other)
    {
        if (Start != other.Start)
            return Start.CompareTo(other.Start);
        return End.CompareTo(other.End);
    }
}

/// <summary>逻辑分区（运行期 builder 视角，与二进制条目 <see cref="LpMetadataPartition"/> 区分）。</summary>
public sealed class LpPartitionBuilder
{
    public string Name { get; }
    public string GroupName { get; internal set; }
    public uint Attributes { get; set; }
    public ulong Size { get; private set; }

    private readonly List<LpExtentBase> _extents = new();
    public IReadOnlyList<LpExtentBase> Extents => _extents;

    public LpPartitionBuilder(string name, string groupName, uint attributes)
    {
        Name = name;
        GroupName = groupName;
        Attributes = attributes;
        Size = 0;
    }

    /// <summary>追加 extent（相邻同设备 LinearExtent 自动合并）。</summary>
    public void AddExtent(LpExtentBase extent)
    {
        Size += extent.NumSectors * LpMakeConstants.LpSectorSize;

        if (extent is LinearExtent newLinear &&
            _extents.Count > 0 &&
            _extents[^1] is LinearExtent prev &&
            prev.EndSector == newLinear.PhysicalSector &&
            prev.DeviceIndex == newLinear.DeviceIndex)
        {
            var merged = new LinearExtent(prev.NumSectors + newLinear.NumSectors,
                prev.DeviceIndex, prev.PhysicalSector);
            _extents.RemoveAt(_extents.Count - 1);
            _extents.Add(merged);
            return;
        }

        _extents.Add(extent);
    }

    public void RemoveExtents()
    {
        Size = 0;
        _extents.Clear();
    }

    /// <summary>裁剪到指定大小（字节，必须是扇区对齐）。</summary>
    internal void ShrinkTo(ulong alignedSize)
    {
        if (alignedSize == 0)
        {
            RemoveExtents();
            return;
        }

        ulong sectorsToRemove = (Size - alignedSize) / LpMakeConstants.LpSectorSize;
        while (sectorsToRemove > 0)
        {
            var extent = _extents[^1];
            if (extent.NumSectors > sectorsToRemove)
            {
                Size -= sectorsToRemove * LpMakeConstants.LpSectorSize;
                extent.NumSectors -= sectorsToRemove;
                break;
            }
            Size -= extent.NumSectors * LpMakeConstants.LpSectorSize;
            sectorsToRemove -= extent.NumSectors;
            _extents.RemoveAt(_extents.Count - 1);
        }
    }

    public ulong BytesOnDisk()
    {
        ulong sectors = 0;
        foreach (var e in _extents)
            if (e is LinearExtent) sectors += e.NumSectors;
        return sectors * LpMakeConstants.LpSectorSize;
    }
}

/// <summary>分区组（用于限制一组分区的总大小，例如 qti_dynamic_partitions_a）。</summary>
public sealed class LpPartitionGroupBuilder
{
    public string Name { get; }
    public ulong MaximumSize { get; internal set; }

    public LpPartitionGroupBuilder(string name, ulong maximumSize)
    {
        Name = name;
        MaximumSize = maximumSize;
    }
}

/// <summary>LpMake 通用工具方法。</summary>
public static class LpMakeUtility
{
    /// <summary>向上对齐到 alignment 的倍数（alignment_offset 用于 stacked device）。</summary>
    public static ulong AlignTo(ulong value, ulong alignment, ulong alignmentOffset = 0)
    {
        if (alignment == 0)
            return value;
        ulong rem = (value - alignmentOffset) % alignment;
        if (rem == 0)
            return value;
        return value + (alignment - rem);
    }

    public static uint AlignTo(uint value, uint alignment) =>
        alignment == 0 ? value : (uint)AlignTo((ulong)value, alignment);

    /// <summary>super 上 metadata 所需保留空间（含 reserved + 双份 geometry + 双份每槽 metadata）。</summary>
    public static ulong GetTotalMetadataSize(uint metadataMaxSize, uint maxSlots) =>
        (ulong)LpMakeConstants.LpPartitionReservedBytes +
        ((ulong)LpMakeConstants.LpMetadataGeometrySize + (ulong)metadataMaxSize * maxSlots) * 2UL;

    public static byte[] Sha256(ReadOnlySpan<byte> data) => SHA256.HashData(data);

    /// <summary>将字符串以 ASCII 写入定长字段，不足补零。</summary>
    public static void WriteFixedAsciiName(Span<byte> dest, string name)
    {
        dest.Clear();
        if (string.IsNullOrEmpty(name))
            return;
        int len = Math.Min(name.Length, dest.Length);
        Encoding.ASCII.GetBytes(name.AsSpan(0, len), dest);
    }

    public static void WriteU16(Span<byte> dest, ref int offset, ushort value)
    {
        BinaryPrimitives.WriteUInt16LittleEndian(dest[offset..], value);
        offset += 2;
    }

    public static void WriteU32(Span<byte> dest, ref int offset, uint value)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(dest[offset..], value);
        offset += 4;
    }

    public static void WriteU64(Span<byte> dest, ref int offset, ulong value)
    {
        BinaryPrimitives.WriteUInt64LittleEndian(dest[offset..], value);
        offset += 8;
    }

    public static void WriteBytes(Span<byte> dest, ref int offset, ReadOnlySpan<byte> data)
    {
        data.CopyTo(dest[offset..]);
        offset += data.Length;
    }
}
