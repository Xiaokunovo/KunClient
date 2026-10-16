using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Text;

namespace KunFlashTool.Core.SwtEdl.Storage;

public sealed class SystemVersionInfo
{
    public string Brand { get; set; } = "";
    public string MarketName { get; set; } = "";
    public string Model { get; set; } = "";
    public string Device { get; set; } = "";
    public string Project { get; set; } = "";
    public string Platform { get; set; } = "";
    public string AndroidVersion { get; set; } = "";
    public string SystemVersion { get; set; } = "";
    public string OtaVersion { get; set; } = "";
    public string SecurityPatch { get; set; } = "";
    public string Region { get; set; } = "";
    public string BuildDate { get; set; } = "";
    public string Fingerprint { get; set; } = "";
    public List<string> FileSystems { get; } = new();                      // 对应 QStringList fileSystems
    public Dictionary<string, string> Properties { get; } = new();         // 对应 QHash<QString,QString>

    public bool IsEmpty => Properties.Count == 0;                          // 对应 isEmpty()
}

/// <summary>
/// 逐文件对应 `third_party/swt-edl/src/storage/SystemVersionReader.{h,cpp}`：
/// 从 physical 分区或 super 动态分区里解析 build.prop，填充 <see cref="SystemVersionInfo"/>。
/// </summary>
public static class SystemVersionReader
{
    /// <summary>对应 C++ 的 using PartitionByteReader = std::function&lt;QByteArray(quint64, quint32, QString*)&gt;</summary>
    public delegate byte[] PartitionByteReader(ulong offset, uint length, out string error);

    // ---- 字节层小工具（对应匿名 namespace 里的 le / fixedText 等） --------------------

    private static bool Le(byte[] data, long at, out ushort value)
    {
        value = 0;
        if (at < 0 || at + 2 > data.Length) return false;
        value = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan((int)at));
        return true;
    }

    private static bool Le(byte[] data, long at, out uint value)
    {
        value = 0;
        if (at < 0 || at + 4 > data.Length) return false;
        value = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan((int)at));
        return true;
    }

    private static bool Le(byte[] data, long at, out ulong value)
    {
        value = 0;
        if (at < 0 || at + 8 > data.Length) return false;
        value = BinaryPrimitives.ReadUInt64LittleEndian(data.AsSpan((int)at));
        return true;
    }

    /// <summary>对应 QByteArray::mid(at, len)：len 为负表示“到结尾”。</summary>
    private static byte[] Mid(byte[] data, long at, long len)
    {
        if (at < 0 || at >= data.Length) return Array.Empty<byte>();
        if (len < 0 || at + len > data.Length) len = data.Length - at;
        var result = new byte[len];
        Array.Copy(data, at, result, 0, len);
        return result;
    }

    private static byte[] Concat(byte[] a, byte[] b)
    {
        var result = new byte[a.Length + b.Length];
        Array.Copy(a, 0, result, 0, a.Length);
        Array.Copy(b, 0, result, a.Length, b.Length);
        return result;
    }

    private static bool IsAsciiSpace(byte b) =>
        b == (byte)' ' || b == (byte)'\t' || b == (byte)'\n' ||
        b == (byte)'\r' || b == (byte)'\v' || b == (byte)'\f';

    /// <summary>对应 QByteArray::trimmed()（只去 ASCII 空白，不去 '\0'）。</summary>
    private static byte[] TrimBytes(byte[] data)
    {
        var start = 0;
        var end = data.Length;
        while (start < end && IsAsciiSpace(data[start])) ++start;
        while (end > start && IsAsciiSpace(data[end - 1])) --end;
        if (start == 0 && end == data.Length) return data;
        return Mid(data, start, end - start);
    }

    private static List<byte[]> SplitByte(byte[] data, byte separator)
    {
        var result = new List<byte[]>();
        var start = 0;
        for (var i = 0; i < data.Length; ++i)
        {
            if (data[i] == separator)
            {
                result.Add(Mid(data, start, i - start));
                start = i + 1;
            }
        }
        result.Add(Mid(data, start, data.Length - start));
        return result;
    }

    private static string Latin1(byte[] data) => Encoding.Latin1.GetString(data);

    private static string Utf8(byte[] data) => Encoding.UTF8.GetString(data);

    private static string Utf8(byte[] data, long at, long length)
    {
        if (length <= 0) return string.Empty;
        return Encoding.UTF8.GetString(data, (int)at, (int)length);
    }

    private static bool Contains(byte[] haystack, string needle)
    {
        var bytes = new byte[needle.Length];
        for (var i = 0; i < needle.Length; ++i) bytes[i] = (byte)needle[i];
        return haystack.AsSpan().IndexOf(bytes) >= 0;
    }

    private static string[] SplitSkipEmpty(string path) =>
        path.Split('/', StringSplitOptions.RemoveEmptyEntries);

    private static string FixedText(byte[] data, long at, long count)
    {
        if (at < 0 || count <= 0 || at + count > data.Length) return string.Empty;
        var text = Mid(data, at, count);
        var zero = Array.IndexOf(text, (byte)0);
        if (zero >= 0) text = Mid(text, 0, zero);
        return Latin1(text).Trim();
    }

    // ---- 分区字节读取器（对应匿名 namespace 的 Reader） ------------------------------

    private sealed class Reader
    {
        private readonly PartitionByteReader m_callback;
        private readonly ulong m_size;
        private string m_error = string.Empty;

        public Reader(PartitionByteReader callback, ulong size)
        {
            m_callback = callback;
            m_size = size;
        }

        public byte[] Read(ulong offset, uint length)
        {
            if (m_callback is null || length == 0 || offset > m_size || length > m_size - offset)
                return Array.Empty<byte>();
            var bytes = m_callback(offset, length, out var error);
            if (bytes is null || (ulong)bytes.Length != length)
            {
                if (!string.IsNullOrEmpty(error)) m_error = error;
                return Array.Empty<byte>();
            }
            return bytes;
        }

        public ulong Size => m_size;
        public string Error => m_error;
    }

    private sealed class Ext4Reader
    {
        private readonly Reader m_reader;
        private uint m_blockSize;
        private uint m_inodeSize;
        private uint m_inodesPerGroup;
        private uint m_descSize;
        private bool m_extents;
        private bool m_64bit;
        private bool m_valid;

        public Ext4Reader(Reader reader)
        {
            m_reader = reader;
            var sb = m_reader.Read(1024, 256);
            ushort magic = 0; uint logBlock = 0, incompat = 0; ushort inodeSize = 0; uint inodesPerGroup = 0;
            if (!Le(sb, 56, out magic) || magic != 0xef53 || !Le(sb, 24, out logBlock) || logBlock > 6 ||
                !Le(sb, 40, out inodesPerGroup) || !Le(sb, 88, out inodeSize) ||
                !Le(sb, 96, out incompat)) return;
            m_inodesPerGroup = inodesPerGroup;
            m_blockSize = 1024U << (int)logBlock;
            m_inodeSize = inodeSize != 0 ? (uint)inodeSize : 128U;
            m_extents = (incompat & 0x40U) != 0;
            m_64bit = (incompat & 0x80U) != 0;
            ushort descSize = 0;
            if (m_64bit) Le(sb, 254, out descSize);
            m_descSize = Math.Max(32U, (uint)descSize);
            m_valid = m_inodesPerGroup > 0 && m_inodeSize >= 128;
        }

        public bool Valid => m_valid;

        public byte[] File(string path)
        {
            uint inode = 2;
            foreach (var part in SplitSkipEmpty(path))
            {
                var directory = InodeData(inode);
                // C++ 这里判 isNull()：失败与空内容在下游都会导致返回空，这里统一用零长数组表示。
                if (directory.Length == 0) return Array.Empty<byte>();
                inode = FindEntry(directory, part);
                if (inode == 0) return Array.Empty<byte>();
            }
            return InodeData(inode);
        }

        private byte[] InodeBytes(uint number)
        {
            if (number == 0) return Array.Empty<byte>();
            var group = (number - 1) / m_inodesPerGroup;
            var index = (number - 1) % m_inodesPerGroup;
            var descAt = (m_blockSize == 1024 ? 2UL * m_blockSize : m_blockSize) +
                         (ulong)group * m_descSize;
            var desc = m_reader.Read(descAt, m_descSize);
            if (!Le(desc, 8, out uint tableLo)) return Array.Empty<byte>();
            uint tableHi = 0;
            if (m_64bit && m_descSize >= 64) Le(desc, 40, out tableHi);
            var table = tableLo | ((ulong)tableHi << 32);
            return m_reader.Read(table * m_blockSize + (ulong)index * m_inodeSize, m_inodeSize);
        }

        private byte[] InodeData(uint number)
        {
            var inode = InodeBytes(number);
            if (!Le(inode, 4, out uint lo) || !Le(inode, 108, out uint hi) || !Le(inode, 32, out uint flags))
                return Array.Empty<byte>();
            var size = lo | ((ulong)hi << 32);
            if (size > 64UL * 1024UL * 1024UL) return Array.Empty<byte>();
            if (size == 0) return Array.Empty<byte>();
            var blocks = Mid(inode, 40, 60);
            if (m_extents && (flags & 0x80000U) != 0) return ExtentData(blocks, size);
            var result = new List<byte>((int)size);
            for (int index = 0; index < 12 && (ulong)result.Count < size; ++index)
            {
                if (!Le(blocks, index * 4, out uint block) || block == 0) break;
                var count = (uint)Math.Min((ulong)m_blockSize, size - (ulong)result.Count);
                var data = m_reader.Read((ulong)block * m_blockSize, count);
                if ((ulong)data.Length != count) return Array.Empty<byte>();
                result.AddRange(data);
            }
            return (ulong)result.Count == size ? result.ToArray() : Array.Empty<byte>();
        }

        private bool AppendExtentNode(byte[] node, ulong maximum, List<byte> output)
        {
            if (!Le(node, 0, out ushort magic) || magic != 0xf30a || !Le(node, 2, out ushort entries) ||
                !Le(node, 6, out ushort depth)) return false;
            for (ushort index = 0; index < entries && (ulong)output.Count < maximum; ++index)
            {
                var at = 12 + index * 12;
                if (depth == 0)
                {
                    if (!Le(node, at + 4, out ushort length) || !Le(node, at + 6, out ushort startHi) ||
                        !Le(node, at + 8, out uint startLo)) return false;
                    var block = startLo | ((ulong)startHi << 32);
                    var blocks = length > 0x8000 ? (uint)(length - 0x8000) : length;
                    for (uint b = 0; b < blocks && (ulong)output.Count < maximum; ++b)
                    {
                        var count = (uint)Math.Min((ulong)m_blockSize, maximum - (ulong)output.Count);
                        var data = m_reader.Read((block + b) * m_blockSize, count);
                        if ((ulong)data.Length != count) return false;
                        output.AddRange(data);
                    }
                }
                else
                {
                    if (!Le(node, at + 4, out uint leafLo) || !Le(node, at + 8, out ushort leafHi)) return false;
                    var leaf = leafLo | ((ulong)leafHi << 32);
                    if (!AppendExtentNode(m_reader.Read(leaf * m_blockSize, m_blockSize), maximum, output))
                        return false;
                }
            }
            return true;
        }

        private byte[] ExtentData(byte[] root, ulong size)
        {
            var result = new List<byte>((int)size);
            if (!AppendExtentNode(root, size, result) || (ulong)result.Count != size) return Array.Empty<byte>();
            return result.ToArray();
        }

        private static uint FindEntry(byte[] data, string name)
        {
            long pos = 0;
            while (pos + 8 <= data.Length)
            {
                uint inode = 0; ushort rec = 0;
                if (!Le(data, pos, out inode) || !Le(data, pos + 4, out rec) ||
                    rec < 8 || pos + rec > data.Length) break;
                var length = data[pos + 6];
                if (inode != 0 && length != 0 && pos + 8 + length <= data.Length &&
                    Utf8(data, pos + 8, length) == name) return inode;
                pos += rec;
            }
            return 0;
        }
    }

    private sealed class ErofsReader
    {
        private readonly Reader m_reader;
        private uint m_blockSize;
        private uint m_meta;
        private ushort m_root;
        private int m_bits;
        private bool m_zeroPadding;
        private bool m_valid;

        public ErofsReader(Reader reader)
        {
            m_reader = reader;
            var sb = m_reader.Read(1024, 128);
            if (!Le(sb, 0, out uint magic) || magic != 0xe0f5e1e2U || sb.Length < 81) return;
            m_bits = sb[12];
            if (m_bits < 9 || m_bits > 16) return;
            m_blockSize = 1U << m_bits;
            Le(sb, 14, out m_root); Le(sb, 40, out m_meta);
            Le(sb, 80, out uint incompat); m_zeroPadding = (incompat & 1U) != 0;
            m_valid = true;
        }

        public bool Valid => m_valid;

        public byte[] File(string path)
        {
            ulong nid = m_root;
            foreach (var part in SplitSkipEmpty(path))
            {
                var directory = NidData(nid);
                // C++ 这里判 isNull()：失败与空内容在下游都会导致返回空，这里统一用零长数组表示。
                if (directory.Length == 0) return Array.Empty<byte>();
                nid = FindEntry(directory, part);
                if (nid == ulong.MaxValue) return Array.Empty<byte>();
            }
            return NidData(nid);
        }

        private struct CompactEntry
        {
            public int Type;            // -1 表示失败（对应 C++ 默认成员初始化）
            public int ClusterOffset;
            public long PhysicalBlock;
            public ulong NextPackOffset;

            public static CompactEntry Invalid => new() { Type = -1, PhysicalBlock = -1 };
        }

        private static (int Low, int Type) DecodeCompactBits(int lowBits, byte[] data, int bitPosition)
        {
            var byteOffset = bitPosition / 8;
            var bitOffset = bitPosition & 7;
            uint value = 0;
            for (int index = 0; index < 4 && byteOffset + index < data.Length; ++index)
            {
                value |= (uint)data[byteOffset + index] << (index * 8);
            }
            value >>= bitOffset;
            var mask = lowBits == 32 ? uint.MaxValue : ((1U << lowBits) - 1U);
            return ((int)(value & mask), (int)((value >> lowBits) & 3U));
        }

        private CompactEntry CompactEntryAt(ulong baseOffset, int logicalCluster, int total,
                                            int clusterBits, bool compacted2Byte, bool bigCluster)
        {
            var initial4Byte = (int)(((32UL - baseOffset % 32UL) / 4UL) & 7UL);
            var compacted2ByteCount = 0;
            if (compacted2Byte && initial4Byte < total)
                compacted2ByteCount = (total - initial4Byte) / 16 * 16;
            var position = baseOffset;
            var shift = 2;
            var adjusted = logicalCluster;
            if (adjusted >= initial4Byte)
            {
                position += (ulong)initial4Byte * 4UL;
                adjusted -= initial4Byte;
                if (adjusted < compacted2ByteCount) shift = 1;
                else
                {
                    position += (ulong)compacted2ByteCount * 2UL;
                    adjusted -= compacted2ByteCount;
                }
            }
            position += (ulong)adjusted * (1UL << shift);
            var count = ((1 << shift) == 4 && clusterBits <= 14) ? 2
                : ((1 << shift) == 2 && clusterBits <= 12) ? 16 : 0;
            if (count == 0) return CompactEntry.Invalid;
            var packSize = count << shift;
            var bytesInPack = (int)(position & (ulong)(packSize - 1));
            var packStart = position - (ulong)bytesInPack;
            var item = bytesInPack >> shift;
            var pack = m_reader.Read(packStart, (uint)packSize);
            if (pack.Length != packSize) return CompactEntry.Invalid;
            var lowBits = Math.Max(clusterBits, 12);
            var encodedBits = (packSize - 4) * 8 / count;
            var (low, type) = DecodeCompactBits(lowBits, pack, encodedBits * item);
            if (type == 2)
                return new CompactEntry { Type = type, ClusterOffset = 1 << clusterBits, PhysicalBlock = -1,
                                          NextPackOffset = packStart + (ulong)packSize };
            var blocks = bigCluster ? 0 : 1;
            var cursor = item;
            while (cursor > 0)
            {
                --cursor;
                var (previousLow, previousType) = DecodeCompactBits(lowBits, pack, encodedBits * cursor);
                if (previousType == 2)
                {
                    if (bigCluster && (previousLow & 2048) != 0)
                    {
                        --cursor;
                        blocks += previousLow & ~2048;
                    }
                    else if (bigCluster)
                    {
                        if (previousLow <= 1) break;
                        cursor -= previousLow - 2;
                    }
                    else
                    {
                        cursor -= previousLow;
                    }
                }
                if (!bigCluster && cursor >= 0) ++blocks;
                else if (bigCluster && previousType != 2) ++blocks;
            }
            if (!Le(pack, packSize - 4, out uint firstBlock)) return CompactEntry.Invalid;
            return new CompactEntry { Type = type, ClusterOffset = low,
                                      PhysicalBlock = (long)firstBlock + blocks,
                                      NextPackOffset = packStart + (ulong)packSize };
        }

        private static int Lz4(byte[] source, int sourceAt, int sourceLength,
                               byte[] target, int targetAt, int targetLength)
        {
            int s = sourceAt, d = targetAt, sEnd = sourceAt + sourceLength,
                dEnd = targetAt + targetLength;
            while (s < sEnd && d < dEnd)
            {
                var token = source[s++];
                int literal = token >> 4, match = token & 15;
                if (literal == 15)
                    while (s < sEnd) { var v = source[s++]; literal += v; if (v != 255) break; }
                if (s + literal > sEnd || d + literal > dEnd) return d - targetAt;
                Array.Copy(source, s, target, d, literal); s += literal; d += literal;
                if (s + 2 > sEnd) break;
                var distance = source[s] | (source[s + 1] << 8); s += 2;
                if (distance == 0 || d - distance < targetAt) return -1;
                match += 4;
                if (match == 19)
                    while (s < sEnd) { var v = source[s++]; match += v; if (v != 255) break; }
                var count = Math.Min(match, dEnd - d);
                for (var i = 0; i < count; ++i) target[d + i] = target[d - distance + i];
                d += count;
            }
            return d - targetAt;
        }

        private ulong InodeAt(ulong nid) => (ulong)m_meta * m_blockSize + nid * 32UL;

        private byte[] NidData(ulong nid)
        {
            var at = InodeAt(nid);
            var hdr = m_reader.Read(at, 64);
            if (!Le(hdr, 0, out ushort format) || !Le(hdr, 2, out ushort xattrs) ||
                !Le(hdr, 16, out uint rawBlock)) return Array.Empty<byte>();
            var extended = (format & 1U) != 0;
            var layout = (format >> 1) & 7;
            ulong size = 0;
            if (extended) Le(hdr, 8, out size);
            else { Le(hdr, 8, out uint s); size = s; }
            if (size > 64UL * 1024UL * 1024UL) return Array.Empty<byte>();
            if (size == 0) return Array.Empty<byte>();
            var inodeSize = extended ? 64U : 32U;
            var xattrSize = xattrs != 0 ? (uint)(12 + (xattrs - 1) * 4) : 0U;
            var inlineAt = at + inodeSize + xattrSize;
            if (layout == 0 || layout == 4)
                return m_reader.Read((ulong)rawBlock * m_blockSize, (uint)size);
            if (layout == 2)
            {
                var tail = (uint)(size % m_blockSize);
                var blockBytes = size - tail;
                var result = Array.Empty<byte>();
                if (blockBytes != 0)
                    result = m_reader.Read((ulong)rawBlock * m_blockSize, (uint)blockBytes);
                if (tail != 0) result = Concat(result, m_reader.Read(inlineAt, tail));
                return (ulong)result.Length == size ? result : Array.Empty<byte>();
            }
            if (layout != 1 && layout != 3) return Array.Empty<byte>();
            // Small build.prop files commonly use EROFS inline tail packing.
            var map = m_reader.Read(inlineAt, 8);
            if (!Le(map, 2, out ushort inlineSize) || !Le(map, 4, out ushort advise) ||
                map.Length < 8 || inlineSize == 0) return Array.Empty<byte>();
            var clusterBits = m_bits + (map[7] & 15);
            var indexes = (inlineAt + 15UL) & ~7UL;
            var totalIndexes = (int)((size + (1UL << clusterBits) - 1) >> clusterBits);
            var inlineData = indexes + (ulong)totalIndexes * 8UL;
            var tailType = 1;
            if (layout == 3)
            {
                var tail = CompactEntryAt(indexes, Math.Max(0, totalIndexes - 1), totalIndexes, clusterBits,
                                          (advise & 1U) != 0, (advise & 10U) != 0);
                if (tail.Type < 0 || tail.NextPackOffset == 0) return Array.Empty<byte>();
                inlineData = tail.NextPackOffset;
                tailType = tail.Type;
            }
            var compressed = m_reader.Read(inlineData, inlineSize);
            var output = new byte[(int)size];
            if (tailType == 0 && compressed.Length >= output.Length)
                return Mid(compressed, 0, output.Length);
            var sourceOffset = 0;
            if (m_zeroPadding)
            {
                while (sourceOffset < compressed.Length && compressed[sourceOffset] == 0)
                    ++sourceOffset;
            }
            if (sourceOffset < compressed.Length &&
                Lz4(compressed, sourceOffset, compressed.Length - sourceOffset,
                    output, 0, output.Length) >= output.Length) return output;
            if (compressed.Length >= output.Length) return Mid(compressed, 0, output.Length);
            return Array.Empty<byte>();
        }

        private ulong FindEntry(byte[] data, string name)
        {
            long pos = 0;
            while (pos < data.Length)
            {
                var start = pos / m_blockSize * m_blockSize;
                var end = Math.Min(start + m_blockSize, data.Length);
                if (start + 12 > end) break;
                if (!Le(data, start + 8, out ushort firstName) || firstName < 12 || start + firstName > end)
                {
                    pos = end;
                    continue;
                }
                var count = firstName / 12;
                for (var i = 0; i < count; ++i)
                {
                    var d = start + i * 12;
                    if (!Le(data, d, out ulong nid) || !Le(data, d + 8, out ushort nameAt)) continue;
                    ushort nextAt;
                    if (i + 1 < count) Le(data, start + (i + 1) * 12 + 8, out nextAt);
                    else nextAt = (ushort)(end - start);
                    var n = Mid(data, start + nameAt, nextAt - nameAt);
                    while (n.Length > 0 && n[^1] == 0) n = Mid(n, 0, n.Length - 1);
                    if (Utf8(n) == name) return nid;
                }
                pos = end;
            }
            return ulong.MaxValue;
        }
    }

    // ---- 属性解析（对应匿名 namespace 的 parseProps / readProperties / prop / fill） ----

    private static Dictionary<string, string> ParseProps(byte[] bytes)
    {
        var props = new Dictionary<string, string>();
        foreach (var raw in SplitByte(bytes, (byte)'\n'))
        {
            var line = TrimBytes(raw);
            if (line.Length == 0 || line[0] == (byte)'#') continue;
            var equals = Array.IndexOf(line, (byte)'=');
            if (equals > 0)
            {
                props[Utf8(line, 0, equals).Trim().ToLowerInvariant()] =
                    Utf8(line, equals + 1, line.Length - equals - 1).Trim();
            }
        }
        return props;
    }

    private static Dictionary<string, string> ReadProperties(Reader reader, ref string fileSystem)
    {
        string[] paths = { "build.prop", "etc/build.prop", "system/build.prop", "system/etc/build.prop" };
        var extMagic = reader.Read(1080, 2);
        Le(extMagic, 0, out ushort em);
        if (em == 0xef53)
        {
            var ext4 = new Ext4Reader(reader);
            if (ext4.Valid)
                foreach (var path in paths)
                {
                    var data = ext4.File(path);
                    if (data.Length != 0) { fileSystem = "EXT4"; return ParseProps(data); }
                }
        }
        var eroMagic = reader.Read(1024, 4);
        Le(eroMagic, 0, out uint erm);
        if (erm == 0xe0f5e1e2U)
        {
            var erofs = new ErofsReader(reader);
            if (erofs.Valid)
                foreach (var path in paths)
                {
                    var data = erofs.File(path);
                    if (data.Length != 0) { fileSystem = "EROFS"; return ParseProps(data); }
                }
        }
        // Vendor images sometimes store a plain property block outside a mounted FS.
        var scanBytes = Math.Min(reader.Size, 4UL * 1024UL * 1024UL);
        for (ulong at = 0; at < scanBytes; at += 1024 * 1024)
        {
            var data = reader.Read(at, (uint)Math.Min(1024UL * 1024UL, scanBytes - at));
            if (Contains(data, "ro.build.version.release=") || Contains(data, "ro.product.brand="))
            {
                fileSystem = "RAW";
                return ParseProps(data);
            }
        }
        return new Dictionary<string, string>();
    }

    private static string Prop(Dictionary<string, string> props, params string[] keys)
    {
        foreach (var key in keys)
        {
            if (props.TryGetValue(key, out var value))
            {
                var trimmed = value.Trim();
                if (trimmed.Length != 0) return trimmed;
            }
        }
        return string.Empty;
    }

    private static void Fill(SystemVersionInfo result)
    {
        var p = result.Properties;
        result.Brand = Prop(p, "ro.product.brand", "ro.product.odm.brand", "ro.product.vendor.brand");
        result.MarketName = Prop(p, "ro.vendor.oplus.market.enname", "ro.vendor.oplus.market.name", "ro.config.marketing_name");
        result.Model = Prop(p, "ro.product.model", "ro.product.odm.model", "ro.product.vendor.model");
        result.Device = Prop(p, "ro.product.device", "ro.product.odm.device", "ro.vendor.product.device.oem");
        result.Project = Prop(p, "ro.separate.soft", "ro.vendor.product.oem");
        result.Platform = Prop(p, "ro.board.platform", "ro.product.oplus.cpuinfo");
        result.AndroidVersion = Prop(p, "ro.build.version.release", "ro.product.build.version.release", "ro.build.version.release_or_codename");
        result.SystemVersion = Prop(p, "ro.build.display.id.show", "ro.build.display.id", "ro.build.version.oplusrom", "ro.build.version.oplusrom.display");
        result.OtaVersion = Prop(p, "ro.build.version.ota", "ro.build.display.ota");
        result.SecurityPatch = Prop(p, "ro.build.version.security_patch", "ro.vendor.build.security_patch");
        result.Region = Prop(p, "ro.vendor.oplus.regionmark", "ro.oplus.image.system_ext.area");
        result.BuildDate = Prop(p, "ro.build.date", "ro.product.build.date", "ro.system.build.date");
        result.Fingerprint = Prop(p, "ro.build.fingerprint", "ro.product.build.fingerprint", "ro.system.build.fingerprint", "ro.vendor.build.fingerprint");
    }

    // ---- super 动态分区（对应匿名 namespace 的 LpExtent / LpPart / parseLp / baseName） --

    private sealed class LpExtent
    {
        public ulong Sectors;
        public ulong PhysicalSector;
        public uint Type;
    }

    private sealed class LpPart
    {
        public string Name = "";
        public List<LpExtent> Extents = new();
        public ulong Sectors;
    }

    private static List<LpPart> ParseLp(Reader super)
    {
        var geometry = super.Read(4096, 4096);
        if (!Le(geometry, 0, out uint gm) || gm != 0x616c4467U ||
            !Le(geometry, 40, out uint maxMeta) || maxMeta < 128 || maxMeta > 4 * 1024 * 1024)
            return new List<LpPart>();
        var meta = super.Read(12288, maxMeta);
        if (!Le(meta, 0, out uint mm) || mm != 0x414c5030U ||
            !Le(meta, 8, out uint header) || header < 128 || header > (uint)meta.Length)
            return new List<LpPart>();
        if (!Le(meta, 80, out uint po) || !Le(meta, 84, out uint pn) || !Le(meta, 88, out uint ps) ||
            !Le(meta, 92, out uint eo) || !Le(meta, 96, out uint en) || !Le(meta, 100, out uint es) ||
            ps < 52 || es < 24) return new List<LpPart>();
        var all = new List<LpExtent>((int)en);
        for (uint i = 0; i < en; ++i)
        {
            var at = header + eo + (long)i * es;
            var e = new LpExtent();
            if (!Le(meta, at, out e.Sectors) || !Le(meta, at + 8, out e.Type) ||
                !Le(meta, at + 12, out e.PhysicalSector) || !Le(meta, at + 20, out uint source))
                return new List<LpPart>();
            all.Add(e);
        }
        var parts = new List<LpPart>();
        for (uint i = 0; i < pn; ++i)
        {
            var at = header + po + (long)i * ps;
            if (at + 52 > meta.Length || !Le(meta, at + 40, out uint first) ||
                !Le(meta, at + 44, out uint count) || first + count > (uint)all.Count)
                return new List<LpPart>();
            var part = new LpPart { Name = FixedText(meta, at, 36) };
            for (uint x = 0; x < count; ++x)
            {
                part.Extents.Add(all[(int)(first + x)]);
                part.Sectors += all[(int)(first + x)].Sectors;
            }
            if (part.Sectors != 0) parts.Add(part);
        }
        return parts;
    }

    private static string BaseName(string name)
    {
        name = name.ToLowerInvariant();
        if (name.EndsWith("_a", StringComparison.Ordinal) || name.EndsWith("_b", StringComparison.Ordinal))
            name = name[..^2];
        return name;
    }

    // ---- 公开入口 ------------------------------------------------------------------

    public static bool ReadPhysical(PartitionByteReader reader, ulong partitionBytes,
                                    SystemVersionInfo result, out string error)
    {
        error = string.Empty;
        if (result is null) return false;
        var r = new Reader(reader, partitionBytes);
        var fileSystem = string.Empty;
        var props = ReadProperties(r, ref fileSystem);
        if (props.Count == 0)
        {
            error = r.Error.Length == 0 ? "分区中未识别到 build.prop" : r.Error;
            return false;
        }
        foreach (var pair in props)
        {
            if (!result.Properties.ContainsKey(pair.Key)) result.Properties[pair.Key] = pair.Value;
        }
        if (fileSystem.Length != 0 && !result.FileSystems.Contains(fileSystem)) result.FileSystems.Add(fileSystem);
        Fill(result);
        return true;
    }

    public static bool ReadSuper(PartitionByteReader superReader, ulong superBytes,
                                 SystemVersionInfo result, out string error,
                                 Action<string>? status = null)
    {
        error = string.Empty;
        if (result is null) return false;
        var super = new Reader(superReader, superBytes);
        var parts = ParseLp(super);
        if (parts.Count == 0)
        {
            error = "super 动态分区元数据解析失败";
            return false;
        }
        string[] priority = { "my_manifest", "odm", "my_product", "vendor", "product", "system_ext", "system" };
        foreach (var wanted in priority)
        {
            foreach (var part in parts)
            {
                if (BaseName(part.Name) != wanted) continue;
                status?.Invoke($"解析 {part.Name} 版本信息…");
                var logicalSize = part.Sectors * 512UL;
                PartitionByteReader logicalReader = (ulong offset, uint length, out string readError) =>
                {
                    readError = string.Empty;
                    var outBytes = new List<byte>((int)length);
                    ulong logical = 0;
                    var current = offset;
                    var remaining = length;
                    foreach (var extent in part.Extents)
                    {
                        var extentBytes = extent.Sectors * 512UL;
                        if (current >= logical + extentBytes) { logical += extentBytes; continue; }
                        var within = current - logical;
                        var take = (uint)Math.Min((ulong)remaining, extentBytes - within);
                        if (extent.Type == 1) outBytes.AddRange(new byte[take]);
                        else if (extent.Type == 0)
                        {
                            var data = superReader(extent.PhysicalSector * 512UL + within, take, out readError);
                            if (data is null || (ulong)data.Length != take) return Array.Empty<byte>();
                            outBytes.AddRange(data);
                        }
                        else return Array.Empty<byte>();
                        current += take;
                        remaining -= take;
                        logical += extentBytes;
                        if (remaining == 0) break;
                    }
                    return remaining != 0 ? Array.Empty<byte>() : outBytes.ToArray();
                };
                var one = new SystemVersionInfo();
                if (ReadPhysical(logicalReader, logicalSize, one, out _))
                {
                    foreach (var pair in one.Properties)
                    {
                        if (!result.Properties.ContainsKey(pair.Key)) result.Properties[pair.Key] = pair.Value;
                    }
                    foreach (var fs in one.FileSystems)
                    {
                        if (!result.FileSystems.Contains(fs)) result.FileSystems.Add(fs);
                    }
                }
            }
        }
        if (result.Properties.Count == 0)
        {
            error = "动态分区中未读取到 build.prop";
            return false;
        }
        Fill(result);
        return true;
    }
}
