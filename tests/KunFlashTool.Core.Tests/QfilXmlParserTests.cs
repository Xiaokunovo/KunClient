using System.IO;
using System.Linq;
using KunFlashTool.Core.Protocol.Firehose;
using Xunit;

namespace KunFlashTool.Core.Tests;

public class QfilXmlParserTests
{
    [Fact]
    public void ParseRawProgram_ReadsEntriesAndNumbers()
    {
        const string xml = """
        <?xml version="1.0"?>
        <data>
          <program SECTOR_SIZE_IN_BYTES="4096" file_sector_offset="0" filename="gpt_main0.bin" label="Basic for 'gpt'" num_partition_sectors="16" physical_partition_number="0" start_sector="0"/>
          <program SECTOR_SIZE_IN_BYTES="4096" filename="sbl1.bin" num_partition_sectors="128" physical_partition_number="0" start_sector="16"/>
          <program SECTOR_SIZE_IN_BYTES="4096" filename="partition.bin" num_partition_sectors="0" physical_partition_number="0" start_sector="NUM_DISK_SECTORS"/>
        </data>
        """;

        using var sr = new StringReader(xml);
        var entries = QfilXmlParser.ParseRawProgram(sr);

        Assert.Equal(3, entries.Count);
        Assert.Equal("gpt_main0.bin", entries[0].FileName);
        Assert.Equal(4096u, entries[0].SectorSizeInBytes);
        Assert.Equal(16ul, entries[0].NumPartitionSectors);
        Assert.Equal("0", entries[0].StartSector);
        Assert.Equal("sbl1.bin", entries[1].FileName);
        Assert.Equal("NUM_DISK_SECTORS", entries[2].StartSector);
    }

    [Fact]
    public void ShouldProgram_KeepsEverythingWhenNoExclusions()
    {
        var entry = new RawProgramEntry { FileName = "boot.img", Label = "boot" };
        Assert.True(QfilXmlParser.ShouldProgram(entry, null));
        Assert.True(QfilXmlParser.ShouldProgram(entry, new HashSet<string>()));
    }

    [Fact]
    public void ShouldProgram_ExcludesCaseInsensitively()
    {
        var entry = new RawProgramEntry { FileName = "boot.img", Label = "boot_a" };
        var excluded = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "BOOT_A" };
        Assert.False(QfilXmlParser.ShouldProgram(entry, excluded));
        Assert.True(QfilXmlParser.ShouldProgram(
            new RawProgramEntry { FileName = "xbl.img", Label = "xbl_a" }, excluded));
    }

    [Fact]
    public void ShouldProgram_KeepsUnlabeledEntries()
    {
        var excluded = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "boot" };
        Assert.True(QfilXmlParser.ShouldProgram(
            new RawProgramEntry { FileName = "gpt_main0.bin", Label = null }, excluded));
        Assert.True(QfilXmlParser.ShouldProgram(
            new RawProgramEntry { FileName = "gpt_both0.bin", Label = "" }, excluded));
    }

    [Fact]
    public void ParsePatch_ReadsDiskPatches()
    {
        const string xml = """
        <?xml version="1.0"?>
        <data>
          <patch SECTOR_SIZE_IN_BYTES="4096" filename="DISK" physical_partition_number="0"
                 size_in_bytes="16" start_sector="2" value="0x0300a57e010000000700000000000000"/>
        </data>
        """;

        using var sr = new StringReader(xml);
        var patches = QfilXmlParser.ParsePatch(sr);

        Assert.Single(patches);
        Assert.Equal("DISK", patches[0].FileName);
        Assert.Equal(16ul, patches[0].SizeInBytes);
        Assert.Equal("2", patches[0].StartSector);
        Assert.StartsWith("0x", patches[0].Value);
        // 16 bytes == 32 hex chars (excluding the 0x prefix).
        Assert.Equal(32, (patches[0].Value?.Length ?? 0) - 2);
    }
}