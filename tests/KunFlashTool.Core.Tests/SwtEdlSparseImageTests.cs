using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using KunFlashTool.Core.SwtEdl;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 移植层 <c>SwtEdl/Storage/SparseImage.cs</c>（对应 <c>third_party/swt-edl/src/storage/SparseImage.cpp</c>）的回归用例。
///
/// <para>输入是**按 libsparse 规范手工拼出来**的文件，不是从实机上抓的镜像 —— 这样才能精确控制
/// 每种 chunk 的边界（尤其是 CRC chunk 不占输出块、FILL chunk 的 4 字节载荷）。</para>
/// </summary>
public class SwtEdlSparseImageTests : IDisposable
{
    private const ushort ChunkRaw = 0xCAC1;
    private const ushort ChunkFill = 0xCAC2;
    private const ushort ChunkSkip = 0xCAC3;
    private const ushort ChunkCrc = 0xCAC4;

    private readonly string _directory;

    public SwtEdlSparseImageTests()
    {
        _directory = Path.Combine(Path.GetTempPath(), "swtedl-sparse-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(_directory);
    }

    public void Dispose()
    {
        try { Directory.Delete(_directory, recursive: true); } catch { }
    }

    private string WriteTemp(byte[] bytes)
    {
        var path = Path.Combine(_directory, Guid.NewGuid().ToString("n") + ".img");
        File.WriteAllBytes(path, bytes);
        return path;
    }

    private static byte[] Header(uint magic, ushort major, ushort fileHeaderSize,
                                 ushort chunkHeaderSize, uint blockSize, uint totalBlocks,
                                 uint totalChunks)
    {
        var b = new byte[28];
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(0), magic);      // magic
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(4), major);      // major_version
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(6), 0);          // minor_version
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(8), fileHeaderSize);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(10), chunkHeaderSize);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(12), blockSize);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(16), totalBlocks);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(20), totalChunks);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(24), 0);         // crc32（不校验）
        return b;
    }

    private static byte[] ChunkHeader(ushort type, uint chunkBlocks, uint totalSize) =>
        Chunk(type, chunkBlocks, totalSize, payload: null);

    private static byte[] Chunk(ushort type, uint chunkBlocks, uint totalSize, byte[]? payload)
    {
        var head = new byte[12];
        BinaryPrimitives.WriteUInt16LittleEndian(head.AsSpan(0), type);
        BinaryPrimitives.WriteUInt16LittleEndian(head.AsSpan(2), 0);       // reserved
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(4), chunkBlocks);
        BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(8), totalSize);
        if (payload is null) return head;

        var all = new byte[head.Length + payload.Length];
        head.CopyTo(all, 0);
        payload.CopyTo(all, head.Length);
        return all;
    }

    private static byte[] Concat(params byte[][] parts)
    {
        var result = new List<byte>();
        foreach (var p in parts) result.AddRange(p);
        return result.ToArray();
    }

    /// <summary>一个 4 块 / 4096 的 sparse：RAW(1) + FILL(2) + DONT_CARE(1) + CRC(0，应被忽略)。</summary>
    private static byte[] BuildSample()
    {
        const uint blockSize = 4096;
        var rawPayload = new byte[blockSize];
        for (var i = 0; i < rawPayload.Length; ++i) rawPayload[i] = 0xAA;

        var fillPayload = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(fillPayload, 0xDEADBEEF);

        var crcPayload = new byte[4];

        return Concat(
            Header(SparseImage.Magic, 1, 28, 12, blockSize, totalBlocks: 4, totalChunks: 4),
            Chunk(ChunkRaw, 1, 12 + blockSize, rawPayload),   // 载荷落在 28+12 = 40
            Chunk(ChunkFill, 2, 12 + 4, fillPayload),
            ChunkHeader(ChunkSkip, 1, 12),
            Chunk(ChunkCrc, 0, 12 + 4, crcPayload));
    }

    [Fact]
    public void Inspect_ParsesRawFillSkip_AndIgnoresCrcChunk()
    {
        var path = WriteTemp(BuildSample());

        using var fs = new FileStream(path, FileMode.Open, FileAccess.ReadWrite);

        // ① magic 探测必须**不移动游标**（调用方紧接着还要读文件头）
        Assert.Equal(0L, fs.Position);
        Assert.True(SparseImage.HasSparseMagic(fs));
        Assert.Equal(0L, fs.Position);

        Assert.True(SparseImage.Inspect(fs, out var info, out var error), error);

        Assert.Equal(4096u, info.BlockSize);
        Assert.Equal(4UL, info.TotalBlocks);
        Assert.Equal(16384UL, info.ExpandedSize);      // 4 × 4096

        // ② CRC chunk 不产生 extent
        Assert.Equal(3, info.Extents.Count);

        Assert.Equal(SparseExtentType.Raw, info.Extents[0].Type);
        Assert.Equal(0UL, info.Extents[0].OutputBlock);
        Assert.Equal(1UL, info.Extents[0].BlockCount);
        Assert.Equal(40UL, info.Extents[0].FileOffset);   // 文件头 28 + chunk 头 12

        Assert.Equal(SparseExtentType.Fill, info.Extents[1].Type);
        Assert.Equal(1UL, info.Extents[1].OutputBlock);
        Assert.Equal(2UL, info.Extents[1].BlockCount);
        Assert.Equal(0xDEADBEEFu, info.Extents[1].FillValue);

        Assert.Equal(SparseExtentType.Skip, info.Extents[2].Type);
        Assert.Equal(3UL, info.Extents[2].OutputBlock);
        Assert.Equal(1UL, info.Extents[2].BlockCount);

        // ③ Inspect 结束时把游标归零，方便后面直接从 RAW 载荷开始顺序读
        Assert.Equal(0L, fs.Position);
    }

    [Fact]
    public void HasSparseMagic_ReturnsFalse_ForPlainImage()
    {
        var path = WriteTemp(new byte[] { 0x00, 0x01, 0x02, 0x03, 0x04 });
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
        Assert.False(SparseImage.HasSparseMagic(fs));
    }

    [Fact]
    public void Inspect_RejectsTruncatedHeader()
    {
        var path = WriteTemp(new byte[20]);   // 不足 28 字节的文件头
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
        Assert.False(SparseImage.Inspect(fs, out _, out var error));
        Assert.Equal("读取 Android sparse 文件头失败", error);
    }

    [Theory]
    [InlineData(0x12345678u, (ushort)1, 4096u, 4u)]   // magic 不对
    [InlineData(SparseImage.Magic, (ushort)2, 4096u, 4u)]  // major 版本不支持
    [InlineData(SparseImage.Magic, (ushort)1, 0u, 4u)]     // blockSize 为 0
    [InlineData(SparseImage.Magic, (ushort)1, 4096u, 0u)]  // totalBlocks 为 0
    public void Inspect_RejectsInvalidHeader(uint magic, ushort major, uint blockSize, uint totalBlocks)
    {
        var path = WriteTemp(Header(magic, major, 28, 12, blockSize, totalBlocks, totalChunks: 0));
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
        Assert.False(SparseImage.Inspect(fs, out _, out var error));
        Assert.Equal("Android sparse 文件头无效", error);
    }

    [Fact]
    public void Inspect_RejectsUnknownChunkType()
    {
        var file = Concat(
            Header(SparseImage.Magic, 1, 28, 12, 4096, totalBlocks: 1, totalChunks: 1),
            ChunkHeader(0xCAC5, 1, 12));

        var path = WriteTemp(file);
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
        Assert.False(SparseImage.Inspect(fs, out _, out var error));
        Assert.Equal("未知 sparse chunk 类型：cac5", error);
    }

    [Fact]
    public void Inspect_RejectsBlockCountMismatch()
    {
        // 文件头声明 99 块，实际 chunk 只铺了 1 块 → 绝不能当成"刷 1 块"糊弄过去。
        var file = Concat(
            Header(SparseImage.Magic, 1, 28, 12, 4096, totalBlocks: 99, totalChunks: 1),
            ChunkHeader(ChunkSkip, 1, 12));

        var path = WriteTemp(file);
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
        Assert.False(SparseImage.Inspect(fs, out _, out var error));
        Assert.Equal("sparse 展开块数不匹配：1 / 99", error);
    }

    [Fact]
    public void Inspect_RejectsRawChunkLengthMismatch()
    {
        // RAW chunk 的 totalSize 必须等于 chunk 头 + 块数 × 块大小；写错说明文件被截断/损坏。
        // ⚠️ 载荷必须**真的写进去**：越界检查在长度检查之前，文件不够长会先报"长度越界"。
        var file = Concat(
            Header(SparseImage.Magic, 1, 28, 12, 4096, totalBlocks: 1, totalChunks: 1),
            Chunk(ChunkRaw, 1, 12 + 100, new byte[100]));

        var path = WriteTemp(file);
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
        Assert.False(SparseImage.Inspect(fs, out _, out var error));
        Assert.Equal("sparse RAW chunk 0 长度错误", error);
    }

    [Fact]
    public void Inspect_RejectsChunkBeyondEndOfFile()
    {
        // 与上面那条区分开：载荷没写全（totalSize 合法但文件被截断）→ 越界先于长度检查触发
        var file = Concat(
            Header(SparseImage.Magic, 1, 28, 12, 4096, totalBlocks: 1, totalChunks: 1),
            ChunkHeader(ChunkRaw, 1, 12 + 4096));

        var path = WriteTemp(file);
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read);
        Assert.False(SparseImage.Inspect(fs, out _, out var error));
        Assert.Equal("sparse chunk 0 长度越界", error);
    }
}
