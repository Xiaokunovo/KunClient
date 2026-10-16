using System;
using System.Collections.Generic;
using System.IO;
using System.Xml.Linq;

namespace KunFlashTool.Core.Protocol.Firehose;

/// <summary>A single <c>&lt;program&gt;</c> entry in a rawprogram XML file.</summary>
public sealed class RawProgramEntry
{
    public string FileName = "";
    public string? Label;
    public uint PhysicalPartitionNumber;
    public string? StartSector;                 // may contain 'NUM_DISK_SECTORS' placeholder or arithmetic
    public ulong NumPartitionSectors;
    public uint SectorSizeInBytes = 512;
    public bool IsSparse;

    public ulong RawStartSector
    {
        get
        {
            if (StartSector == null)
                return 0;
            if (ulong.TryParse(StartSector, out var v))
                return v;
            // Handle a few simple expressions QFIL uses.
            if (StartSector.Contains("NUM_DISK_SECTORS", StringComparison.OrdinalIgnoreCase))
            {
                // replaced by the caller via ResolveStartSector
            }
            return 0;
        }
    }

    public override string ToString() => $"[{PhysicalPartitionNumber}] {FileName} @ {StartSector}";
}

/// <summary>A single <patch> entry in a patch XML file.</summary>
public sealed class PatchEntry
{
    public string FileName = "";   // typically "DISK"
    public string? StartSector;
    public ulong SizeInBytes;
    public string? Id;
    public string? Value;
    public string? Offset;
    public string? BeginBlocks;
    public string? PhysicalPartitionNumber;

    public override string ToString() => $"patch {StartSector}+{SizeInBytes} ({Value})";
}

/// <summary>
/// Parses the QFIL-style rawprogram&lt;n&gt;.xml / patch&lt;n&gt;.xml files that ship with
/// factory images. These files drive the full-device flash flow in the same way
/// QFIL / bkerler's "edl qfil" command does.
/// </summary>
public static class QfilXmlParser
{
    /// <summary>Parses rawprogram XML entries from a file.</summary>
    public static List<RawProgramEntry> ParseRawProgram(string xmlFile)
    {
        var result = new List<RawProgramEntry>();
        using var reader = File.OpenText(xmlFile);
        return ParseRawProgram(reader);
    }

    public static List<RawProgramEntry> ParseRawProgram(TextReader xmlFile)
    {
        var result = new List<RawProgramEntry>();
        var doc = XDocument.Load(xmlFile);
        foreach (var elem in doc.Root?.Elements("program") ?? new XElement[0])
        {
            var entry = new RawProgramEntry
            {
                FileName = (string?)elem.Attribute("filename") ?? "",
                Label = (string?)elem.Attribute("label"),
                PhysicalPartitionNumber = ParseUInt(elem.Attribute("physical_partition_number")?.Value, 0),
                StartSector = (string?)elem.Attribute("start_sector"),
                SectorSizeInBytes = ParseUInt(elem.Attribute("SECTOR_SIZE_IN_BYTES")?.Value, 512),
            };
            var numSectors = elem.Attribute("num_partition_sectors")?.Value;
            if (numSectors != null && ulong.TryParse(numSectors, out var ns))
                entry.NumPartitionSectors = ns;
            result.Add(entry);
        }
        return result;
    }

    /// <summary>Parses patch XML entries into a list.</summary>
    public static List<PatchEntry> ParsePatch(string xmlFile)
    {
        var result = new List<PatchEntry>();
        using var reader = File.OpenText(xmlFile);
        return ParsePatch(reader);
    }

    public static List<PatchEntry> ParsePatch(TextReader reader)
    {
        var result = new List<PatchEntry>();
        var doc = XDocument.Load(reader);
        foreach (var elem in doc.Root?.Elements("patch") ?? new XElement[0])
        {
            var entry = new PatchEntry
            {
                FileName = (string?)elem.Attribute("filename") ?? "",
                StartSector = (string?)elem.Attribute("start_sector"),
                Value = (string?)elem.Attribute("value"),
                Offset = (string?)elem.Attribute("offset"),
                BeginBlocks = (string?)elem.Attribute("begin_blocks"),
                PhysicalPartitionNumber = (string?)elem.Attribute("physical_partition_number"),
                Id = (string?)elem.Attribute("id"),
            };
            var size = elem.Attribute("size_in_bytes")?.Value;
            if (size != null && ulong.TryParse(size, out var sz))
                entry.SizeInBytes = sz;
            result.Add(entry);
        }
        return result;
    }

    /// <summary>
    /// QFIL files reference unnamed images (filename empty, SECTOR_SIZE_IN_BYTES set and
    /// num_partition_sectors). Use this to compute the sector size used by a rawprogram set.
    /// </summary>
    public static uint? GetSectorSize(string xmlFile)
    {
        foreach (var e in ParseRawProgram(xmlFile))
            if (e.SectorSizeInBytes != 0)
                return e.SectorSizeInBytes;
        return null;
    }

    /// <summary>
    /// True when a rawprogram entry should still be programmed given the set of
    /// partition labels the user unchecked. A null/empty set keeps everything,
    /// and entries without a label (GPT headers, anonymous images) are always kept.
    /// Matching is case-insensitive (build the set with StringComparer.OrdinalIgnoreCase).
    /// </summary>
    public static bool ShouldProgram(RawProgramEntry entry, ISet<string>? excludedLabels)
        => excludedLabels is not { Count: > 0 }
           || entry.Label is null
           || !excludedLabels.Contains(entry.Label);

    private static uint ParseUInt(string? value, uint fallback)
    {
        if (value != null && uint.TryParse(value, out var v))
            return v;
        return fallback;
    }
}