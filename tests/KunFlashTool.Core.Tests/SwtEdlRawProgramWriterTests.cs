using System;
using System.Collections.Generic;
using System.IO;
using KunFlashTool.Core.SwtEdl;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 移植层 <c>SwtEdl/Storage/RawProgramWriter.cs</c>（对应 <c>third_party/swt-edl/src/storage/RawProgramWriter.cpp</c>）的回归用例。
///
/// <para>这个类的输出是**要拿去给 QFIL 整包刷写**的，所以断言打在字节层面：
/// LUN 分组与生成顺序、属性顺序、4 空格缩进、CRLF 换行、XML 转义、以及"不留 .new 半截文件"。
/// 这些一旦漂了，用户拿到的 XML 在 QFIL 里读不出来，而本工具自己完全不会报错。</para>
/// </summary>
public class SwtEdlRawProgramWriterTests : IDisposable
{
    private readonly string _directory;

    public SwtEdlRawProgramWriterTests()
    {
        _directory = Path.Combine(Path.GetTempPath(), "swtedl-writer-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(_directory);
    }

    public void Dispose()
    {
        try { Directory.Delete(_directory, recursive: true); } catch { }
    }

    private static PartitionInfo Partition(string name, uint lun, ulong first, ulong last) =>
        new() { Name = name, Lun = lun, FirstLba = first, LastLba = last };

    [Fact]
    public void WriteBackup_GroupsByLun_InAscendingOrder()
    {
        var partitions = new List<PartitionInfo>
        {
            Partition("boot", 0, 1000, 1999),
            Partition("system_a", 0, 2000, 2999),
            Partition("vendor", 4, 100, 199),
        };
        var images = new List<string> { "boot.img", "system.img", "vendor.img" };
        var generated = new List<string>();

        Assert.True(RawProgramWriter.WriteBackup(_directory, partitions, images, 512,
                                                 generated, out var error), error);

        // QMap 有序 → 按 LUN 升序，而不是输入顺序（输入里 LUN 4 在最后，恰好也是升序；
        // 下面那条用例把顺序反过来，才能证明"排序"而不是"输入序"）。
        Assert.Equal(2, generated.Count);
        Assert.Equal(Path.Combine(_directory, "rawprogram0.xml"), generated[0]);
        Assert.Equal(Path.Combine(_directory, "rawprogram4.xml"), generated[1]);
        Assert.True(File.Exists(generated[0]));
        Assert.True(File.Exists(generated[1]));

        var text = File.ReadAllText(generated[0]);
        Assert.StartsWith("<?xml version=\"1.0\"?>\r\n<data>\r\n", text, StringComparison.Ordinal);
        Assert.EndsWith("</data>\r\n", text, StringComparison.Ordinal);
        Assert.DoesNotContain("\n\r", text, StringComparison.Ordinal);   // 不能出现裸 LF

        // 属性顺序 / 数值换算全部逐字符对齐
        Assert.Contains(
            "    <program" +
            " SECTOR_SIZE_IN_BYTES=\"512\"" +
            " file_sector_offset=\"0\"" +
            " filename=\"boot.img\"" +
            " label=\"boot\"" +
            " num_partition_sectors=\"1000\"" +
            " physical_partition_number=\"0\"" +
            " size_in_KB=\"500\"" +
            " sparse=\"false\"" +
            " start_byte_hex=\"0x7d000\"" +     // 1000 × 512 = 512000
            " start_sector=\"1000\"" +
            "/>\r\n", text, StringComparison.Ordinal);

        Assert.Contains(" start_byte_hex=\"0xfa000\"", text, StringComparison.Ordinal);  // 2000 × 512

        var lun4 = File.ReadAllText(generated[1]);
        Assert.Contains(" physical_partition_number=\"4\"", lun4, StringComparison.Ordinal);
        Assert.Contains(" num_partition_sectors=\"100\"", lun4, StringComparison.Ordinal);
        Assert.Contains(" size_in_KB=\"50\"", lun4, StringComparison.Ordinal);
        Assert.Contains(" start_byte_hex=\"0xc800\"", lun4, StringComparison.Ordinal);   // 100 × 512

        // 原子写入：临时文件不能残留
        Assert.All(generated, p => Assert.False(File.Exists(p + ".new")));
    }

    [Fact]
    public void WriteBackup_SortsLunIndependentlyOfInputOrder()
    {
        // 输入先给 LUN 5 再给 LUN 1 —— 输出文件名顺序必须是 1、5。
        var partitions = new List<PartitionInfo>
        {
            Partition("xbl", 5, 10, 19),
            Partition("boot", 1, 20, 29),
        };
        var generated = new List<string>();

        Assert.True(RawProgramWriter.WriteBackup(_directory, partitions,
                                                 new List<string> { "xbl.img", "boot.img" },
                                                 512, generated, out var error), error);

        Assert.Equal("rawprogram1.xml", Path.GetFileName(generated[0]));
        Assert.Equal("rawprogram5.xml", Path.GetFileName(generated[1]));
    }

    [Fact]
    public void WriteBackup_EscapesXmlSpecialCharacters()
    {
        var partitions = new List<PartitionInfo> { Partition("a&b\"c'd<e>", 0, 0, 0) };
        var generated = new List<string>();

        Assert.True(RawProgramWriter.WriteBackup(_directory, partitions,
                                                 new List<string> { "x.img" }, 512,
                                                 generated, out var error), error);

        var text = File.ReadAllText(generated[0]);
        Assert.Contains("label=\"a&amp;b&quot;c&apos;d&lt;e&gt;\"", text, StringComparison.Ordinal);
    }

    [Fact]
    public void WriteBackup_OverwritesPreviousContent()
    {
        var generated = new List<string>();
        var many = new List<PartitionInfo> { Partition("a", 0, 0, 9), Partition("b", 0, 10, 19) };
        Assert.True(RawProgramWriter.WriteBackup(_directory, many,
                                                 new List<string> { "a.img", "b.img" }, 512,
                                                 generated, out _));

        var few = new List<PartitionInfo> { Partition("a", 0, 0, 9) };
        Assert.True(RawProgramWriter.WriteBackup(_directory, few,
                                                 new List<string> { "a.img" }, 512,
                                                 generated, out var error), error);

        var text = File.ReadAllText(generated[0]);
        Assert.Contains("label=\"a\"", text, StringComparison.Ordinal);
        Assert.DoesNotContain("label=\"b\"", text, StringComparison.Ordinal);   // 旧的不能残留
    }

    [Fact]
    public void WriteBackup_RejectsInconsistentArguments()
    {
        var generated = new List<string>();
        var empty = new List<PartitionInfo>();
        var one = new List<PartitionInfo> { Partition("a", 0, 0, 9) };

        Assert.False(RawProgramWriter.WriteBackup(_directory, empty, new List<string>(), 512, generated, out var e1));
        Assert.Equal("生成 rawprogram XML 的参数异常", e1);

        // 分区数与镜像名数不一致 = 会把错误的镜像名配到错误的分区上，宁可失败
        Assert.False(RawProgramWriter.WriteBackup(_directory, one, new List<string>(), 512, generated, out var e2));
        Assert.Equal("生成 rawprogram XML 的参数异常", e2);

        Assert.False(RawProgramWriter.WriteBackup(_directory, one, new List<string> { "a.img" }, 0, generated, out var e3));
        Assert.Equal("生成 rawprogram XML 的参数异常", e3);
    }

    [Fact]
    public void WriteBackup_RejectsMissingOutputDirectory()
    {
        var generated = new List<string>();
        var missing = Path.Combine(_directory, "no-such-dir");
        Assert.False(RawProgramWriter.WriteBackup(missing,
                                                  new List<PartitionInfo> { Partition("a", 0, 0, 9) },
                                                  new List<string> { "a.img" }, 512,
                                                  generated, out var error));
        Assert.StartsWith("XML 输出目录不存在：", error, StringComparison.Ordinal);
        Assert.Empty(generated);
    }
}
