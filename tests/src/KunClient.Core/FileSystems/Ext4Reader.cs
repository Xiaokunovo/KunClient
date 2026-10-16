using System.Buffers.Binary;
using System.IO;
using System.Text;

namespace KunFlashTool.Core.FileSystems;

/// <summary>
/// 最小化 LZ4 块解压器 - 仅支持 LZ4 block format（非 frame format）。
/// 用于解压 EROFS 压缩数据块。移植自 VioletToolBox EdlVersionInfo/Lz4Decompressor.cs。
/// </summary>
public static class Lz4Decompressor
{
    /// <summary>
    /// 解压 LZ4 块数据。
    /// </summary>
    /// <param name="src">压缩数据</param>
    /// <param name="srcOffset">源偏移</param>
    /// <param name="srcLength">压缩数据长度</param>
    /// <param name="dst">输出缓冲区（需预分配）</param>
    /// <param name="dstOffset">目标偏移</param>
    /// <param name="dstLength">期望解压后的大小</param>
    /// <returns>实际解压字节数，-1 表示失败</returns>
    public static int Decompress(byte[] src, int srcOffset, int srcLength,
        byte[] dst, int dstOffset, int dstLength)
    {
        var sIdx = srcOffset;
        var sEnd = srcOffset + srcLength;
        var dIdx = dstOffset;
        var dEnd = dstOffset + dstLength;

        while (sIdx < sEnd && dIdx < dEnd)
        {
            // Token byte
            var token = src[sIdx++];
            var literalLen = (token >> 4) & 0x0F;
            var matchLen = token & 0x0F;

            // 扩展 literal 长度
            if (literalLen == 15)
            {
                while (sIdx < sEnd)
                {
                    var b = src[sIdx++];
                    literalLen += b;
                    if (b != 255) break;
                }
            }

            // 复制 literal 数据
            if (literalLen > 0)
            {
                if (sIdx + literalLen > sEnd || dIdx + literalLen > dEnd)
                    return dIdx - dstOffset; // 部分解压
                Buffer.BlockCopy(src, sIdx, dst, dIdx, literalLen);
                sIdx += literalLen;
                dIdx += literalLen;
            }

            // 检查是否到达末尾（最后一个 sequence 可以没有 match）
            if (sIdx >= sEnd) break;

            // Match offset (2 bytes, little-endian)
            if (sIdx + 2 > sEnd) break;
            var offset = src[sIdx] | (src[sIdx + 1] << 8);
            sIdx += 2;
            if (offset == 0) return -1; // 无效偏移

            // 扩展 match 长度（最小 match = 4）
            matchLen += 4;
            if (matchLen == 19) // 15 + 4
            {
                while (sIdx < sEnd)
                {
                    var b = src[sIdx++];
                    matchLen += b;
                    if (b != 255) break;
                }
            }

            // 复制 match 数据（可能与目标重叠）
            var matchSrc = dIdx - offset;
            if (matchSrc < dstOffset) return -1;

            var toCopy = Math.Min(matchLen, dEnd - dIdx);
            for (var i = 0; i < toCopy; i++)
                dst[dIdx + i] = dst[matchSrc + i];
            dIdx += toCopy;
        }

        return dIdx - dstOffset;
    }
}

/// <summary>
/// ext4 文件系统只读解析器 - 通过 IByteReader 按需读取，定位并读取指定路径的文件。
/// 仅支持 extent-based 布局（Android 默认），支持 64-bit feature。
/// 移植自 VioletToolBox EdlVersionInfo/Ext4Reader.cs。
/// </summary>
public class Ext4Reader
{
    private const ushort EXT4_MAGIC = 0xEF53;
    private const int SUPERBLOCK_OFFSET = 1024;
    private const int ROOT_INODE = 2;
    private const uint INCOMPAT_EXTENTS = 0x0040;
    private const uint INCOMPAT_64BIT = 0x0080;
    private const ushort EXTENT_MAGIC = 0xF30A;

    private readonly IByteReader _reader;

    // Superblock 字段
    private int _blockSize;
    private int _inodeSize;
    private int _inodesPerGroup;
    private int _descSize;
    private bool _has64Bit;
    private bool _hasExtents;
    private bool _valid;

    public bool IsValid => _valid;

    public Ext4Reader(IByteReader reader)
    {
        _reader = reader;
        ParseSuperblock();
    }

    /// <summary>检测数据是否为 ext4 文件系统（superblock @1024+0x38 的 magic）。</summary>
    public static bool Detect(IByteReader reader)
    {
        var magic = reader.ReadBytes(SUPERBLOCK_OFFSET + 0x38, 2);
        if (magic == null) return false;
        return BinaryPrimitives.ReadUInt16LittleEndian(magic) == EXT4_MAGIC;
    }

    /// <summary>读取指定路径的文件内容（如 "build.prop" 或 "etc/build.prop"）。</summary>
    public byte[]? ReadFile(string path)
    {
        if (!_valid) return null;

        var parts = path.Split('/', StringSplitOptions.RemoveEmptyEntries);
        var currentInode = ROOT_INODE;

        for (var i = 0; i < parts.Length; i++)
        {
            var dirData = ReadInodeData(currentInode);
            if (dirData == null) return null;

            var found = FindDirEntry(dirData, parts[i]);
            if (found == null) return null;

            currentInode = found.Value;
        }

        return ReadInodeData(currentInode);
    }

    private void ParseSuperblock()
    {
        var sb = _reader.ReadBytes(SUPERBLOCK_OFFSET, 256);
        if (sb == null) return;

        var magic = BinaryPrimitives.ReadUInt16LittleEndian(sb.AsSpan(0x38));
        if (magic != EXT4_MAGIC) return;

        var logBlockSize = BinaryPrimitives.ReadUInt32LittleEndian(sb.AsSpan(0x18));
        _blockSize = (int)(1024U << (int)logBlockSize);
        _inodesPerGroup = (int)BinaryPrimitives.ReadUInt32LittleEndian(sb.AsSpan(0x28));
        _inodeSize = BinaryPrimitives.ReadUInt16LittleEndian(sb.AsSpan(0x58));
        if (_inodeSize == 0) _inodeSize = 128;

        var featureIncompat = BinaryPrimitives.ReadUInt32LittleEndian(sb.AsSpan(0x60));
        _hasExtents = (featureIncompat & INCOMPAT_EXTENTS) != 0;
        _has64Bit = (featureIncompat & INCOMPAT_64BIT) != 0;
        _descSize = _has64Bit ? BinaryPrimitives.ReadUInt16LittleEndian(sb.AsSpan(0xFE)) : 32;
        if (_descSize < 32) _descSize = 32;

        _valid = true;
    }

    private byte[]? ReadInodeData(int inodeNum)
    {
        var inode = ReadInode(inodeNum);
        if (inode == null) return null;

        var sizeLo = BinaryPrimitives.ReadUInt32LittleEndian(inode.AsSpan(0x04));
        var sizeHi = BinaryPrimitives.ReadUInt32LittleEndian(inode.AsSpan(0x6C));
        var fileSize = (long)sizeLo | ((long)sizeHi << 32);
        if (fileSize <= 0 || fileSize > 64 * 1024 * 1024)
            return fileSize == 0 ? Array.Empty<byte>() : null;

        var flags = BinaryPrimitives.ReadUInt32LittleEndian(inode.AsSpan(0x20));
        var iBlock = new byte[60];
        Buffer.BlockCopy(inode, 0x28, iBlock, 0, 60);

        if (_hasExtents && (flags & 0x00080000) != 0)
            return ReadExtentData(iBlock, fileSize);
        return ReadDirectBlockData(iBlock, fileSize);
    }

    private byte[]? ReadExtentData(byte[] extentBlock, long fileSize)
    {
        var result = new byte[fileSize];
        var written = 0L;

        var ehMagic = BinaryPrimitives.ReadUInt16LittleEndian(extentBlock.AsSpan(0));
        if (ehMagic != EXTENT_MAGIC) return null;

        var ehEntries = BinaryPrimitives.ReadUInt16LittleEndian(extentBlock.AsSpan(2));
        var ehDepth = BinaryPrimitives.ReadUInt16LittleEndian(extentBlock.AsSpan(6));

        if (ehDepth == 0)
        {
            for (var i = 0; i < ehEntries && written < fileSize; i++)
            {
                var off = 12 + i * 12;
                var eeLen = BinaryPrimitives.ReadUInt16LittleEndian(extentBlock.AsSpan(off + 4));
                var eeStartHi = BinaryPrimitives.ReadUInt16LittleEndian(extentBlock.AsSpan(off + 6));
                var eeStartLo = BinaryPrimitives.ReadUInt32LittleEndian(extentBlock.AsSpan(off + 8));
                var physBlock = (long)eeStartLo | ((long)eeStartHi << 32);
                var blockCount = eeLen > 32768 ? eeLen - 32768 : eeLen;

                for (var b = 0; b < blockCount && written < fileSize; b++)
                {
                    var toRead = (int)Math.Min(_blockSize, fileSize - written);
                    var data = _reader.ReadBytes((physBlock + b) * _blockSize, toRead);
                    if (data == null) return null;
                    Buffer.BlockCopy(data, 0, result, (int)written, toRead);
                    written += toRead;
                }
            }
        }
        else
        {
            for (var i = 0; i < ehEntries && written < fileSize; i++)
            {
                var off = 12 + i * 12;
                var leafLo = BinaryPrimitives.ReadUInt32LittleEndian(extentBlock.AsSpan(off + 4));
                var leafHi = BinaryPrimitives.ReadUInt16LittleEndian(extentBlock.AsSpan(off + 8));
                var leafBlock = (long)leafLo | ((long)leafHi << 32);

                var childData = _reader.ReadBytes(leafBlock * _blockSize, _blockSize);
                if (childData == null) return null;

                var childResult = ReadExtentDataFromBlock(childData, fileSize - written);
                if (childResult == null) return null;
                Buffer.BlockCopy(childResult, 0, result, (int)written, childResult.Length);
                written += childResult.Length;
            }
        }

        return result;
    }

    private byte[]? ReadExtentDataFromBlock(byte[] block, long maxSize)
    {
        var ehMagic = BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(0));
        if (ehMagic != EXTENT_MAGIC) return null;

        var ehEntries = BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(2));
        var ehDepth = BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(6));

        var result = new MemoryStream();
        if (ehDepth == 0)
        {
            for (var i = 0; i < ehEntries && result.Length < maxSize; i++)
            {
                var off = 12 + i * 12;
                var eeLen = BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(off + 4));
                var eeStartHi = BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(off + 6));
                var eeStartLo = BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(off + 8));
                var physBlock = (long)eeStartLo | ((long)eeStartHi << 32);
                var blockCount = eeLen > 32768 ? eeLen - 32768 : eeLen;

                for (var b = 0; b < blockCount && result.Length < maxSize; b++)
                {
                    var toRead = (int)Math.Min(_blockSize, maxSize - result.Length);
                    var data = _reader.ReadBytes((physBlock + b) * _blockSize, toRead);
                    if (data == null) return null;
                    result.Write(data, 0, toRead);
                }
            }
            return result.ToArray();
        }
        else
        {
            for (var i = 0; i < ehEntries && result.Length < maxSize; i++)
            {
                var off = 12 + i * 12;
                var leafLo = BinaryPrimitives.ReadUInt32LittleEndian(block.AsSpan(off + 4));
                var leafHi = BinaryPrimitives.ReadUInt16LittleEndian(block.AsSpan(off + 8));
                var leafBlock = (long)leafLo | ((long)leafHi << 32);

                var childData = _reader.ReadBytes(leafBlock * _blockSize, _blockSize);
                if (childData == null) return null;

                var childResult = ReadExtentDataFromBlock(childData, maxSize - result.Length);
                if (childResult == null) return null;
                result.Write(childResult, 0, childResult.Length);
            }
            return result.ToArray();
        }
    }

    private byte[]? ReadDirectBlockData(byte[] iBlock, long fileSize)
    {
        var result = new byte[fileSize];
        var written = 0L;

        for (var i = 0; i < 12 && written < fileSize; i++)
        {
            var blockNum = BinaryPrimitives.ReadUInt32LittleEndian(iBlock.AsSpan(i * 4));
            if (blockNum == 0) break;
            var toRead = (int)Math.Min(_blockSize, fileSize - written);
            var data = _reader.ReadBytes((long)blockNum * _blockSize, toRead);
            if (data == null) return null;
            Buffer.BlockCopy(data, 0, result, (int)written, toRead);
            written += toRead;
        }

        return result;
    }

    private byte[]? ReadInode(int inodeNum)
    {
        if (inodeNum < 1) return null;

        var group = (inodeNum - 1) / _inodesPerGroup;
        var indexInGroup = (inodeNum - 1) % _inodesPerGroup;

        var descBlockStart = _blockSize == 1024 ? 2 * _blockSize : _blockSize;
        var descOffset = descBlockStart + group * _descSize;
        var desc = _reader.ReadBytes(descOffset, _descSize);
        if (desc == null) return null;

        var inodeTableLo = BinaryPrimitives.ReadUInt32LittleEndian(desc.AsSpan(8));
        var inodeTableBlock = (long)inodeTableLo;
        if (_has64Bit && _descSize >= 64)
        {
            var inodeTableHi = BinaryPrimitives.ReadUInt32LittleEndian(desc.AsSpan(40));
            inodeTableBlock |= (long)inodeTableHi << 32;
        }

        var inodeOffset = inodeTableBlock * _blockSize + (long)indexInGroup * _inodeSize;
        return _reader.ReadBytes(inodeOffset, _inodeSize);
    }

    private static int? FindDirEntry(byte[] dirData, string name)
    {
        var pos = 0;
        while (pos + 8 <= dirData.Length)
        {
            var inode = BinaryPrimitives.ReadUInt32LittleEndian(dirData.AsSpan(pos));
            var recLen = BinaryPrimitives.ReadUInt16LittleEndian(dirData.AsSpan(pos + 4));
            var nameLen = dirData[pos + 6];

            if (recLen < 8 || pos + recLen > dirData.Length) break;

            if (inode != 0 && nameLen > 0 && pos + 8 + nameLen <= dirData.Length)
            {
                var entryName = Encoding.UTF8.GetString(dirData, pos + 8, nameLen);
                if (entryName == name)
                    return (int)inode;
            }

            pos += recLen;
        }
        return null;
    }
}
