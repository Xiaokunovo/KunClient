using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Text;

namespace KunFlashTool.Core.SwtEdl;

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/storage/GptParser.cpp`：从一段 LBA0 起始的镜像里解析 GPT 分区表。
/// </summary>
public static class GptParser
{
    private static readonly byte[] Signature = Encoding.ASCII.GetBytes("EFI PART");

    private static uint U32(byte[] data, long offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan((int)offset));

    private static ulong U64(byte[] data, long offset) =>
        BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan((int)offset));

    private static bool AllZero(ReadOnlySpan<byte> bytes)
    {
        foreach (var b in bytes)
        {
            if (b != 0) return false;
        }
        return true;
    }

    /// <summary>
    /// GPT 里 GUID 的前 3 段按**小端**存放（Data1 4 字节 / Data2 2 字节 / Data3 2 字节），后 8 字节顺序存放。
    ///
    /// <para>🔴 这里**不能**照抄 C++ 的反转：C++ 侧是 <c>std::reverse(...)</c> 之后再喂给
    /// <c>QUuid::fromRfc4122()</c>，而 <c>fromRfc4122</c> 吃的是**大端** RFC 序，所以那边反转是对的。
    /// .NET 的 <c>new Guid(ReadOnlySpan&lt;byte&gt;)</c> 用的却是 <c>ToByteArray()</c> 序 ——
    /// 也就是前 3 段小端，正好等于 GPT 的磁盘序。照抄反转 = 反转两次，
    /// 得到的会是 <c>A2A0D0EB-E5B9-3344-…</c> 这种前后颠倒的串（2026-10-09 单测实测）。</para>
    /// </summary>
    private static string GptGuid(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length != 16) return string.Empty;
        // 等价写法：new Guid(bytes, bigEndian: true) 需要先把前 3 段反转；这里直接用小端构造，更短也更不容易写反。
        return new Guid(bytes).ToString("D").ToUpperInvariant();
    }

    /// <summary>分区名是 UTF-16LE，遇 0 结束。</summary>
    private static string Utf16Name(ReadOnlySpan<byte> bytes)
    {
        var sb = new StringBuilder(bytes.Length / 2);
        for (var i = 0; i + 1 < bytes.Length; i += 2)
        {
            var value = BinaryPrimitives.ReadUInt16LittleEndian(bytes.Slice(i, 2));
            if (value == 0) break;
            sb.Append((char)value);
        }
        return sb.ToString().Trim();
    }

    public static bool Parse(byte[] data, uint sectorSize, uint lun,
                             List<PartitionInfo> partitions, out string error)
    {
        error = string.Empty;
        if (partitions == null || sectorSize < 512 || data.Length < (long)(sectorSize * 2UL))
        {
            error = "GPT 数据长度不足";
            return false;
        }

        var header = (long)sectorSize;
        if (!data.AsSpan((int)header, 8).SequenceEqual(Signature))
        {
            error = "未找到 GPT 头签名";
            return false;
        }

        var headerSize = U32(data, header + 12);
        var entryLba = U64(data, header + 72);
        var entryCount = U32(data, header + 80);
        var entrySize = U32(data, header + 84);
        if (headerSize < 92 || headerSize > sectorSize || entrySize < 128 || entrySize > 4096 ||
            entryCount == 0 || entryCount > 4096)
        {
            error = "GPT 头字段异常";
            return false;
        }

        var tableOffset = entryLba * sectorSize;
        var tableBytes = (ulong)entryCount * entrySize;
        if (tableOffset > (ulong)data.Length || tableBytes > (ulong)data.Length - tableOffset)
        {
            error = "GPT 分区项超出已读取范围";
            return false;
        }

        partitions.Clear();
        for (uint index = 0; index < entryCount; ++index)
        {
            var offset = (long)(tableOffset + (ulong)index * entrySize);
            // ⚠️ AsSpan 只有 (int) / (int,int) 两个重载，传 long 会静默选错重载并报出
            //    「byte[] 不包含 AsSpan 的定义」这种误导性错误 —— 这里必须显式转 int。
            var entry = data.AsSpan((int)offset, (int)entrySize);
            if (AllZero(entry.Slice(0, 16))) continue;

            var partition = new PartitionInfo
            {
                TypeGuid = GptGuid(entry.Slice(0, 16)),
                UniqueGuid = GptGuid(entry.Slice(16, 16)),
                FirstLba = U64(data, offset + 32),
                LastLba = U64(data, offset + 40),
                Attributes = U64(data, offset + 48),
                Name = Utf16Name(entry.Slice(56, (int)Math.Min(72, entrySize - 56))),
                Lun = lun,
            };
            if (partition.LastLba >= partition.FirstLba) partitions.Add(partition);
        }

        if (partitions.Count == 0)
        {
            error = "GPT 中没有有效分区项";
            return false;
        }
        return true;
    }
}
