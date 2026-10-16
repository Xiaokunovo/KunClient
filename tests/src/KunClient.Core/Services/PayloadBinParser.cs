using System;
using System.Collections.Generic;
using System.IO;

namespace KunFlashTool.Core.Services;

/// <summary>
/// Android OTA payload.bin（DeltaArchivePayload）解析器。
///
/// <para>
/// 2026-10-02 新增：PayloadPage 原先的「8 字节 magic + headerSize + entryCount」是自造格式，
/// 真实的 payload.bin 一律是 <c>"CrAU"</c> 魔数 + 大端头 + <b>protobuf manifest</b>
/// （AOSP update_metadata.proto DeltaArchiveManifest），所以真机包从来解析不出来，
/// 提取必然落到「名称扫描」兜底（只有名字没有偏移 → 全部跳过）。
/// </para>
///
/// <para>
/// 支持范围与 payload-dumper 同口径：**整包（full）载荷里的原始段**。
/// 一个分区可提取 ⇔ 它的全部算子 ∈ {REPLACE, ZERO, DISCARD}（REPLACE 直接拷贝数据段，
/// ZERO/DISCARD 目标区域保持全零）。增量算子（SOURCE_COPY / BSDIFF / PUFFDIFF / 压缩类）
/// 需要旧分区或解压逻辑，标记为不支持并如实告知。
/// </para>
/// </summary>
public static class PayloadBinParser
{
    /// <summary>"CrAU" 按**小端**读进 u32 的值（文件里字节序是 0x43 0x72 0x41 0x55）。</summary>
    public const uint CrAuMagicLe = 0x55417243;

    /// <summary>块大小（manifest 未声明时按 AOSP 默认 4096）。</summary>
    public const ulong DefaultBlockSize = 4096;

    /// <summary>一个可提取的数据段：payload 文件里的 [FileOffset, +Length) → 输出镜像的 [OutOffset, +Length)。</summary>
    public readonly record struct PayloadSegment(ulong FileOffset, ulong OutOffset, ulong Length);

    public sealed class PayloadPartition
    {
        public required string Name;
        /// <summary>输出镜像总大小（由 dst extents 的最大末端 × block_size 得出）。</summary>
        public ulong SizeBytes;
        /// <summary>全部数据段（已按 OutOffset 排序）。可提取 = 全部算子都支持。</summary>
        public List<PayloadSegment> Segments { get; } = new();
        /// <summary>false = 含不支持的算子（增量 / 压缩类），提取时应跳过并说明原因。</summary>
        public bool Extractable { get; internal set; } = true;
        public string UnsupportedReason { get; internal set; } = "";
    }

    public sealed class PayloadBinInfo
    {
        public uint FormatVersion;
        public ulong BlockSize = DefaultBlockSize;
        public List<PayloadPartition> Partitions { get; } = new();
    }

    /// <summary>解析失败时抛 FormatException，message 说明原因。</summary>
    public static PayloadBinInfo Parse(Stream stream)
    {
        using var br = new BinaryReaderBigEndian(stream ?? throw new ArgumentNullException(nameof(stream)), leaveOpen: true);

        // 🔴 魔数必须按**小端**读：它在文件里就是 4 个 ASCII 字节 "CrAU"（0x43 0x72 0x41 0x55），
        //    按大端读会得到 0x43724155，与 CrAuMagicLe(0x55417243) 永远不相等 ——
        //    结果是**任何** payload.bin 都被判成"不是 payload.bin"。
        //    后面的 version / manifestSize 才是大端（那是真正的整数域）。
        var magic = br.ReadUInt32LE();
        if (magic != CrAuMagicLe)
            throw new FormatException($"不是 payload.bin（魔数 0x{magic:X8}，应为 \"CrAU\"）");

        var version = br.ReadUInt64BE();
        if (version is not (1 or 2))
            throw new FormatException($"payload 格式版本 {version} 不受支持（仅 1/2）");

        var manifestSize = br.ReadUInt64BE();
        if (manifestSize == 0 || manifestSize > (1UL << 30))
            throw new FormatException($"manifest 大小异常: {manifestSize}");

        ulong signatureSize = 0;
        if (version >= 2)
            signatureSize = br.ReadUInt32BE();

        var manifest = br.ReadBytes((long)manifestSize);
        if (version >= 2)
            br.ReadBytes((long)signatureSize);   // 签名对本工具无用，跳过

        // 数据区基准：v1 = 紧跟 manifest；v2 = 算子里是**绝对**偏移（base = 0）。
        // 头部 = magic(4) + version(8) + manifestSize(8) + [sigSize(4)]。
        var headerSize = 4UL + 8 + 8 + (version >= 2 ? 4UL : 0);
        ulong dataBase = version >= 2 ? 0UL : headerSize + manifestSize + signatureSize;

        var info = new PayloadBinInfo { FormatVersion = (uint)version };
        ParseManifest(manifest, dataBase, info);
        return info;
    }

    // ────────────────────────── protobuf wire-format 走读 ──────────────────────────

    private static void ParseManifest(byte[] manifest, ulong dataBase, PayloadBinInfo info)
    {
        var pos = 0;
        // DeltaArchiveManifest: field 2 = partitions (repeated PartitionUpdate)
        while (pos < manifest.Length)
        {
            var (tag, next) = ReadKey(manifest, pos);
            pos = next;
            if (tag.Field == 2 && tag.Wire == 2)
            {
                var body = ReadChunk(manifest, ref pos);
                var part = ParsePartition(body, dataBase, info.BlockSize);
                if (part != null) info.Partitions.Add(part);
            }
            else
            {
                Skip(manifest, ref pos, tag.Wire);
            }
        }
    }

    private static PayloadPartition? ParsePartition(byte[] body, ulong dataBase, ulong defaultBlock)
    {
        string name = "";
        ulong blockSize = defaultBlock;
        var ops = new List<byte[]>();
        var pos = 0;
        // PartitionUpdate: 1 = partition_name, 3 = operations (repeated), 7 = block_size（新版）
        while (pos < body.Length)
        {
            var (tag, next) = ReadKey(body, pos);
            pos = next;
            if (tag.Field == 1 && tag.Wire == 2) name = System.Text.Encoding.UTF8.GetString(ReadChunk(body, ref pos));
            else if (tag.Field == 3 && tag.Wire == 2) ops.Add(ReadChunk(body, ref pos));
            else if (tag.Field == 7 && tag.Wire == 0) blockSize = ReadVarint(body, ref pos);
            else Skip(body, ref pos, tag.Wire);
        }
        if (name.Length == 0) return null;

        var part = new PayloadPartition { Name = name };
        foreach (var op in ops)
            ApplyOperation(op, dataBase, blockSize, part);

        // 尺寸从 dst extents 的最大末端来（比逐 op 累加大更稳：extent 之间可以稀疏）
        if (part.SizeBytes == 0)
            foreach (var s in part.Segments)
            {
                var end = s.OutOffset + s.Length;
                if (end > part.SizeBytes) part.SizeBytes = end;
            }
        return part;
    }

    private static void ApplyOperation(byte[] body, ulong dataBase, ulong blockSize, PayloadPartition part)
    {
        uint type = 0;
        ulong? dataOffset = null, dataLength = null;
        var extents = new List<(ulong StartBlock, ulong NumBlocks)>();
        var pos = 0;
        // InstallOperation: 1 = type, 2 = data_offset, 3 = data_length, 4 = dst_extents (repeated)
        while (pos < body.Length)
        {
            var (tag, next) = ReadKey(body, pos);
            pos = next;
            switch (tag.Field)
            {
                case 1 when tag.Wire == 0: type = (uint)ReadVarint(body, ref pos); break;
                case 2 when tag.Wire == 0: dataOffset = ReadVarint(body, ref pos); break;
                case 3 when tag.Wire == 0: dataLength = ReadVarint(body, ref pos); break;
                case 4 when tag.Wire == 2:
                {
                    var ext = ReadChunk(body, ref pos);
                    ulong start = 0, num = 0;
                    var p2 = 0;
                    while (p2 < ext.Length)
                    {
                        var (t2, n2) = ReadKey(ext, p2);
                        p2 = n2;
                        if (t2.Field == 1 && t2.Wire == 0) start = ReadVarint(ext, ref p2);
                        else if (t2.Field == 2 && t2.Wire == 0) num = ReadVarint(ext, ref p2);
                        else Skip(ext, ref p2, t2.Wire);
                    }
                    extents.Add((start, num));
                    break;
                }
                default: Skip(body, ref pos, tag.Wire); break;
            }
        }

        var endBytes = 0UL;
        foreach (var (start, num) in extents)
        {
            var end = (start + num) * blockSize;
            if (end > endBytes) endBytes = end;
        }
        if (endBytes > part.SizeBytes) part.SizeBytes = endBytes;

        // 算子类型（update_metadata.proto InstallOperation.Type）：
        // 0 SOURCE_COPY｜1 SOURCE_BSDIFF｜2 REPLACE｜3 REPLACE_BZ｜4 PUFFDIFF｜5 REPLACE_XZ｜6 ZERO｜7 DISCARD｜8 BROTLI_BZ
        switch (type)
        {
            case 2: // REPLACE：数据段原样拷贝
                if (dataOffset.HasValue && dataLength.HasValue)
                    part.Segments.Add(new PayloadSegment(dataBase + dataOffset.Value, extents.Count > 0 ? extents[0].StartBlock * blockSize : 0, dataLength.Value));
                break;
            case 6: // ZERO / 7 DISCARD：目标区域保持全零（输出文件本就零初始化）——无需段
            case 7:
                break;
            default:
                if (part.Extractable)
                {
                    part.Extractable = false;
                    part.UnsupportedReason = type switch
                    {
                        0 => "增量算子 SOURCE_COPY（需要旧分区）",
                        1 => "增量算子 SOURCE_BSDIFF（需要旧分区）",
                        4 => "增量算子 PUFFDIFF（需要旧分区）",
                        3 => "压缩算子 REPLACE_BZ（本工具不做解压重组）",
                        5 => "压缩算子 REPLACE_XZ（本工具不做解压重组）",
                        8 => "压缩算子 BROTLI_BZ（本工具不做解压重组）",
                        _ => $"不支持的算子类型 {type}",
                    };
                }
                break;
        }
    }

    private readonly record struct PbTag(uint Field, uint Wire);

    /// <summary>读一个 protobuf key（= (field &lt;&lt; 3) | wire）；返回 tag 与下一个字节的位置。</summary>
    private static (PbTag Tag, int Next) ReadKey(byte[] buf, int pos)
    {
        // protobuf tag = (field_number << 3) | wire_type
        var key = ReadVarint(buf, ref pos);
        return (new PbTag((uint)(key >> 3), (uint)(key & 7)), pos);
    }

    private static ulong ReadVarint(byte[] buf, ref int pos)
    {
        ulong value = 0;
        int shift = 0;
        while (pos < buf.Length)
        {
            var b = buf[pos++];
            value |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return value;
            shift += 7;
            if (shift > 63) throw new FormatException("protobuf varint 超长（manifest 已损坏）");
        }
        throw new FormatException("protobuf varint 越界（manifest 已损坏）");
    }

    private static byte[] ReadChunk(byte[] buf, ref int pos)
    {
        var len = (int)ReadVarint(buf, ref pos);
        if (len < 0 || pos + len > buf.Length) throw new FormatException("protobuf 字段长度越界（manifest 已损坏）");
        var chunk = new byte[len];
        Array.Copy(buf, pos, chunk, 0, len);
        pos += len;
        return chunk;
    }

    private static void Skip(byte[] buf, ref int pos, uint wire)
    {
        switch (wire)
        {
            case 0: ReadVarint(buf, ref pos); break;
            case 1: pos += 8; break;
            case 2: pos += (int)ReadVarint(buf, ref pos); break;
            case 5: pos += 4; break;
            default: throw new FormatException($"protobuf wire 类型 {wire} 不认识（manifest 已损坏）");
        }
    }

    /// <summary>大端读数的简易封装（payload 头部是大端；manifest 里的 varint 自带顺序）。</summary>
    private sealed class BinaryReaderBigEndian : IDisposable
    {
        private readonly Stream _s;
        private readonly bool _leaveOpen;
        private readonly byte[] _buf = new byte[8];

        public BinaryReaderBigEndian(Stream s, bool leaveOpen) { _s = s; _leaveOpen = leaveOpen; }

        public uint ReadUInt32BE()
        {
            ReadExact(4);
            return (uint)((_buf[0] << 24) | (_buf[1] << 16) | (_buf[2] << 8) | _buf[3]);
        }

        /// <summary>按**小端**读 u32。只用于 "CrAU" 这类 ASCII 魔数 —— 头部其余字段都是大端。</summary>
        public uint ReadUInt32LE()
        {
            ReadExact(4);
            return (uint)(_buf[0] | (_buf[1] << 8) | (_buf[2] << 16) | (_buf[3] << 24));
        }

        public ulong ReadUInt64BE()
        {
            ReadExact(8);
            ulong v = 0;
            for (var i = 0; i < 8; i++) v = (v << 8) | _buf[i];
            return v;
        }

        public byte[] ReadBytes(long count)
        {
            var bytes = new byte[count];
            var read = 0;
            while (read < count)
            {
                var n = _s.Read(bytes, read, (int)(count - read));
                if (n == 0) throw new FormatException($"payload 文件被截断（读到 {read}/{count} 字节）");
                read += n;
            }
            return bytes;
        }

        private void ReadExact(int n)
        {
            var read = 0;
            while (read < n)
            {
                var got = _s.Read(_buf, read, n - read);
                if (got == 0) throw new FormatException("payload 文件被截断（头部不完整）");
                read += got;
            }
        }

        public void Dispose() { if (!_leaveOpen) _s.Dispose(); }
    }
}
