using System;
using System.Text;
using KunFlashTool.Core.SwtEdl;
using KunFlashTool.Core.SwtEdl.Storage;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 移植层 <c>SwtEdl/Storage/SystemVersionReader.cs</c>（对应 <c>third_party/swt-edl/src/storage/SystemVersionReader.cpp</c>）的回归用例。
///
/// <para>走的是 <c>RAW</c> 回退通道：既没有 ext4 超级块（1080 偏移不是 0xEF53）也没有
/// EROFS 魔数（1024 偏移不是 0xE0F5E1E2）时，实现会直接扫前 4 MiB 找 <c>ro.build.*</c> 文本。
/// 这条通道不需要构造真实文件系统，正好用来锁住 <c>Fill()</c> 的字段映射与 key 优先级。</para>
/// </summary>
public class SwtEdlSystemVersionReaderTests
{
    private const string BuildProp =
        "# 注释行应当被忽略\n" +
        "ro.product.brand=OnePlus\n" +
        "ro.product.model=KB2000\n" +
        "ro.product.device=kalama\n" +
        "ro.board.platform=kalama\n" +
        "ro.build.version.release=13\n" +
        "ro.build.version.security_patch=2024-03-01\n" +
        "ro.build.display.id=KB2000_13.1.0.500\n" +
        "ro.build.fingerprint=OnePlus/KB2000/kalama:13/UKQ1/1234:user/release-keys\n" +
        "ro.build.date=Fri Mar 1 10:00:00 CST 2024\n" +
        "ro.vendor.oplus.market.enname=OnePlus 8T\n" +
        "ro.vendor.oplus.regionmark=CN\n" +
        "   \n" +                       // 纯空白行
        "no-equals-sign-line\n";        // 没有 '=' 的行

    /// <summary>把一段文本铺进一个既非 ext4 也非 EROFS 的裸缓冲区里。</summary>
    private static byte[] RawPartition(string text, int size = 4096)
    {
        var buffer = new byte[size];
        var bytes = Encoding.UTF8.GetBytes(text);
        Array.Copy(bytes, buffer, Math.Min(bytes.Length, size));
        return buffer;
    }

    private static SystemVersionReader.PartitionByteReader Reader(byte[] buffer) =>
        (ulong offset, uint length, out string error) =>
        {
            error = string.Empty;
            if (offset >= (ulong)buffer.Length) return Array.Empty<byte>();
            var take = (int)Math.Min((ulong)length, (ulong)buffer.Length - offset);
            var slice = new byte[take];
            Array.Copy(buffer, (long)offset, slice, 0, take);
            return slice;
        };

    [Fact]
    public void ReadPhysical_ParsesRawPropertyBlock()
    {
        var buffer = RawPartition(BuildProp);
        var info = new SystemVersionInfo();

        Assert.True(SystemVersionReader.ReadPhysical(Reader(buffer), (ulong)buffer.Length,
                                                     info, out var error), error);

        Assert.Equal("OnePlus", info.Brand);
        Assert.Equal("KB2000", info.Model);
        Assert.Equal("kalama", info.Device);
        Assert.Equal("kalama", info.Platform);
        Assert.Equal("13", info.AndroidVersion);
        Assert.Equal("2024-03-01", info.SecurityPatch);
        Assert.Equal("KB2000_13.1.0.500", info.SystemVersion);   // ro.build.display.id
        Assert.Equal("OnePlus/KB2000/kalama:13/UKQ1/1234:user/release-keys", info.Fingerprint);
        Assert.Equal("Fri Mar 1 10:00:00 CST 2024", info.BuildDate);
        Assert.Equal("OnePlus 8T", info.MarketName);
        Assert.Equal("CN", info.Region);

        Assert.Contains("RAW", info.FileSystems);
        Assert.False(info.IsEmpty);

        // 注释行与无 '=' 的行不能进 Properties
        Assert.DoesNotContain("# 注释行应当被忽略", info.Properties.Keys);
        Assert.False(info.Properties.ContainsKey("no-equals-sign-line"));
    }

    [Fact]
    public void ReadPhysical_PrefersVendorFallbackKeys()
    {
        // Fill() 的 key 优先级：ro.product.* 缺失时要回落到 ro.product.odm.* / ro.product.vendor.*。
        // ⚠️ 必须带 ro.product.brand= 或 ro.build.version.release= —— RAW 回退通道靠这两个子串触发扫描。
        const string text =
            "ro.build.version.release=12\n" +
            "ro.product.brand=Realme\n" +
            "ro.product.vendor.model=RMX3081\n" +
            "ro.product.odm.device=RMX3081\n" +
            "ro.system.build.fingerprint=realme/RMX3081:12/S/1:user/release-keys\n";

        var buffer = RawPartition(text);
        var info = new SystemVersionInfo();

        Assert.True(SystemVersionReader.ReadPhysical(Reader(buffer), (ulong)buffer.Length,
                                                     info, out var error), error);

        Assert.Equal("Realme", info.Brand);                 // 首选取到
        Assert.Equal("RMX3081", info.Model);                // 走 ro.product.vendor.model
        Assert.Equal("RMX3081", info.Device);               // 走 ro.product.odm.device
        Assert.Equal("12", info.AndroidVersion);
        Assert.Equal("realme/RMX3081:12/S/1:user/release-keys", info.Fingerprint);  // 走 ro.system.build.fingerprint
    }

    [Fact]
    public void ReadPhysical_SkipsBlankValuesWhenChoosingFallback()
    {
        // 首选 key 存在但值为空 → 必须继续往下找，不能返回空串
        const string text =
            "ro.product.brand=\n" +
            "ro.product.vendor.brand=Meizu\n" +
            "ro.build.version.release=11\n";

        var buffer = RawPartition(text);
        var info = new SystemVersionInfo();

        Assert.True(SystemVersionReader.ReadPhysical(Reader(buffer), (ulong)buffer.Length,
                                                     info, out var error), error);

        Assert.Equal("Meizu", info.Brand);
    }

    [Fact]
    public void ReadPhysical_RejectsPartitionWithoutBuildProp()
    {
        var buffer = RawPartition("nothing useful here at all\n");
        var info = new SystemVersionInfo();

        Assert.False(SystemVersionReader.ReadPhysical(Reader(buffer), (ulong)buffer.Length,
                                                      info, out var error));
        Assert.Equal("分区中未识别到 build.prop", error);
        Assert.True(info.IsEmpty);
    }

    [Fact]
    public void ReadPhysical_RejectsNullResult()
    {
        Assert.False(SystemVersionReader.ReadPhysical(Reader(RawPartition(BuildProp)), 4096UL,
                                                      null!, out _));
    }
}
