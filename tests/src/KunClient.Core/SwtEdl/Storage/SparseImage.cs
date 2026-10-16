using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;

namespace KunFlashTool.Core.SwtEdl;

public enum SparseExtentType
{
    Raw,
    Fill,
    Skip,
}

public sealed class SparseExtent
{
    public SparseExtentType Type { get; set; } = SparseExtentType.Skip;
    public ulong OutputBlock { get; set; }
    public ulong BlockCount { get; set; }
    public ulong FileOffset { get; set; }
    public uint FillValue { get; set; }
}

public sealed class SparseImageInfo
{
    public uint BlockSize { get; set; }
    public ulong TotalBlocks { get; set; }
    public List<SparseExtent> Extents { get; } = new();

    public ulong ExpandedSize => TotalBlocks * BlockSize;
}

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/storage/SparseImage.cpp`：Android sparse 镜像的文件头与 extent 表解析。
/// </summary>
public static class SparseImage
{
    public const uint Magic = 0xED26FF3A;

    private const ushort RawChunk = 0xCAC1;
    private const ushort FillChunk = 0xCAC2;
    private const ushort SkipChunk = 0xCAC3;
    private const ushort CrcChunk = 0xCAC4;

    private static byte[] ReadExact(FileStream file, long offset, long length)
    {
        file.Seek(offset, SeekOrigin.Begin);
        var bytes = new byte[length];
        var read = 0;
        while (read < length)
        {
            var n = file.Read(bytes, read, (int)(length - read));
            if (n <= 0) break;
            read += n;
        }
        if (read != length) return Array.Empty<byte>();
        return bytes;
    }

    public static bool HasSparseMagic(FileStream file)
    {
        var original = file.Position;
        var bytes = ReadExact(file, 0, 4);
        var result = bytes.Length == 4 &&
                     BinaryPrimitives.ReadUInt32LittleEndian(bytes) == Magic;
        file.Seek(original, SeekOrigin.Begin);
        return result;
    }

    public static bool Inspect(FileStream file, out SparseImageInfo info, out string error)
    {
        info = new SparseImageInfo();
        error = string.Empty;

        if (file is null || !file.CanRead)
        {
            error = "稀疏镜像参数错误";
            return false;
        }

        var header = ReadExact(file, 0, 28);
        if (header.Length != 28)
        {
            error = "读取 Android sparse 文件头失败";
            return false;
        }

        var magic = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(0));
        var major = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(4));
        var fileHeaderSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(8));
        var chunkHeaderSize = BinaryPrimitives.ReadUInt16LittleEndian(header.AsSpan(10));
        var blockSize = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(12));
        var totalBlocks = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(16));
        var totalChunks = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(20));

        if (magic != Magic || major != 1 || fileHeaderSize < 28 || chunkHeaderSize < 12 ||
            blockSize == 0 || totalBlocks == 0)
        {
            error = "Android sparse 文件头无效";
            return false;
        }

        long cursor = fileHeaderSize;
        ulong outputBlock = 0;

        for (uint index = 0; index < totalChunks; ++index)
        {
            var chunk = ReadExact(file, cursor, chunkHeaderSize);
            if (chunk.Length != chunkHeaderSize)
            {
                error = $"读取 sparse chunk {index} 失败";
                return false;
            }

            var type = BinaryPrimitives.ReadUInt16LittleEndian(chunk.AsSpan(0));
            var chunkBlocks = BinaryPrimitives.ReadUInt32LittleEndian(chunk.AsSpan(4));
            var totalSize = BinaryPrimitives.ReadUInt32LittleEndian(chunk.AsSpan(8));
            if (totalSize < chunkHeaderSize || cursor + totalSize > file.Length)
            {
                error = $"sparse chunk {index} 长度越界";
                return false;
            }

            if (type == CrcChunk)
            {
                if (totalSize != (uint)chunkHeaderSize + 4U)
                {
                    error = $"sparse CRC chunk {index} 长度错误";
                    return false;
                }
                cursor += totalSize;
                continue;
            }

            if (chunkBlocks == 0 || outputBlock + chunkBlocks > totalBlocks)
            {
                error = $"sparse chunk {index} 输出范围错误";
                return false;
            }

            var extent = new SparseExtent
            {
                OutputBlock = outputBlock,
                BlockCount = chunkBlocks,
            };

            if (type == RawChunk)
            {
                if (chunkBlocks > ulong.MaxValue / blockSize ||
                    totalSize != (ulong)chunkHeaderSize + (ulong)chunkBlocks * blockSize)
                {
                    error = $"sparse RAW chunk {index} 长度错误";
                    return false;
                }
                extent.Type = SparseExtentType.Raw;
                extent.FileOffset = (ulong)(cursor + chunkHeaderSize);
            }
            else if (type == FillChunk)
            {
                if (totalSize != (uint)chunkHeaderSize + 4U)
                {
                    error = $"sparse FILL chunk {index} 长度错误";
                    return false;
                }
                var fill = ReadExact(file, cursor + chunkHeaderSize, 4);
                if (fill.Length != 4)
                {
                    error = $"读取 sparse FILL chunk {index} 失败";
                    return false;
                }
                extent.FillValue = BinaryPrimitives.ReadUInt32LittleEndian(fill);
                extent.Type = SparseExtentType.Fill;
            }
            else if (type == SkipChunk)
            {
                if (totalSize != chunkHeaderSize)
                {
                    error = $"sparse DONT_CARE chunk {index} 长度错误";
                    return false;
                }
                extent.Type = SparseExtentType.Skip;
            }
            else
            {
                error = $"未知 sparse chunk 类型：{type:x4}";
                return false;
            }

            info.Extents.Add(extent);
            outputBlock += chunkBlocks;
            cursor += totalSize;
        }

        if (outputBlock != totalBlocks)
        {
            error = $"sparse 展开块数不匹配：{outputBlock} / {totalBlocks}";
            return false;
        }

        info.BlockSize = blockSize;
        info.TotalBlocks = totalBlocks;
        file.Seek(0, SeekOrigin.Begin);
        return true;
    }
}
