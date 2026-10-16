using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Text;
using KunFlashTool.Core.SwtEdl;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 移植层 <c>SwtEdl/Storage/GptParser.cs</c>（对应 <c>third_party/swt-edl/src/storage/GptParser.cpp</c>）的回归用例。
///
/// <para>判据全部从 **UEFI 规范里的 GPT 布局** 推导（头字段偏移、GUID 混合端序、UTF-16LE 分区名），
/// 而不是从当前实现的输出快照抄回来 —— 这样端序处理、边界判断哪天被改错，这里才会真的变红。
/// 两个已知 GUID 特意选成「磁盘序 ≠ 文本序」的，混合端序一写反就会立刻暴露。</para>
/// </summary>
public class SwtEdlGptParserTests
{
    // ---- GPT 头字段偏移（UEFI 规范，非实现细节） ------------------------------------
    private const int HdrSignature = 0;
    private const int HdrHeaderSize = 12;
    private const int HdrEntryLba = 72;
    private const int HdrEntryCount = 80;
    private const int HdrEntrySize = 84;

    // ---- 分区项字段偏移 --------------------------------------------------------------
    private const int EntTypeGuid = 0;
    private const int EntUniqueGuid = 16;
    private const int EntFirstLba = 32;
    private const int EntLastLba = 40;
    private const int EntAttributes = 48;
    private const int EntName = 56;

    /// <summary>Linux 基本数据分区 GUID 的**磁盘序**（前 3 段各自反转，后 8 字节原样）。</summary>
    private static readonly byte[] LinuxDataOnDisk =
    {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
    };
    private const string LinuxDataText = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";

    /// <summary>EFI System Partition GUID 的**磁盘序**。</summary>
    private static readonly byte[] EspOnDisk =
    {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
    };
    private const string EspText = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";

    private static byte[] BuildGpt(uint sectorSize, uint entryLba, uint entryCount,
                                   uint headerSize = 92, uint entrySize = 128)
    {
        var data = new byte[(long)sectorSize * 2 + (long)entryCount * entrySize];
        Encoding.ASCII.GetBytes("EFI PART").CopyTo(data, (int)sectorSize + HdrSignature);
        WriteU32(data, (long)sectorSize + HdrHeaderSize, headerSize);
        WriteU64(data, (long)sectorSize + HdrEntryLba, entryLba);
        WriteU32(data, (long)sectorSize + HdrEntryCount, entryCount);
        WriteU32(data, (long)sectorSize + HdrEntrySize, entrySize);
        return data;
    }

    private static void WriteEntry(byte[] data, uint sectorSize, uint entrySize, uint index,
                                   byte[]? typeGuid, ulong firstLba, ulong lastLba, string name,
                                   ulong attributes = 0)
    {
        var at = (long)sectorSize * 2 + (long)index * entrySize;

        if (typeGuid != null) typeGuid.CopyTo(data, at + EntTypeGuid);

        // unique GUID 只要非零就行（全零 = 空项，会被跳过）；用 index 派生一个稳定值。
        for (var i = 0; i < 16; ++i)
            data[at + EntUniqueGuid + i] = (byte)(0x30 + index * 16 + i);

        WriteU64(data, at + EntFirstLba, firstLba);
        WriteU64(data, at + EntLastLba, lastLba);
        WriteU64(data, at + EntAttributes, attributes);

        var nameBytes = Encoding.Unicode.GetBytes(name);   // UTF-16LE，无 BOM
        var room = (int)Math.Min(entrySize - EntName, 72);
        Array.Copy(nameBytes, 0, data, at + EntName, Math.Min(nameBytes.Length, room));
    }

    private static void WriteU32(byte[] data, long at, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(data.AsSpan((int)at), value);

    private static void WriteU64(byte[] data, long at, ulong value) =>
        BinaryPrimitives.WriteUInt64LittleEndian(data.AsSpan((int)at), value);

    [Fact]
    public void Parse_ReadsEntries_WithMixedEndianGuidsAndUtf16Names()
    {
        var data = BuildGpt(512, entryLba: 2, entryCount: 4);
        WriteEntry(data, 512, 128, 0, LinuxDataOnDisk, 1000, 1999, "boot");
        WriteEntry(data, 512, 128, 1, EspOnDisk, 2000, 2999, "system", attributes: 0x1);
        // 末 LBA < 首 LBA 的坏项：会被丢掉（SectorCount 判据）
        WriteEntry(data, 512, 128, 2, LinuxDataOnDisk, 5000, 4999, "bad_range");
        // 第 4 项全零 = 空项，应当被跳过

        var partitions = new List<PartitionInfo>();
        Assert.True(GptParser.Parse(data, 512, lun: 3, partitions, out var error), error);

        Assert.Equal(2, partitions.Count);

        // ① GUID 混合端序：磁盘序 A2A0D0EB-E5B9-3344-… 必须还原成 EBD0A0A2-B9E5-4433-…
        Assert.Equal("boot", partitions[0].Name);
        Assert.Equal(LinuxDataText, partitions[0].TypeGuid);
        Assert.Equal(1000UL, partitions[0].FirstLba);
        Assert.Equal(1999UL, partitions[0].LastLba);
        Assert.Equal(1000UL, partitions[0].SectorCount);
        Assert.Equal(0UL, partitions[0].Attributes);

        // ② 属性位与第二个已知 GUID
        Assert.Equal("system", partitions[1].Name);
        Assert.Equal(EspText, partitions[1].TypeGuid);
        Assert.Equal(2000UL, partitions[1].FirstLba);
        Assert.Equal(1UL, partitions[1].Attributes);

        // ③ LUN 是按调用方传入的值透传的（UFS 多 LUN 靠它区分）
        Assert.All(partitions, p => Assert.Equal(3u, p.Lun));

        // ④ unique GUID 不等于 type GUID，且能反向解析成一个合法 Guid
        Assert.NotEqual(partitions[0].TypeGuid, partitions[0].UniqueGuid);
        Assert.True(Guid.TryParse(partitions[0].UniqueGuid, out _));
    }

    [Fact]
    public void Parse_SingleSectorPartition_AndFourKnSectorSize()
    {
        // 4096 字节扇区 + 首尾同 LBA（1 个扇区）：4Kn UFS 与最小分区两个边界一起覆盖。
        var data = BuildGpt(4096, entryLba: 2, entryCount: 2);
        WriteEntry(data, 4096, 128, 0, LinuxDataOnDisk, 64, 64, "xbl");

        var partitions = new List<PartitionInfo>();
        Assert.True(GptParser.Parse(data, 4096, lun: 0, partitions, out var error), error);

        Assert.Single(partitions);
        Assert.Equal("xbl", partitions[0].Name);
        Assert.Equal(1UL, partitions[0].SectorCount);   // LastLba == FirstLba → 1
    }

    [Fact]
    public void Parse_RejectsTooShortBuffer()
    {
        var partitions = new List<PartitionInfo>();
        // 要求至少 2 个扇区（MBR + GPT 头）；给 1 个扇区 + 1 字节都不够。
        Assert.False(GptParser.Parse(new byte[512], 512, 0, partitions, out var error));
        Assert.Equal("GPT 数据长度不足", error);
    }

    [Fact]
    public void Parse_RejectsMissingSignature()
    {
        var data = BuildGpt(512, entryLba: 2, entryCount: 4);
        Encoding.ASCII.GetBytes("EFI PARX").CopyTo(data, 512 + HdrSignature);

        var partitions = new List<PartitionInfo>();
        Assert.False(GptParser.Parse(data, 512, 0, partitions, out var error));
        Assert.Equal("未找到 GPT 头签名", error);
    }

    [Theory]
    [InlineData(10u, 4u, 128u)]     // headerSize 过小
    [InlineData(92u, 0u, 128u)]     // entryCount 为 0
    [InlineData(92u, 4u, 64u)]      // entrySize 过小
    [InlineData(92u, 4u, 8192u)]    // entrySize 过大
    public void Parse_RejectsBadHeaderFields(uint headerSize, uint entryCount, uint entrySize)
    {
        var data = BuildGpt(512, entryLba: 2, entryCount: 4);
        WriteU32(data, 512 + HdrHeaderSize, headerSize);
        WriteU32(data, 512 + HdrEntryCount, entryCount);
        WriteU32(data, 512 + HdrEntrySize, entrySize);

        var partitions = new List<PartitionInfo>();
        Assert.False(GptParser.Parse(data, 512, 0, partitions, out var error));
        Assert.Equal("GPT 头字段异常", error);
    }

    [Fact]
    public void Parse_RejectsTableOutsideBuffer()
    {
        // entryLba 指到缓冲区外：分区项表根本没被读进来，不能当成"没有分区"放行。
        var data = BuildGpt(512, entryLba: 1000, entryCount: 4);

        var partitions = new List<PartitionInfo>();
        Assert.False(GptParser.Parse(data, 512, 0, partitions, out var error));
        Assert.Equal("GPT 分区项超出已读取范围", error);
    }

    [Fact]
    public void Parse_RejectsEmptyPartitionTable()
    {
        var data = BuildGpt(512, entryLba: 2, entryCount: 4);

        var partitions = new List<PartitionInfo>();
        Assert.False(GptParser.Parse(data, 512, 0, partitions, out var error));
        Assert.Equal("GPT 中没有有效分区项", error);
        Assert.Empty(partitions);
    }

    [Fact]
    public void PartitionInfo_SectorCount_IsZeroWhenRangeInverted()
    {
        // 即使拿到一个末 < 首的坏项（解析时会被丢掉），SectorCount 也不能下溢成天文数字。
        var info = new PartitionInfo { FirstLba = 5000, LastLba = 4999 };
        Assert.Equal(0UL, info.SectorCount);
    }
}
