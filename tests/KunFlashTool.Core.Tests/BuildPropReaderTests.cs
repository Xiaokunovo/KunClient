using System;
using System.Buffers.Binary;
using System.Text;
using KunFlashTool.Core.FileSystems;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// ext4 / EROFS 只读解析器 + build.prop 提取的验收（VioletToolBox EdlVersionInfo 移植）。
/// 用手工构造的最小文件系统镜像钉住二进制布局解析行为。
/// </summary>
public sealed class BuildPropReaderTests
{
    private sealed class ByteArrayReader : IByteReader
    {
        private readonly byte[] _data;
        public ByteArrayReader(byte[] data) => _data = data;

        public byte[]? ReadBytes(long offset, int length)
        {
            if (offset < 0 || length < 0 || offset + length > _data.Length)
                return null;
            return _data[(int)offset..(int)(offset + length)];
        }
    }

    private const string BuildPropContent =
        "ro.product.brand=KunTest\n" +
        "ro.build.version.release=15\n" +
        "# comment line\n" +
        "\n" +
        "badline\n" +
        "ro.build.version.security_patch=2026-09-05\n";

    // ────────── LZ4 块解压 ──────────

    [Fact]
    public void Lz4_DecodesLiteralAndMatch()
    {
        // token 0x40: 4 字节 literal + match 长度 4（offset 指向刚写入的 "abcd"）
        var src = new byte[] { 0x40, (byte)'a', (byte)'b', (byte)'c', (byte)'d', 0x04, 0x00 };
        var dst = new byte[8];

        var written = Lz4Decompressor.Decompress(src, 0, src.Length, dst, 0, dst.Length);

        Assert.Equal(8, written);
        Assert.Equal("abcdabcd", Encoding.ASCII.GetString(dst));
    }

    [Fact]
    public void Lz4_DecodesExtendedLiteralLength()
    {
        // token 0xF0: literal 长度 15 + 扩展字节 5 = 20 字节 literal，无 match（块末尾）
        var literals = Encoding.ASCII.GetBytes(new string('K', 20));
        var src = new byte[2 + 20];
        src[0] = 0xF0;
        src[1] = 5;
        literals.CopyTo(src, 2);
        var dst = new byte[20];

        var written = Lz4Decompressor.Decompress(src, 0, src.Length, dst, 0, dst.Length);

        Assert.Equal(20, written);
        Assert.Equal(literals, dst);
    }

    [Fact]
    public void Lz4_ZeroMatchOffsetIsInvalid()
    {
        var src = new byte[] { 0x40, (byte)'a', (byte)'b', (byte)'c', (byte)'d', 0x00, 0x00 };
        var dst = new byte[8];

        Assert.Equal(-1, Lz4Decompressor.Decompress(src, 0, src.Length, dst, 0, dst.Length));
    }

    // ────────── LogicalPartitionReader ──────────

    [Fact]
    public void LogicalPartitionReader_MapsExtentsAndZeroFills()
    {
        // 4 字节"扇区"；super 内容：扇区0=AAAA、扇区1=未映射、扇区2=CCCC
        var super = new byte[12];
        Encoding.ASCII.GetBytes("AAAA").CopyTo(super, 0);
        Encoding.ASCII.GetBytes("CCCC").CopyTo(super, 8);
        var superReader = new ByteArrayReader(super);

        var extents = new[]
        {
            new LpExtentMapping(0, 1),                    // 物理扇区 0
            new LpExtentMapping(0, 1, IsZero: true),      // zero extent
            new LpExtentMapping(2, 1),                    // 物理扇区 2
        };
        var reader = new LogicalPartitionReader(superReader, extents, 4);

        Assert.Equal("AAAA\0\0\0\0CCCC", Encoding.ASCII.GetString(reader.ReadBytes(0, 12)!));
        // 中段读取（跨 extent 边界）
        Assert.Equal("\0\0\0\0CC", Encoding.ASCII.GetString(reader.ReadBytes(4, 6)!));
        // 越界 → null
        Assert.Null(reader.ReadBytes(10, 4));
    }

    // ────────── ext4 ──────────

    /// <summary>构造最小 ext4（1KB 块）：root 目录(块6) + build.prop(块7)，extent-based。</summary>
    private static byte[] BuildExt4Image()
    {
        const int blockSize = 1024;
        var image = new byte[blockSize * 8];

        // superblock @1024
        int sb = blockSize;
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 0x00), 64);    // inodes_count
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 0x18), 0);     // log block size → 1024
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 0x28), 16);    // inodes_per_group
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(sb + 0x38), 0xEF53);// magic
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(sb + 0x58), 128);   // inode size
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 0x60), 0x40);  // incompat: extents

        // group desc @block 2
        int gd = 2 * blockSize;
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(gd + 8), 5);        // inode table @block 5

        // inode 表 @block5：inode1 @+0，root(inode2) @+128，file(inode3) @+256
        WriteExt4InodeWithExtent(image, 5 * blockSize + 0 * 128, 0, 0);           // 占位
        WriteExt4InodeWithExtent(image, 5 * blockSize + 1 * 128, blockSize, 6);   // root 目录 → 块6（大小必须是整块，否则 dirent rec_len 会被判越界）
        WriteExt4InodeWithExtent(image, 5 * blockSize + 2 * 128, Encoding.UTF8.GetByteCount(BuildPropContent), 7);

        // 目录块 @block6："." ".." "build.prop"
        int dir = 6 * blockSize;
        WriteDirent(image, dir + 0, 2, 12, ".", fileType: 2);
        WriteDirent(image, dir + 12, 2, 12, "..", fileType: 2);
        WriteDirent(image, dir + 24, 3, blockSize - 24, "build.prop", fileType: 1);

        // 文件数据 @block7
        Encoding.UTF8.GetBytes(BuildPropContent).CopyTo(image, 7 * blockSize);
        return image;
    }

    private static void WriteExt4InodeWithExtent(byte[] image, int inodeOffset, int size, uint dataBlock)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(inodeOffset + 0x04), (uint)size);   // size lo
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(inodeOffset + 0x20), 0x00080000);   // EXT4_EXTENTS_FL
        // extent header @0x28: magic / entries=1 / max=4 / depth=0 / gen
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(inodeOffset + 0x28), 0xF30A);
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(inodeOffset + 0x2A), 1);
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(inodeOffset + 0x2C), 4);
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(inodeOffset + 0x2E), 0);
        // extent entry @0x28+12: ee_block / ee_len / start_hi / start_lo
        int ee = inodeOffset + 0x28 + 12;
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(ee), 0);                             // ee_block
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(ee + 4), 1);                         // ee_len
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(ee + 6), 0);                         // start hi
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(ee + 8), dataBlock);                 // start lo
    }

    private static void WriteDirent(byte[] image, int offset, int inode, ushort recLen, string name, byte fileType)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(offset), (uint)inode);
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(offset + 4), recLen);
        image[offset + 6] = (byte)name.Length;
        image[offset + 7] = fileType;
        Encoding.UTF8.GetBytes(name).CopyTo(image, offset + 8);
    }

    [Fact]
    public void Ext4Reader_DetectsAndReadsBuildProp()
    {
        var reader = new ByteArrayReader(BuildExt4Image());
        Assert.True(Ext4Reader.Detect(reader));

        var ext4 = new Ext4Reader(reader);
        Assert.True(ext4.IsValid);

        var data = ext4.ReadFile("build.prop");
        Assert.NotNull(data);
        Assert.Equal(BuildPropContent, Encoding.UTF8.GetString(data!));
    }

    [Fact]
    public void BuildPropReader_ParsesPhysicalExt4Partition()
    {
        var result = new BuildPropResult();
        new BuildPropReader().ReadFromPhysical(result, "vendor_a", new ByteArrayReader(BuildExt4Image()));

        Assert.Equal("vendor", result.PartitionProps.Keys.Single());
        Assert.Equal("KunTest", result.Properties["ro.product.brand"]);
        Assert.Equal("15", result.Properties["ro.build.version.release"]);
        // 注释行 / 空行 / 无等号行都被丢弃
        Assert.Equal(3, result.Properties.Count);
        Assert.Contains("EXT4", result.FileSystems);
    }

    // ────────── EROFS（FLAT_PLAIN） ──────────

    /// <summary>构造最小 EROFS（4KB 块）：root 目录(nid1, 数据块2) + build.prop(nid2, 数据块3)，均为 FLAT_PLAIN。</summary>
    private static byte[] BuildErofsImage()
    {
        const int blockSize = 4096;
        var image = new byte[blockSize * 4];

        int sb = 1024;
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 0), 0xE0F5E1E2);   // magic
        image[sb + 12] = 12;                                                           // blkszbits → 4096
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(sb + 14), 1);            // root_nid = 1
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 36), 4);            // blocks
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 40), 0);            // meta_blk_addr = 块0
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(sb + 80), 0);            // incompat

        // compact inode(32B)：nid1 @offset 32（root 目录），nid2 @offset 64（文件）
        WriteErofsInode(image, 32, size: blockSize, dataBlock: 2);
        WriteErofsInode(image, 64, size: Encoding.UTF8.GetByteCount(BuildPropContent), dataBlock: 3);

        // 目录块 @block2。dirent 12 字节头：nid(8) + nameoff(2) + type(2)；
        // 首条目的 nameoff 同时充当 first_name_off（块头 +8）→ 2 个 dirent。
        //   dirent0 @+0 : nid=2, nameoff=24 → 名字 [24,34) = "build.prop"
        //   dirent1 @+12: nid=0, nameoff=34 → 名字空段（被零截断后跳过）
        int dir = 2 * blockSize;
        BinaryPrimitives.WriteUInt64LittleEndian(image.AsSpan(dir + 0), 2);
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(dir + 8), 24);
        BinaryPrimitives.WriteUInt64LittleEndian(image.AsSpan(dir + 12), 0);
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(dir + 20), 34);
        Encoding.UTF8.GetBytes("build.prop").CopyTo(image, dir + 24);

        // 文件数据 @block3
        Encoding.UTF8.GetBytes(BuildPropContent).CopyTo(image, 3 * blockSize);
        return image;
    }

    private static void WriteErofsInode(byte[] image, int offset, int size, uint dataBlock)
    {
        // i_format: bit0 = extended(0=compact 32B)，bits1-3 = layout(0=FLAT_PLAIN)
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(offset + 0), 0);
        BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(offset + 2), 0);          // xattr count
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(offset + 8), (uint)size); // size（compact u32）
        BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(offset + 16), dataBlock); // raw_blk_addr
    }

    [Fact]
    public void ErofsReader_DetectsAndReadsBuildProp()
    {
        var reader = new ByteArrayReader(BuildErofsImage());
        Assert.True(ErofsReader.Detect(reader));

        var erofs = new ErofsReader(reader);
        Assert.True(erofs.IsValid);

        var data = erofs.ReadFile("build.prop");
        Assert.NotNull(data);
        Assert.Equal(BuildPropContent, Encoding.UTF8.GetString(data!));
    }

    [Fact]
    public void BuildPropReader_MergesSuperLogicalPartitionsByPriority()
    {
        // 同一个属性在 system 与 odm 都出现：odm 优先级更高 → 先到先得
        var superImage = BuildExt4Image(); // 内容当作 odm 分区
        var superReader = new CachedByteReader(
            (offset, length) => offset + length <= superImage.Length
                ? superImage.AsSpan((int)offset, (int)length).ToArray()
                : null,
            512);

        var extents = new[] { new LpExtentMapping(0, superImage.Length / 512) };
        var logical = new Dictionary<string, IReadOnlyList<LpExtentMapping>>(StringComparer.OrdinalIgnoreCase)
        {
            ["odm_a"] = extents,
            ["system_b"] = extents,
        };

        var result = new BuildPropReader().ReadFromSuper(superReader, logical);

        // 两个分区都被解析（物理上同一份镜像），但合并结果是先到先得：odm 优先级高于 system
        Assert.Equal(new[] { "odm", "system" }, result.PartitionProps.Keys.OrderBy(k => k));
        Assert.Equal("KunTest", result.Properties["ro.product.brand"]);
        Assert.Contains("EXT4", result.FileSystems);
    }
}
