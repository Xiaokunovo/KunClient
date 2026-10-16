using System;
using System.Buffers.Binary;
using System.Text;
using KunFlashTool.Core.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>LP（super 动态分区）元数据解析测试。</summary>
public class LpMetadataParserTests
{
    private const int HeaderSize = 128;
    private const int MetadataMaxSize = 4096;
    private const int PartitionsOffset = 128;
    private const int ExtentsOffset = PartitionsOffset + 2 * 52;   // 232
    private const int DevicesOffset = ExtentsOffset + 2 * 24;      // 280

    private static byte[] BuildGeometry(uint slotCount = 2, int metadataMaxSize = MetadataMaxSize, uint magic = LpMetadataParser.GeometryMagic)
    {
        var geometry = new byte[LpMetadataParser.GeometrySize];
        BinaryPrimitives.WriteUInt32LittleEndian(geometry.AsSpan(0), magic);
        BinaryPrimitives.WriteUInt32LittleEndian(geometry.AsSpan(4), 52);
        BinaryPrimitives.WriteUInt32LittleEndian(geometry.AsSpan(40), (uint)metadataMaxSize);
        BinaryPrimitives.WriteUInt32LittleEndian(geometry.AsSpan(44), slotCount);
        BinaryPrimitives.WriteUInt32LittleEndian(geometry.AsSpan(48), 4096);
        return geometry;
    }

    private static byte[] BuildMetadata(uint headerMagic = LpMetadataParser.HeaderMagic, uint badExtentIndex = uint.MaxValue)
    {
        var buffer = new byte[MetadataMaxSize];
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(0), headerMagic);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(8), HeaderSize);

        // 描述符中的 offset 是相对 header 末尾（headerSize）的偏移，表数据紧随 header 之后
        WriteDescriptor(buffer, 80, 0, 2, 52);
        WriteDescriptor(buffer, 92, 2 * 52, 2, 24);
        WriteDescriptor(buffer, 116, 2 * 52 + 2 * 24, 1, 64);

        WritePartition(buffer, PartitionsOffset, "system_a", firstExtentIndex: 0, numExtents: 1);
        var vendorExtentIndex = badExtentIndex == uint.MaxValue ? 1u : badExtentIndex;
        WritePartition(buffer, PartitionsOffset + 52, "vendor_a", firstExtentIndex: vendorExtentIndex, numExtents: 1);

        WriteExtent(buffer, ExtentsOffset, numSectors: 2048, targetType: LpMetadataParser.TargetTypeLinear, targetData: 1024);
        WriteExtent(buffer, ExtentsOffset + 24, numSectors: 4096, targetType: LpMetadataParser.TargetTypeZero, targetData: 0);

        BinaryPrimitives.WriteUInt64LittleEndian(buffer.AsSpan(DevicesOffset), 2048);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(DevicesOffset + 8), 1048576);
        BinaryPrimitives.WriteUInt64LittleEndian(buffer.AsSpan(DevicesOffset + 16), 16384ul * 512);
        WriteFixedString(buffer, DevicesOffset + 24, 36, "super");

        return buffer;
    }

    private static void WriteDescriptor(byte[] buffer, int descriptorOffset, int offset, int count, int entrySize)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(descriptorOffset), (uint)offset);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(descriptorOffset + 4), (uint)count);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(descriptorOffset + 8), (uint)entrySize);
    }

    private static void WritePartition(byte[] buffer, int offset, string name, uint firstExtentIndex, uint numExtents)
    {
        WriteFixedString(buffer, offset, 36, name);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(offset + 36), 0);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(offset + 40), firstExtentIndex);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(offset + 44), numExtents);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(offset + 48), 0);
    }

    private static void WriteExtent(byte[] buffer, int offset, ulong numSectors, uint targetType, ulong targetData)
    {
        BinaryPrimitives.WriteUInt64LittleEndian(buffer.AsSpan(offset), numSectors);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(offset + 8), targetType);
        BinaryPrimitives.WriteUInt64LittleEndian(buffer.AsSpan(offset + 12), targetData);
        BinaryPrimitives.WriteUInt32LittleEndian(buffer.AsSpan(offset + 20), 0);
    }

    private static void WriteFixedString(byte[] buffer, int offset, int length, string value)
    {
        var bytes = Encoding.ASCII.GetBytes(value);
        Array.Copy(bytes, 0, buffer, offset, Math.Min(bytes.Length, length));
    }

    [Fact]
    public void Geometry_Parses_ValidHeader()
    {
        var ok = LpMetadataParser.TryParseGeometry(BuildGeometry(), out var geometry, out var error);

        Assert.True(ok, error);
        Assert.Equal(2, geometry.MetadataSlotCount);
        Assert.Equal(MetadataMaxSize, geometry.MetadataMaxSize);
        Assert.Equal(4096u, geometry.LogicalBlockSize);
    }

    [Fact]
    public void Geometry_Rejects_BadMagic()
    {
        var ok = LpMetadataParser.TryParseGeometry(BuildGeometry(magic: 0xDEADBEEF), out _, out var error);

        Assert.False(ok);
        Assert.NotNull(error);
    }

    [Fact]
    public void Parse_ReadsPartitionsExtentsAndDevices()
    {
        var metadata = LpMetadataParser.Parse(BuildGeometry(), BuildMetadata(), out var error);

        Assert.NotNull(metadata);
        Assert.Null(error);
        Assert.Equal(2, metadata!.Partitions.Count);
        Assert.Equal("system_a", metadata.Partitions[0].Name);
        Assert.Equal("vendor_a", metadata.Partitions[1].Name);
        Assert.Equal(2, metadata.Extents.Count);
        Assert.True(metadata.Extents[0].IsLinear);
        Assert.True(metadata.Extents[1].IsZero);
        Assert.Single(metadata.BlockDevices);
        Assert.Equal("super", metadata.BlockDevices[0].PartitionName);
    }

    [Fact]
    public void Parse_ComputesPartitionSizeAndRanges()
    {
        var metadata = LpMetadataParser.Parse(BuildGeometry(), BuildMetadata(), out _);

        Assert.NotNull(metadata);

        // 宽松查找：无槽位后缀也能命中 system_a
        Assert.NotNull(metadata!.FindPartition("system"));
        Assert.Equal((ulong)(2048 * 512), metadata.GetPartitionSize("system"));
        Assert.Equal((ulong)(4096 * 512), metadata.GetPartitionSize("vendor_a"));

        var linear = metadata.GetPartitionRanges("system_a");
        Assert.Single(linear);
        Assert.False(linear[0].IsZero);
        Assert.Equal(1024ul * 512, linear[0].ByteOffset);
        Assert.Equal(2048ul * 512, linear[0].ByteLength);

        var zero = metadata.GetPartitionRanges("vendor_a");
        Assert.Single(zero);
        Assert.True(zero[0].IsZero);
        Assert.Equal(0ul, zero[0].ByteOffset);
    }

    [Fact]
    public void Offsets_FollowLpLayout()
    {
        // slot1 的 metadata 紧随 geometry + slot0
        Assert.Equal(LpMetadataParser.GeometrySize + MetadataMaxSize, LpMetadataParser.GetMetadataOffset(1, MetadataMaxSize));
        // 数据区起点 = geometry + metadata_max_size * slot_count
        Assert.Equal(LpMetadataParser.GeometrySize + MetadataMaxSize * 2L, LpMetadataParser.GetDataStartOffset(MetadataMaxSize, 2));
    }

    [Fact]
    public void Parse_Fails_OnBadMetadataMagic()
    {
        var metadata = LpMetadataParser.Parse(BuildGeometry(), BuildMetadata(headerMagic: 0xDEADBEEF), out var error);

        Assert.Null(metadata);
        Assert.NotNull(error);
    }

    [Fact]
    public void Parse_Fails_OnOutOfRangeExtentIndex()
    {
        var metadata = LpMetadataParser.Parse(BuildGeometry(), BuildMetadata(badExtentIndex: 99), out var error);

        Assert.Null(metadata);
        Assert.NotNull(error);
    }
}
