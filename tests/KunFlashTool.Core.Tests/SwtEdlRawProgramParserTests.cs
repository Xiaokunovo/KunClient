using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using KunFlashTool.Core.SwtEdl;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 移植层 <c>SwtEdl/Storage/RawProgramParser.cs</c>（对应 <c>third_party/swt-edl/src/storage/RawProgramParser.cpp</c>）的回归用例。
///
/// <para>rawprogram XML 是整包刷写的**入口**，解析错一条就是把镜像写进错误的扇区。
/// 所以这里锁的是筛选语义：缺失镜像跳过、零扇区跳过、OCDT 魔数校验、
/// 十六进制/十进制混写、设备端解析的动态扇区，以及 super 拆分判据。</para>
/// </summary>
public class SwtEdlRawProgramParserTests : IDisposable
{
    private readonly string _directory;

    public SwtEdlRawProgramParserTests()
    {
        _directory = Path.Combine(Path.GetTempPath(), "swtedl-parser-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(_directory);
    }

    public void Dispose()
    {
        try { Directory.Delete(_directory, recursive: true); } catch { }
    }

    private string WriteXml(string name, string content)
    {
        var path = Path.Combine(_directory, name);
        File.WriteAllText(path, content, new UTF8Encoding(false));
        return path;
    }

    private void WriteImage(string name, int size, byte filler = 0x00)
    {
        var bytes = new byte[size];
        if (filler != 0x00) Array.Fill(bytes, filler);
        File.WriteAllBytes(Path.Combine(_directory, name), bytes);
    }

    private static string Program(string filename, string label, string sectors, string startSector,
                                  string sectorSize = "512") =>
        $"  <program SECTOR_SIZE_IN_BYTES=\"{sectorSize}\" file_sector_offset=\"0\"" +
        $" filename=\"{filename}\" label=\"{label}\"" +
        $" num_partition_sectors=\"{sectors}\" physical_partition_number=\"0\"" +
        $" sparse=\"false\" start_sector=\"{startSector}\"/>\n";

    private static string Wrap(string body) =>
        "<?xml version=\"1.0\" ?>\n<data>\n" + body + "</data>\n";

    [Fact]
    public void Parse_KeepsResolvableEntries_AndSkipsMissingOrZeroSector()
    {
        WriteImage("boot.img", 1024);
        WriteImage("system.img", 2048);
        WriteImage("zero.img", 512);

        var xml = WriteXml("rawprogram0.xml", Wrap(
            Program("boot.img", "boot", "2", "1000") +
            Program("system.img", "system", "4", "2000") +
            Program("missing.img", "missing", "2", "3000") +   // 磁盘上没有 → 跳过
            Program("zero.img", "zero", "0", "4000")));        // 零扇区 → 跳过

        var plan = new RawProgramPlan();
        Assert.True(RawProgramParser.Parse(new[] { xml }, plan, out var error), error);

        Assert.Equal(2, plan.Entries.Count);

        var boot = plan.Entries[0];
        Assert.Equal("boot", boot.Label);
        Assert.Equal("boot.img", boot.Filename);
        Assert.Equal(512u, boot.SectorSize);
        Assert.Equal(0u, boot.Lun);
        Assert.Equal(2UL, boot.SectorCount);
        Assert.Equal(1000UL, boot.StartSector);
        Assert.True(boot.StartSectorResolved);
        Assert.Equal(Path.Combine(_directory, "boot.img"), boot.SourcePath);

        Assert.Equal("system", plan.Entries[1].Label);
        Assert.Equal(4UL, plan.Entries[1].SectorCount);

        // 源字节数 = Σ min(文件长度, 扇区数 × 扇区大小)，是进度条的分母
        Assert.Equal(3072UL, plan.TotalSourceBytes);   // 1024 + 2048

        Assert.Contains(plan.Warnings, w => w.Contains("跳过缺失镜像：missing", StringComparison.Ordinal));
        Assert.Contains(plan.Warnings, w => w.Contains("跳过零扇区条目：zero", StringComparison.Ordinal));
    }

    [Fact]
    public void Parse_AcceptsHexValues_AndTrailingDotNumbers()
    {
        WriteImage("hex.img", 8192);

        // 厂商包里 SECTOR_SIZE_IN_BYTES / num_partition_sectors 常见 0x 前缀写法
        var xml = WriteXml("hex.xml", Wrap(
            Program("hex.img", "hex", "0x10", "0x1000", sectorSize: "0x200")));

        var plan = new RawProgramPlan();
        Assert.True(RawProgramParser.Parse(new[] { xml }, plan, out var error), error);

        Assert.Single(plan.Entries);
        Assert.Equal(512u, plan.Entries[0].SectorSize);
        Assert.Equal(16UL, plan.Entries[0].SectorCount);
        Assert.Equal(4096UL, plan.Entries[0].StartSector);
        Assert.Equal(8192UL, plan.TotalSourceBytes);
    }

    [Fact]
    public void Parse_DefersDynamicStartSectorToDevice()
    {
        WriteImage("dyn.img", 1024);

        // NUM_DISK_SECTORS-100. 这类别名要留给设备端算，不能当成 0 刷到 0 号扇区
        var xml = WriteXml("dyn.xml", Wrap(
            Program("dyn.img", "dyn", "2", "NUM_DISK_SECTORS-100.")));

        var plan = new RawProgramPlan();
        Assert.True(RawProgramParser.Parse(new[] { xml }, plan, out var error), error);

        Assert.Single(plan.Entries);
        Assert.False(plan.Entries[0].StartSectorResolved);
        Assert.Equal(0UL, plan.Entries[0].StartSector);
        Assert.Contains(plan.Warnings,
            w => w.Contains("动态起始扇区将在设备端解析：dyn (NUM_DISK_SECTORS-100.)", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData(true, "TDCO", 64)]    // "TDCO" + 64 = 68 字节 → 合格
    [InlineData(false, "XXCO", 64)]   // 魔数不对
    [InlineData(false, "TDCO", 60)]   // 长度不足 68
    public void Parse_ValidatesOcdtImage(bool expectKept, string magic, int extraBytes)
    {
        WriteImage("boot.img", 1024);
        var ocdt = new List<byte>(Encoding.ASCII.GetBytes(magic));
        ocdt.AddRange(new byte[extraBytes]);
        File.WriteAllBytes(Path.Combine(_directory, "ocdt.img"), ocdt.ToArray());

        var xml = WriteXml("ocdt.xml", Wrap(
            Program("boot.img", "boot", "2", "1000") +
            Program("ocdt.img", "ocdt", "1", "5000")));

        var plan = new RawProgramPlan();
        Assert.True(RawProgramParser.Parse(new[] { xml }, plan, out var error), error);

        if (expectKept)
        {
            Assert.Equal(2, plan.Entries.Count);
            Assert.Contains(plan.Entries, e => e.Label == "ocdt");
        }
        else
        {
            Assert.Single(plan.Entries);
            Assert.DoesNotContain(plan.Entries, e => e.Label == "ocdt");
            Assert.Contains(plan.Warnings, w => w.Contains("跳过无效 OCDT 镜像：ocdt.img", StringComparison.Ordinal));
        }
    }

    [Fact]
    public void Parse_DetectsSplitSuper()
    {
        WriteImage("super.img", 1024);
        WriteImage("super_b.img", 1024);
        WriteImage("boot.img", 1024);

        var single = WriteXml("one.xml", Wrap(
            Program("boot.img", "boot", "2", "1000") +
            Program("super.img", "super", "2", "2000")));
        var plan = new RawProgramPlan();
        Assert.True(RawProgramParser.Parse(new[] { single }, plan, out var e1), e1);
        Assert.Equal(1, plan.SuperEntryCount);
        Assert.False(plan.HasSplitSuper());

        var split = WriteXml("two.xml", Wrap(
            Program("boot.img", "boot", "2", "1000") +
            Program("super.img", "super", "2", "2000") +
            Program("super_b.img", "super_b", "2", "3000")));
        var plan2 = new RawProgramPlan();
        Assert.True(RawProgramParser.Parse(new[] { split }, plan2, out var e2), e2);
        Assert.Equal(2, plan2.SuperEntryCount);
        Assert.True(plan2.HasSplitSuper());
    }

    [Fact]
    public void Parse_DeduplicatesRepeatedXmlPath()
    {
        WriteImage("boot.img", 1024);
        var xml = WriteXml("dup.xml", Wrap(Program("boot.img", "boot", "2", "1000")));

        var plan = new RawProgramPlan();
        // 同一个 XML 被传两次（UI 复选 / 拖拽重复）→ 条目不能翻倍
        Assert.True(RawProgramParser.Parse(new[] { xml, xml }, plan, out var error), error);
        Assert.Single(plan.XmlPaths);
        Assert.Single(plan.Entries);
    }

    [Fact]
    public void Parse_RejectsEmptySelection()
    {
        var plan = new RawProgramPlan();
        Assert.False(RawProgramParser.Parse(Array.Empty<string>(), plan, out var error));
        Assert.Equal("尚未选择 rawprogram XML", error);
    }

    [Fact]
    public void Parse_RejectsMissingXmlFile()
    {
        var plan = new RawProgramPlan();
        var missing = Path.Combine(_directory, "nope.xml");
        Assert.False(RawProgramParser.Parse(new[] { missing }, plan, out var error));
        Assert.StartsWith("XML 文件不存在：", error, StringComparison.Ordinal);
    }

    [Fact]
    public void Parse_RejectsBadSectorSize()
    {
        WriteImage("boot.img", 1024);
        var xml = WriteXml("bad.xml", Wrap(
            "<program SECTOR_SIZE_IN_BYTES=\"abc\" filename=\"boot.img\" label=\"boot\"" +
            " num_partition_sectors=\"2\" physical_partition_number=\"0\" start_sector=\"1\"/>\n"));

        var plan = new RawProgramPlan();
        Assert.False(RawProgramParser.Parse(new[] { xml }, plan, out var error));
        Assert.Equal("bad.xml：program 的 SECTOR_SIZE_IN_BYTES 错误", error);
    }

    [Fact]
    public void Parse_RejectsMalformedXml()
    {
        var xml = WriteXml("broken.xml", "<?xml version=\"1.0\" ?>\n<data>\n<program>\n");
        var plan = new RawProgramPlan();
        Assert.False(RawProgramParser.Parse(new[] { xml }, plan, out var error));
        Assert.StartsWith("XML 解析失败：", error, StringComparison.Ordinal);
    }

    [Fact]
    public void Parse_RejectsFileSectorOffsetBeyondImage()
    {
        WriteImage("boot.img", 1024);
        var xml = WriteXml("offset.xml", Wrap(
            "  <program SECTOR_SIZE_IN_BYTES=\"512\" file_sector_offset=\"100\"" +
            " filename=\"boot.img\" label=\"boot\" num_partition_sectors=\"2\"" +
            " physical_partition_number=\"0\" sparse=\"false\" start_sector=\"1\"/>\n"));

        var plan = new RawProgramPlan();
        Assert.False(RawProgramParser.Parse(new[] { xml }, plan, out var error));
        Assert.Equal("镜像偏移超出文件：boot.img", error);
    }

    [Fact]
    public void Parse_RejectsXmlWithoutUsableEntries()
    {
        var xml = WriteXml("empty.xml", Wrap(Program("nothing.img", "nothing", "2", "1")));
        var plan = new RawProgramPlan();
        Assert.False(RawProgramParser.Parse(new[] { xml }, plan, out var error));
        Assert.Equal("所选 XML 中没有可刷写的 program 条目", error);
    }
}
