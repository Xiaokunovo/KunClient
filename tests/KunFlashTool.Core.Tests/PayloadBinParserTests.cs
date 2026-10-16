using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Text;
using KunFlashTool.Core.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// payload.bin（CrAU + protobuf manifest）解析回归。
///
/// 背景：PayloadPage 原先按自造的「8 字节 magic + entryCount」格式解析，真实 payload.bin
/// 永远解析不出来（全部落到名称扫描兜底，提取必然「无偏移信息」）。本测试用**手工构造的
/// 合成 payload**（与 AOSP update_metadata.proto 同构）锁住解析语义：
///   · 头部：CrAU + version(2) + manifest_size + manifest_signature_size（全大端）
///   · manifest：field2=partitions{field1=name, field3=operations{type, data_offset,
///     data_length, dst_extents{start_block, num_blocks}}}
///   · v2 的 data_offset 是**文件内绝对偏移**；REPLACE 段必须落成正确 (FileOffset→OutOffset)。
/// </summary>
public sealed class PayloadBinParserTests
{
    private const uint BlockSize = 4096;

    // ───────── protobuf wire 编码助手 ─────────

    private static void WriteVarint(List<byte> buf, ulong value)
    {
        while (value >= 0x80)
        {
            buf.Add((byte)(value | 0x80));
            value >>= 7;
        }
        buf.Add((byte)value);
    }

    private static void VarintField(List<byte> buf, uint fieldNo, ulong value)
    {
        WriteVarint(buf, ((ulong)fieldNo << 3) | 0);
        WriteVarint(buf, value);
    }

    private static void LenField(List<byte> buf, uint fieldNo, List<byte> inner)
    {
        WriteVarint(buf, ((ulong)fieldNo << 3) | 2);
        WriteVarint(buf, (ulong)inner.Count);
        buf.AddRange(inner);
    }

    private static byte[] Str(string s) => Encoding.ASCII.GetBytes(s);

    /// <summary>
    /// 构造一个三分区合成 payload：
    ///   boot   = REPLACE(块0-1, 0xAA) + REPLACE(块6, 0xBB)   → 可提取、两段
    ///   system = REPLACE(块0, 0xCC)   + ZERO(块1-2)           → 可提取、单段（零块靠零初始化）
    ///   vendor = SOURCE_COPY                                   → 不可提取（增量算子）
    /// </summary>
    private static byte[] MakePayload()
    {
        // 先用一个估算 manifest 长度算 data 区起点（v2 的 data_offset 是绝对偏移，
        // 而 offset 的 varint 长度取决于它自己的大小 —— 先假设 dataStart < 16384（两字节 varint），
        // 两次构造收敛即可；构造完成后再校验一次实际偏移与写入值一致。
        const int headerLen = 4 + 8 + 8 + 4;   // magic + version + manifest_size + sig_size
        var manifest = BuildManifest(16384, 16384 + 2 * BlockSize, 16384 + 3 * BlockSize);
        var dataStart = (ulong)(headerLen + manifest.Count);
        var pad = (4 - (int)(dataStart % 4)) % 4;
        dataStart += (ulong)pad;

        manifest = BuildManifest(dataStart, dataStart + 2 * BlockSize, dataStart + 3 * BlockSize);
        pad = (4 - (int)((headerLen + manifest.Count) % 4)) % 4;
        dataStart = (ulong)(headerLen + manifest.Count) + (ulong)pad;

        var file = new List<byte>();
        file.AddRange(Str("CrAU"));
        var v = new byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(v, 2);
        file.AddRange(v);
        var ms = new byte[8];
        BinaryPrimitives.WriteUInt64BigEndian(ms, (ulong)manifest.Count);
        file.AddRange(ms);
        var sig = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(sig, 0);
        file.AddRange(sig);
        file.AddRange(manifest);
        for (var i = 0; i < pad; i++) file.Add(0);
        // 数据区：boot 0xAA×2 块 + boot 0xBB×1 块 + system 0xCC×1 块
        for (ulong i = 0; i < 2 * BlockSize; i++) file.Add(0xAA);
        for (ulong i = 0; i < BlockSize; i++) file.Add(0xBB);
        for (ulong i = 0; i < BlockSize; i++) file.Add(0xCC);
        return file.ToArray();
    }

    private static List<byte> BuildManifest(ulong bootOff0, ulong bootOff1, ulong sysOff)
    {
        var manifest = new List<byte>();

        // PartitionUpdate "boot"：REPLACE×2
        var boot = new List<byte>();
        LenField(boot, 1, new List<byte>(Str("boot")));
        var op0 = new List<byte>();
        VarintField(op0, 1, 2);                       // type = REPLACE
        VarintField(op0, 2, bootOff0);                // data_offset（v2 = 绝对）
        VarintField(op0, 3, 2 * BlockSize);           // data_length
        var ext0 = new List<byte>();
        VarintField(ext0, 1, 0);                      // start_block
        VarintField(ext0, 2, 2);                      // num_blocks
        LenField(op0, 4, ext0);
        LenField(boot, 3, op0);
        var op1 = new List<byte>();
        VarintField(op1, 1, 2);
        VarintField(op1, 2, bootOff1);
        VarintField(op1, 3, BlockSize);
        var ext1 = new List<byte>();
        VarintField(ext1, 1, 6);
        VarintField(ext1, 2, 1);
        LenField(op1, 4, ext1);
        LenField(boot, 3, op1);
        LenField(manifest, 2, boot);

        // PartitionUpdate "system"：REPLACE + ZERO
        var system = new List<byte>();
        LenField(system, 1, new List<byte>(Str("system")));
        var so = new List<byte>();
        VarintField(so, 1, 2);
        VarintField(so, 2, sysOff);
        VarintField(so, 3, BlockSize);
        var se = new List<byte>();
        VarintField(se, 1, 0);
        VarintField(se, 2, 1);
        LenField(so, 4, se);
        LenField(system, 3, so);
        var sz = new List<byte>();
        VarintField(sz, 1, 6);                        // ZERO：目标区保持全零
        var sze = new List<byte>();
        VarintField(sze, 1, 1);
        VarintField(sze, 2, 2);
        LenField(sz, 4, sze);
        LenField(system, 3, sz);
        LenField(manifest, 2, system);

        // PartitionUpdate "vendor"：SOURCE_COPY → 不可提取
        var vendor = new List<byte>();
        LenField(vendor, 1, new List<byte>(Str("vendor")));
        var vo = new List<byte>();
        VarintField(vo, 1, 0);
        LenField(vendor, 3, vo);
        LenField(manifest, 2, vendor);

        return manifest;
    }

    [Fact]
    public void ParsesRealLayout_WithAbsoluteV2DataOffsets()
    {
        var bytes = MakePayload();
        using var ms = new MemoryStream(bytes);
        var info = PayloadBinParser.Parse(ms);

        Assert.Equal(2u, info.FormatVersion);
        Assert.Equal(3, info.Partitions.Count);

        var boot = Assert.Single(info.Partitions, p => p.Name == "boot");
        Assert.True(boot.Extractable);
        Assert.Equal(2, boot.Segments.Count);
        Assert.Equal(0UL, boot.Segments[0].OutOffset);
        Assert.Equal(2 * BlockSize, boot.Segments[0].Length);
        Assert.Equal(6UL * BlockSize, boot.Segments[1].OutOffset);
        Assert.Equal(BlockSize, boot.Segments[1].Length);
        // 数据段必须在数据区里（紧跟 manifest 之后），且两段相邻
        Assert.True(boot.Segments[0].FileOffset >= 4UL + 8 + 8 + 4);
        Assert.Equal(boot.Segments[0].FileOffset + 2 * BlockSize, boot.Segments[1].FileOffset);
        // 尺寸 = dst extent 最大末端（7 块 × 4096）
        Assert.Equal(7UL * BlockSize, boot.SizeBytes);

        var system = Assert.Single(info.Partitions, p => p.Name == "system");
        Assert.True(system.Extractable);
        Assert.Single(system.Segments);
        Assert.Equal(3UL * BlockSize, system.SizeBytes);

        var vendor = Assert.Single(info.Partitions, p => p.Name == "vendor");
        Assert.False(vendor.Extractable);
        Assert.Contains("SOURCE_COPY", vendor.UnsupportedReason);
    }

    [Fact]
    public void RejectsNonPayloadFile()
    {
        Assert.Throws<FormatException>(
            () => PayloadBinParser.Parse(new MemoryStream(new byte[] { 1, 2, 3, 4, 5, 6 })));
    }

    [Fact]
    public void RejectsTruncatedHeader()
    {
        Assert.Throws<FormatException>(
            () => PayloadBinParser.Parse(new MemoryStream(Encoding.ASCII.GetBytes("CrAU1"))));
    }
}
