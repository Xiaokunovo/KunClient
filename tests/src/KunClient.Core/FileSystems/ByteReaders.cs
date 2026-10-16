namespace KunFlashTool.Core.FileSystems;

/// <summary>按字节偏移读取任意数据的抽象：ext4/EROFS 解析器经它按需取扇区，
/// 上游可以是设备分区（EDL 读）、super 内的逻辑分区、或本地镜像文件。</summary>
public interface IByteReader
{
    byte[]? ReadBytes(long offset, int length);
}

/// <summary>按扇区缓存批量读取的 IByteReader：底层一次读连续多个扇区，逐扇区缓存。
/// EDL 链路上每条 read 命令都有固定开销，缓存把文件系统解析的随机小读合并成大读。</summary>
public sealed class CachedByteReader : IByteReader
{
    private readonly Func<long, int, byte[]?> _read;
    private readonly int _sectorSize;
    private readonly Dictionary<long, byte[]> _cache = new();

    public CachedByteReader(Func<long, int, byte[]?> read, int sectorSize)
    {
        _read = read;
        _sectorSize = sectorSize;
    }

    public byte[]? ReadBytes(long offset, int length)
    {
        if (offset < 0 || length < 0)
            return null;
        if (length == 0)
            return Array.Empty<byte>();

        long firstSector = offset / _sectorSize;
        long lastSector = checked((offset + length - 1) / _sectorSize);
        if (!EnsureCached(firstSector, lastSector))
            return null;

        byte[] result = new byte[length];
        int destinationOffset = 0;
        long currentOffset = offset;
        while (destinationOffset < length)
        {
            long sector = currentOffset / _sectorSize;
            int offsetInSector = (int)(currentOffset % _sectorSize);
            int copyLength = Math.Min(length - destinationOffset, _sectorSize - offsetInSector);
            Buffer.BlockCopy(_cache[sector], offsetInSector, result, destinationOffset, copyLength);
            currentOffset += copyLength;
            destinationOffset += copyLength;
        }
        return result;
    }

    private bool EnsureCached(long firstSector, long lastSector)
    {
        long sector = firstSector;
        while (sector <= lastSector)
        {
            if (_cache.ContainsKey(sector))
            {
                sector++;
                continue;
            }

            long batchStart = sector;
            while (sector <= lastSector && !_cache.ContainsKey(sector))
                sector++;

            int sectorCount = checked((int)(sector - batchStart));
            byte[]? data = _read(checked(batchStart * _sectorSize), checked(sectorCount * _sectorSize));
            if (data == null || data.Length < sectorCount * _sectorSize)
                return false;

            for (int i = 0; i < sectorCount; i++)
            {
                byte[] sectorData = new byte[_sectorSize];
                Buffer.BlockCopy(data, i * _sectorSize, sectorData, 0, _sectorSize);
                _cache[batchStart + i] = sectorData;
            }
        }
        return true;
    }
}

/// <summary>super 内逻辑分区的 extent 映射（512B 逻辑扇区单位）。</summary>
public sealed record LpExtentMapping(long PhysicalSector, long NumSectors, bool IsZero = false);

/// <summary>把「super 块设备上的 IByteReader」映射成「某个逻辑分区的 IByteReader」：
/// 按逻辑偏移定位 extent，zero extent 补零，线性 extent 换算成物理偏移后透传。</summary>
public sealed class LogicalPartitionReader : IByteReader
{
    private readonly IByteReader _superReader;
    private readonly IReadOnlyList<LpExtentMapping> _extents;
    private readonly int _sectorSize;

    public LogicalPartitionReader(
        IByteReader superReader,
        IReadOnlyList<LpExtentMapping> extents,
        int sectorSize)
    {
        _superReader = superReader;
        _extents = extents;
        _sectorSize = sectorSize;
    }

    public byte[]? ReadBytes(long offset, int length)
    {
        if (offset < 0 || length < 0)
            return null;
        if (length == 0)
            return Array.Empty<byte>();

        byte[] result = new byte[length];
        int destinationOffset = 0;
        long logicalOffset = offset;

        while (destinationOffset < length)
        {
            if (!FindExtent(logicalOffset, out var extent, out long offsetInExtent))
                return null;

            long extentBytes = checked(extent.NumSectors * _sectorSize);
            int copyLength = (int)Math.Min(length - destinationOffset, extentBytes - offsetInExtent);
            if (!extent.IsZero)
            {
                long physicalOffset = checked(extent.PhysicalSector * _sectorSize + offsetInExtent);
                byte[]? data = _superReader.ReadBytes(physicalOffset, copyLength);
                if (data == null || data.Length != copyLength)
                    return null;
                Buffer.BlockCopy(data, 0, result, destinationOffset, copyLength);
            }

            logicalOffset += copyLength;
            destinationOffset += copyLength;
        }

        return result;
    }

    private bool FindExtent(long logicalOffset, out LpExtentMapping result, out long offsetInExtent)
    {
        long extentStart = 0;
        foreach (LpExtentMapping extent in _extents)
        {
            long extentBytes = checked(extent.NumSectors * _sectorSize);
            if (logicalOffset < extentStart + extentBytes)
            {
                result = extent;
                offsetInExtent = logicalOffset - extentStart;
                return true;
            }
            extentStart += extentBytes;
        }

        result = _extents.Count > 0 ? _extents[0] : new LpExtentMapping(0, 0);
        offsetInExtent = 0;
        return false;
    }
}
