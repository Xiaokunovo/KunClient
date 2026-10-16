using System.Runtime.InteropServices;

namespace KunFlashTool.Core.DynamicSuper.LpMake;

/// <summary>
/// LP 元数据构建器 - 移植自 AOSP liblp MetadataBuilder（精简版）。
/// 职责：按 logical_block_size 对齐分配线性 extent，保证生成的 super 元数据
/// 可被设备端 dm-linear 正确展开。
/// </summary>
public sealed class LpMetadataBuilder
{
    private LpMetadataGeometry _geometry = new();
    private LpMetadataHeader _header = new();
    private readonly List<LpPartitionBuilder> _partitions = new();
    private readonly List<LpPartitionGroupBuilder> _groups = new();
    private readonly List<LpMetadataBlockDevice> _blockDevices = new();

    private LpMetadataBuilder() { }

    /// <summary>从 block device 列表创建空的逻辑分区表。</summary>
    public static LpMetadataBuilder? New(
        IReadOnlyList<BlockDeviceInfo> blockDevices,
        string superPartition,
        uint metadataMaxSize,
        uint metadataSlotCount)
    {
        var builder = new LpMetadataBuilder();
        return builder.Init(blockDevices, superPartition, metadataMaxSize, metadataSlotCount)
            ? builder
            : null;
    }

    public bool AddGroup(string groupName, ulong maximumSize)
    {
        if (FindGroup(groupName) != null)
            return false;
        _groups.Add(new LpPartitionGroupBuilder(groupName, maximumSize));
        return true;
    }

    public LpPartitionBuilder? AddPartition(string name, string groupName, uint attributes)
    {
        if (string.IsNullOrEmpty(name) || FindPartition(name) != null || FindGroup(groupName) == null)
            return null;
        var p = new LpPartitionBuilder(name, groupName, attributes);
        _partitions.Add(p);
        return p;
    }

    /// <summary>调整分区大小（向上对齐到 logical_block_size）。</summary>
    public bool ResizePartition(LpPartitionBuilder partition, ulong requestedSize)
    {
        ulong alignedSize = LpMakeUtility.AlignTo(requestedSize, _geometry.LogicalBlockSize);
        ulong oldSize = partition.Size;

        if (!ValidatePartitionSizeChange(partition, oldSize, alignedSize))
            return false;

        if (alignedSize > oldSize)
            return GrowPartition(partition, alignedSize);
        if (alignedSize < oldSize)
            partition.ShrinkTo(alignedSize);
        return true;
    }

    /// <summary>导出为可序列化的 <see cref="LpMakeMetadata"/>。</summary>
    public LpMakeMetadata? Export()
    {
        if (!ValidatePartitionGroups())
            return null;

        var metadata = new LpMakeMetadata
        {
            Geometry = _geometry,
            Header = CloneHeader()
        };

        foreach (var bd in _blockDevices)
            metadata.BlockDevices.Add(bd);

        // 映射 group 名 → index
        var groupIndices = new Dictionary<string, uint>();
        foreach (var group in _groups)
        {
            if (group.Name.Length > LpMakeConstants.PartitionNameLen)
                return null;

            metadata.Groups.Add(new LpMetadataPartitionGroup
            {
                Name = group.Name,
                MaximumSize = group.MaximumSize
            });
            groupIndices[group.Name] = (uint)(metadata.Groups.Count - 1);
        }

        foreach (var partition in _partitions)
        {
            if (partition.Name.Length > LpMakeConstants.PartitionNameLen)
                return null;
            if (!groupIndices.TryGetValue(partition.GroupName, out uint gi))
                return null;

            var part = new LpMetadataPartition
            {
                Name = partition.Name,
                Attributes = partition.Attributes,
                FirstExtentIndex = (uint)metadata.Extents.Count,
                NumExtents = (uint)partition.Extents.Count,
                GroupIndex = gi
            };

            foreach (var extent in partition.Extents)
            {
                if (!extent.AddTo(metadata))
                    return null;
            }

            metadata.Partitions.Add(part);
        }

        metadata.Header.Partitions.NumEntries = (uint)metadata.Partitions.Count;
        metadata.Header.Extents.NumEntries = (uint)metadata.Extents.Count;
        metadata.Header.Groups.NumEntries = (uint)metadata.Groups.Count;
        metadata.Header.BlockDevices.NumEntries = (uint)metadata.BlockDevices.Count;

        return metadata;
    }

    public LpPartitionBuilder? FindPartition(string name) =>
        _partitions.FirstOrDefault(p => p.Name == name);

    public LpPartitionGroupBuilder? FindGroup(string name) =>
        _groups.FirstOrDefault(g => g.Name == name);

    public ulong AllocatableSpace()
    {
        ulong total = 0;
        foreach (var bd in _blockDevices)
            total += bd.Size - bd.FirstLogicalSector * LpMakeConstants.LpSectorSize;
        return total;
    }

    private bool Init(IReadOnlyList<BlockDeviceInfo> blockDevices, string superPartition,
        uint metadataMaxSize, uint metadataSlotCount)
    {
        if (metadataMaxSize < LpMakeConstants.SizeOfHeaderV10 || metadataSlotCount == 0 || blockDevices.Count == 0)
            return false;

        metadataMaxSize = LpMakeUtility.AlignTo(metadataMaxSize, (uint)LpMakeConstants.LpSectorSize);

        uint logicalBlockSize = 0;
        foreach (var di in blockDevices)
        {
            if (di.LogicalBlockSize == 0 || di.LogicalBlockSize % LpMakeConstants.LpSectorSize != 0)
                return false;
            if (di.Size % di.LogicalBlockSize != 0)
                return false;

            if (logicalBlockSize == 0) logicalBlockSize = di.LogicalBlockSize;
            if (logicalBlockSize != di.LogicalBlockSize) return false;

            var bd = new LpMetadataBlockDevice
            {
                Alignment = di.Alignment,
                AlignmentOffset = di.AlignmentOffset,
                Size = di.Size,
                PartitionName = di.PartitionName
            };

            // 初始 first_logical_sector（非 super 设备）
            ulong freeStart = LpMakeConstants.LpSectorSize;
            if (bd.Alignment != 0 || bd.AlignmentOffset != 0)
                freeStart = LpMakeUtility.AlignTo(freeStart, bd.Alignment, bd.AlignmentOffset);
            else
                freeStart = LpMakeUtility.AlignTo(freeStart, logicalBlockSize);
            bd.FirstLogicalSector = freeStart / LpMakeConstants.LpSectorSize;

            if (di.PartitionName == superPartition)
                _blockDevices.Insert(0, bd);
            else
                _blockDevices.Add(bd);
        }

        if (_blockDevices.Count == 0 || _blockDevices[0].PartitionName != superPartition)
            return false;

        // 必须是 ref 局部变量：super.FirstLogicalSector 的修改要写回列表元素，
        // 按值拷贝会让下面的赋值全部丢在临时副本上。
        ref var super = ref CollectionsMarshal.AsSpan(_blockDevices)[0];

        ulong totalReserved = LpMakeUtility.GetTotalMetadataSize(metadataMaxSize, metadataSlotCount);
        if (super.Size < totalReserved)
            return false;

        ulong freeAreaStart = totalReserved;
        if (super.Alignment != 0 || super.AlignmentOffset != 0)
            freeAreaStart = LpMakeUtility.AlignTo(freeAreaStart, super.Alignment, super.AlignmentOffset);
        else
            freeAreaStart = LpMakeUtility.AlignTo(freeAreaStart, logicalBlockSize);
        super.FirstLogicalSector = freeAreaStart / LpMakeConstants.LpSectorSize;

        ulong minimumDisk = super.FirstLogicalSector * LpMakeConstants.LpSectorSize + logicalBlockSize;
        if (super.Size < minimumDisk)
            return false;

        _geometry.MetadataMaxSize = metadataMaxSize;
        _geometry.MetadataSlotCount = metadataSlotCount;
        _geometry.LogicalBlockSize = logicalBlockSize;

        return AddGroup(LpMakeConstants.KDefaultGroup, 0);
    }

    private bool GrowPartition(LpPartitionBuilder partition, ulong alignedSize)
    {
        ulong spaceNeeded = alignedSize - partition.Size;
        ulong sectorsNeeded = spaceNeeded / LpMakeConstants.LpSectorSize;

        var freeRegions = GetFreeRegions();
        ulong sectorsPerBlock = _geometry.LogicalBlockSize / LpMakeConstants.LpSectorSize;
        if (sectorsPerBlock == 0) return false;

        var newExtents = new List<LinearExtent>();

        // 尝试扩展最后一个 extent 的尾巴
        if (partition.Extents.Count > 0 && partition.Extents[^1] is LinearExtent lastExt)
        {
            var bd = _blockDevices[(int)lastExt.DeviceIndex];
            ulong nextAligned = AlignSector(bd, lastExt.EndSector);
            if (lastExt.EndSector != nextAligned)
            {
                ulong extra = Math.Min(nextAligned - lastExt.EndSector, sectorsNeeded);
                var ext = new LinearExtent(extra, lastExt.DeviceIndex, lastExt.EndSector);
                if (!IsAnyRegionAllocated(ext))
                {
                    sectorsNeeded -= extra;
                    newExtents.Add(ext);
                }
            }
        }

        foreach (var region in freeRegions)
        {
            if (sectorsNeeded == 0) break;

            ulong len = region.Length;
            if (len % sectorsPerBlock != 0)
            {
                len = len / sectorsPerBlock * sectorsPerBlock;
                if (len == 0) continue;
            }

            ulong sectors = Math.Min(sectorsNeeded, len);
            newExtents.Add(new LinearExtent(sectors, region.DeviceIndex, region.Start));
            sectorsNeeded -= sectors;
        }

        if (sectorsNeeded > 0)
            return false;

        foreach (var ext in newExtents)
            partition.AddExtent(ext);
        return true;
    }

    private List<LpInterval> GetFreeRegions()
    {
        var freeRegions = new List<LpInterval>();
        var deviceExtents = new List<List<LpInterval>>(_blockDevices.Count);
        for (int i = 0; i < _blockDevices.Count; i++)
            deviceExtents.Add(new List<LpInterval>());

        foreach (var partition in _partitions)
        foreach (var extent in partition.Extents)
        {
            if (extent is LinearExtent le)
                deviceExtents[(int)le.DeviceIndex].Add(le.AsInterval());
        }

        for (int i = 0; i < _blockDevices.Count; i++)
        {
            var bd = _blockDevices[i];
            ulong firstSector = bd.FirstLogicalSector;
            ulong lastSector = bd.Size / LpMakeConstants.LpSectorSize;

            var extents = deviceExtents[i];
            extents.Add(new LpInterval((uint)i, firstSector, firstSector));
            extents.Add(new LpInterval((uint)i, lastSector, lastSector));
            extents.Sort();

            for (int j = 1; j < extents.Count; j++)
            {
                var prev = extents[j - 1];
                var cur = extents[j];
                ulong aligned = AlignSector(bd, prev.End);
                if (aligned >= cur.Start) continue;
                freeRegions.Add(new LpInterval(cur.DeviceIndex, aligned, cur.Start));
            }
        }

        return freeRegions;
    }

    private ulong AlignSector(LpMetadataBlockDevice bd, ulong sector)
    {
        ulong lba = sector * LpMakeConstants.LpSectorSize;
        ulong aligned = LpMakeUtility.AlignTo(lba, bd.Alignment, bd.AlignmentOffset);
        return LpMakeUtility.AlignTo(aligned, LpMakeConstants.LpSectorSize) / LpMakeConstants.LpSectorSize;
    }

    private bool ValidatePartitionSizeChange(LpPartitionBuilder partition, ulong oldSize, ulong newSize)
    {
        if (newSize <= oldSize) return true;
        var group = FindGroup(partition.GroupName);
        if (group == null) return false;
        if (group.MaximumSize == 0) return true;

        ulong spaceNeeded = newSize - oldSize;
        ulong groupSize = TotalSizeOfGroup(group);
        return groupSize + spaceNeeded <= group.MaximumSize;
    }

    private ulong TotalSizeOfGroup(LpPartitionGroupBuilder group) =>
        _partitions.Where(p => p.GroupName == group.Name).Aggregate(0UL, (s, p) => s + p.BytesOnDisk());

    private bool ValidatePartitionGroups()
    {
        foreach (var group in _groups)
        {
            if (group.MaximumSize == 0) continue;
            if (TotalSizeOfGroup(group) > group.MaximumSize)
                return false;
        }
        return true;
    }

    private bool IsAnyRegionAllocated(LinearExtent candidate)
    {
        foreach (var partition in _partitions)
        foreach (var extent in partition.Extents)
        {
            if (extent is LinearExtent le && le.OverlapsWith(candidate))
                return true;
        }
        return false;
    }

    private LpMetadataHeader CloneHeader() => new()
    {
        Magic = _header.Magic,
        MajorVersion = _header.MajorVersion,
        MinorVersion = _header.MinorVersion,
        HeaderSize = _header.HeaderSize,
        Flags = _header.Flags
    };
}
