using System.IO;

namespace KunFlashTool.Core.DynamicSuper.LpMake;

/// <summary>
/// LP 元数据二进制序列化 - 移植自 AOSP liblp writer.cpp。
/// <para>
/// 校验和语义：geometry 的 SHA256 对前 52 字节计算（checksum 区先置 0）；metadata header
/// 的 SHA256 对 headerSize 范围计算（HeaderChecksum 区先置 0）。两次序列化同一对象必须
/// 幂等 —— 所以 header 校验和区域每次先清零再算。
/// </para>
/// </summary>
public static class LpMetadataSerializer
{
    /// <summary>序列化几何信息为 4096 字节块（含 SHA256 校验）。</summary>
    public static byte[] SerializeGeometry(LpMetadataGeometry geometry)
    {
        var buf = new byte[LpMakeConstants.LpMetadataGeometrySize];
        int off = 0;

        LpMakeUtility.WriteU32(buf, ref off, geometry.Magic);
        LpMakeUtility.WriteU32(buf, ref off, geometry.StructSize);

        // checksum 占位先写 0
        int checksumOffset = off;
        off += 32;

        LpMakeUtility.WriteU32(buf, ref off, geometry.MetadataMaxSize);
        LpMakeUtility.WriteU32(buf, ref off, geometry.MetadataSlotCount);
        LpMakeUtility.WriteU32(buf, ref off, geometry.LogicalBlockSize);

        // 对前 SizeOfGeometry 字节计算 SHA256（checksum 区域为 0）
        byte[] hash = LpMakeUtility.Sha256(buf.AsSpan(0, LpMakeConstants.SizeOfGeometry));
        hash.CopyTo(buf.AsSpan(checksumOffset));

        return buf;
    }

    /// <summary>序列化 metadata header + tables（不含 geometry）。</summary>
    public static byte[] SerializeMetadata(LpMakeMetadata metadata)
    {
        var header = metadata.Header;

        // 序列化各表
        byte[] partitionsBlob = SerializePartitions(metadata.Partitions);
        byte[] extentsBlob = SerializeExtents(metadata.Extents);
        byte[] groupsBlob = SerializeGroups(metadata.Groups);
        byte[] blockDevicesBlob = SerializeBlockDevices(metadata.BlockDevices);

        // 计算偏移
        header.Partitions.Offset = 0;
        header.Extents.Offset = (uint)(header.Partitions.Offset + partitionsBlob.Length);
        header.Groups.Offset = (uint)(header.Extents.Offset + extentsBlob.Length);
        header.BlockDevices.Offset = (uint)(header.Groups.Offset + groupsBlob.Length);
        header.TablesSize = (uint)(header.BlockDevices.Offset + blockDevicesBlob.Length);

        // 表 payload
        byte[] tables = new byte[header.TablesSize];
        partitionsBlob.CopyTo(tables, (int)header.Partitions.Offset);
        extentsBlob.CopyTo(tables, (int)header.Extents.Offset);
        groupsBlob.CopyTo(tables, (int)header.Groups.Offset);
        blockDevicesBlob.CopyTo(tables, (int)header.BlockDevices.Offset);

        // tables checksum
        header.TablesChecksum = LpMakeUtility.Sha256(tables);

        // The checksum field itself must be zero while calculating the header hash.
        // SerializeMetadata can be called more than once for the same metadata object.
        header.HeaderChecksum = new byte[32];
        byte[] headerBlob = SerializeHeader(header);

        // header checksum（checksum 区域为 0）
        header.HeaderChecksum = LpMakeUtility.Sha256(headerBlob.AsSpan(0, (int)header.HeaderSize));
        headerBlob = SerializeHeader(header);

        // 拼接
        byte[] result = new byte[headerBlob.Length + tables.Length];
        headerBlob.CopyTo(result, 0);
        tables.CopyTo(result, headerBlob.Length);
        return result;
    }

    private static byte[] SerializeHeader(LpMetadataHeader h)
    {
        var buf = new byte[h.HeaderSize];
        int off = 0;

        LpMakeUtility.WriteU32(buf, ref off, h.Magic);
        LpMakeUtility.WriteU16(buf, ref off, h.MajorVersion);
        LpMakeUtility.WriteU16(buf, ref off, h.MinorVersion);
        LpMakeUtility.WriteU32(buf, ref off, h.HeaderSize);
        LpMakeUtility.WriteBytes(buf, ref off, h.HeaderChecksum);
        LpMakeUtility.WriteU32(buf, ref off, h.TablesSize);
        LpMakeUtility.WriteBytes(buf, ref off, h.TablesChecksum);
        WriteTableDescriptor(buf, ref off, h.Partitions);
        WriteTableDescriptor(buf, ref off, h.Extents);
        WriteTableDescriptor(buf, ref off, h.Groups);
        WriteTableDescriptor(buf, ref off, h.BlockDevices);

        // V1.2 扩展字段
        if (h.HeaderSize >= LpMakeConstants.SizeOfHeaderV12)
        {
            LpMakeUtility.WriteU32(buf, ref off, h.Flags);
            // reserved 已为 0
        }

        return buf;
    }

    private static void WriteTableDescriptor(Span<byte> buf, ref int off, LpMetadataTableDescriptor td)
    {
        LpMakeUtility.WriteU32(buf, ref off, td.Offset);
        LpMakeUtility.WriteU32(buf, ref off, td.NumEntries);
        LpMakeUtility.WriteU32(buf, ref off, td.EntrySize);
    }

    private static byte[] SerializePartitions(IReadOnlyList<LpMetadataPartition> partitions)
    {
        var buf = new byte[partitions.Count * LpMakeConstants.SizeOfPartition];
        for (int i = 0; i < partitions.Count; i++)
        {
            int off = i * LpMakeConstants.SizeOfPartition;
            var p = partitions[i];
            LpMakeUtility.WriteFixedAsciiName(buf.AsSpan(off, LpMakeConstants.PartitionNameLen), p.Name);
            off += LpMakeConstants.PartitionNameLen;
            LpMakeUtility.WriteU32(buf, ref off, p.Attributes);
            LpMakeUtility.WriteU32(buf, ref off, p.FirstExtentIndex);
            LpMakeUtility.WriteU32(buf, ref off, p.NumExtents);
            LpMakeUtility.WriteU32(buf, ref off, p.GroupIndex);
        }
        return buf;
    }

    private static byte[] SerializeExtents(IReadOnlyList<LpMetadataExtent> extents)
    {
        var buf = new byte[extents.Count * LpMakeConstants.SizeOfExtent];
        for (int i = 0; i < extents.Count; i++)
        {
            int off = i * LpMakeConstants.SizeOfExtent;
            var e = extents[i];
            LpMakeUtility.WriteU64(buf, ref off, e.NumSectors);
            LpMakeUtility.WriteU32(buf, ref off, e.TargetType);
            LpMakeUtility.WriteU64(buf, ref off, e.TargetData);
            LpMakeUtility.WriteU32(buf, ref off, e.TargetSource);
        }
        return buf;
    }

    private static byte[] SerializeGroups(IReadOnlyList<LpMetadataPartitionGroup> groups)
    {
        var buf = new byte[groups.Count * LpMakeConstants.SizeOfPartitionGroup];
        for (int i = 0; i < groups.Count; i++)
        {
            int off = i * LpMakeConstants.SizeOfPartitionGroup;
            var g = groups[i];
            LpMakeUtility.WriteFixedAsciiName(buf.AsSpan(off, LpMakeConstants.PartitionNameLen), g.Name);
            off += LpMakeConstants.PartitionNameLen;
            LpMakeUtility.WriteU32(buf, ref off, g.Flags);
            LpMakeUtility.WriteU64(buf, ref off, g.MaximumSize);
        }
        return buf;
    }

    private static byte[] SerializeBlockDevices(IReadOnlyList<LpMetadataBlockDevice> devices)
    {
        var buf = new byte[devices.Count * LpMakeConstants.SizeOfBlockDevice];
        for (int i = 0; i < devices.Count; i++)
        {
            int off = i * LpMakeConstants.SizeOfBlockDevice;
            var d = devices[i];
            LpMakeUtility.WriteU64(buf, ref off, d.FirstLogicalSector);
            LpMakeUtility.WriteU32(buf, ref off, d.Alignment);
            LpMakeUtility.WriteU32(buf, ref off, d.AlignmentOffset);
            LpMakeUtility.WriteU64(buf, ref off, d.Size);
            LpMakeUtility.WriteFixedAsciiName(buf.AsSpan(off, LpMakeConstants.PartitionNameLen), d.PartitionName);
            off += LpMakeConstants.PartitionNameLen;
            LpMakeUtility.WriteU32(buf, ref off, d.Flags);
        }
        return buf;
    }
}

/// <summary>
/// LP 镜像写入器 - 将 metadata 写入 super_meta / super_empty 镜像字节流。
/// </summary>
public static class LpImageWriter
{
    /// <summary>
    /// 写入 metadata-only 镜像（super_empty.img 格式）。
    /// 布局: geometry + metadata（无 reserved 前缀，仅 geometry + 序列化 metadata）。
    /// </summary>
    public static void WriteEmptyImage(Stream output, LpMakeMetadata metadata)
    {
        byte[] geometry = LpMetadataSerializer.SerializeGeometry(metadata.Geometry);
        byte[] metaBlob = LpMetadataSerializer.SerializeMetadata(metadata);
        output.Write(geometry);
        output.Write(metaBlob);
    }

    /// <summary>
    /// 写入完整 metadata 区域（OPLUS super_meta 格式）。布局：
    /// <code>
    /// [4096 reserved zeros]
    /// [4096 geometry primary]
    /// [4096 geometry backup]
    /// [metadata_max_size × slot_count]  (primary copies，有效数据 + 零填充)
    /// [metadata_max_size × slot_count]  (backup copies)
    /// </code>
    /// 总大小 = GetTotalMetadataSize(metadata_max_size, slot_count)。
    /// </summary>
    public static void WriteMetaImage(Stream output, LpMakeMetadata metadata)
    {
        var geo = metadata.Geometry;
        byte[] geometryBlob = LpMetadataSerializer.SerializeGeometry(geo);
        byte[] metaBlob = LpMetadataSerializer.SerializeMetadata(metadata);

        // 将 metadata 填充到 metadata_max_size
        byte[] paddedMeta = new byte[geo.MetadataMaxSize];
        metaBlob.AsSpan(0, Math.Min(metaBlob.Length, paddedMeta.Length)).CopyTo(paddedMeta);

        // 1. Reserved zeros
        output.Write(new byte[LpMakeConstants.LpPartitionReservedBytes]);

        // 2. Geometry primary + backup
        output.Write(geometryBlob);
        output.Write(geometryBlob);

        // 3. Primary metadata slots
        for (uint i = 0; i < geo.MetadataSlotCount; i++)
            output.Write(paddedMeta);

        // 4. Backup metadata slots
        for (uint i = 0; i < geo.MetadataSlotCount; i++)
            output.Write(paddedMeta);
    }
}
