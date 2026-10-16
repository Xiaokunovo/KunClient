using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using KunFlashTool.Core.SwtEdl;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 移植层 <c>SwtEdl/PartitionProtection.cs</c>（对应 <c>core/PartitionProtection.cpp</c>）的回归用例。
///
/// <para>保护名单决定「哪些分区绝不被擦写」——写漏一个就是变砖，写多一个就是刷机失败。
/// 所以除了逐条命中判定，还锁两条自洽性：名单里每一项**自己必须被判定为受保护**（防止
/// 归一化规则把某个名字改出名单），且名单**不能有重复**。</para>
/// </summary>
public class SwtEdlPartitionProtectionTests
{
    [Theory]
    [InlineData("persist", "persist")]
    [InlineData("persist_a", "persist")]                 // A/B 槽位后缀要被吃掉
    [InlineData("persist_b", "persist")]
    [InlineData("PERSIST", "persist")]                   // 大小写不敏感
    [InlineData("persist.img", "persist")]               // 镜像名当成分区名传入也要认
    [InlineData("modemst1.img", "modemst1")]
    [InlineData("a.tar.gz", "a.tar")]                    // 只剥最后一个扩展名
    public void NormalizedPartitionName_StripsSlotAndExtension(string input, string expected)
    {
        Assert.Equal(expected, PartitionProtection.NormalizedPartitionName(input));
    }

    [Fact]
    public void NormalizedPartitionName_StripsDirectory()
    {
        // 完整路径进来时只取文件名 —— 否则 "C:/persist/persist.img" 永远匹配不上名单
        Assert.Equal("persist",
            PartitionProtection.NormalizedPartitionName(Path.Combine("images", "persist.img")));
    }

    [Fact]
    public void NormalizedPartitionName_HandlesNullAndBlank()
    {
        Assert.Equal("", PartitionProtection.NormalizedPartitionName(null));
        Assert.Equal("", PartitionProtection.NormalizedPartitionName(""));
        Assert.Equal("", PartitionProtection.NormalizedPartitionName("   "));
    }

    [Theory]
    [InlineData("persist", true)]
    [InlineData("persistbak", true)]
    [InlineData("ocdt", true)]
    [InlineData("modemst1", true)]
    [InlineData("modemst2", true)]
    [InlineData("fsg", true)]
    [InlineData("oplusreserve4", true)]
    [InlineData("param", true)]
    [InlineData("preisp_otp", true)]
    [InlineData("modemst1_a", true)]       // 槽位后缀不影响受保护判定
    [InlineData("MODEMST2.IMG", true)]
    [InlineData("system", false)]
    [InlineData("boot", false)]
    [InlineData("vendor", false)]
    [InlineData("userdata", false)]
    public void IsProtectedPartitionName_MatchesPolicy(string name, bool expected)
    {
        Assert.Equal(expected, PartitionProtection.IsProtectedPartitionName(name));
    }

    [Fact]
    public void ProtectedList_IsSelfConsistentAndUnique()
    {
        var names = PartitionProtection.ProtectedPartitionNames();

        Assert.NotEmpty(names);
        Assert.Equal(names.Count, names.Distinct(StringComparer.Ordinal).Count());

        // 自洽性：名单里的每一项都必须在归一化之后仍然命中自己。
        // 这条能挡住「加了一个带 _a 后缀或大写字母的项，结果永远拦不住」这类静默失效。
        foreach (var name in names)
        {
            Assert.True(PartitionProtection.IsProtectedPartitionName(name),
                        $"保护名单里的 {name} 归一化后不再命中自己");
        }
    }

    [Fact]
    public void IsProtectedPartitionMapping_ChecksLabelAndFilename()
    {
        // 界面上一行是「分区名 + 待刷镜像名」两列，任一命中都算受保护
        Assert.True(PartitionProtection.IsProtectedPartitionMapping("boot", "persist.img"));
        Assert.True(PartitionProtection.IsProtectedPartitionMapping("persist", "system.img"));
        Assert.True(PartitionProtection.IsProtectedPartitionMapping("persist", null));
        Assert.False(PartitionProtection.IsProtectedPartitionMapping("boot", "system.img"));
        Assert.False(PartitionProtection.IsProtectedPartitionMapping("boot", ""));
        Assert.False(PartitionProtection.IsProtectedPartitionMapping(null, null));
    }
}

/// <summary>
/// 移植层 <c>SwtEdl/DeviceIdentity.cpp</c> 的回归用例：高通型号识别。
/// 型号认错 = 选错 programmer = 连不上或刷坏，所以连「并列时保留先出现的那个」这种
/// 看起来无关紧要的取舍也一并锁死（C++ 侧是严格大于才替换）。
/// </summary>
public class SwtEdlDeviceIdentityTests : IDisposable
{
    private readonly string _directory;

    public SwtEdlDeviceIdentityTests()
    {
        _directory = Path.Combine(Path.GetTempPath(), "swtedl-ident-" + Guid.NewGuid().ToString("n"));
        Directory.CreateDirectory(_directory);
    }

    public void Dispose()
    {
        try { Directory.Delete(_directory, recursive: true); } catch { }
    }

    [Theory]
    [InlineData("prog_firehose_ddr_sm8250.melf", "SM8250")]
    [InlineData("SM8250", "SM8250")]
    [InlineData("sm8250", "SM8250")]
    [InlineData("sm-8250", "SM8250")]
    [InlineData("sm_8250", "SM8250")]
    [InlineData("sdm 8450", "SDM8450")]         // 分隔符可省
    [InlineData("SDM845", "")]                  // 型号码至少 4 位，3 位不匹配
    [InlineData("MSM8998", "MSM8998")]
    [InlineData("APQ8096", "APQ8096")]
    [InlineData("SM7635", "SM7635")]            // 5 位型号
    [InlineData("no chip here", "")]
    [InlineData("ABC12345", "")]                // 不在 SM/MSM/SDM/APQ 前缀表里
    [InlineData("", "")]
    [InlineData(null, "")]
    public void NormalizeQualcommChipName_ExtractsChipCode(string? input, string expected)
    {
        Assert.Equal(expected, DeviceIdentity.NormalizeQualcommChipName(input));
    }

    [Theory]
    [InlineData("0x0c30e1", "SM8250")]
    [InlineData("0x000c30e1", "SM8250")]        // Sahara 常给 32 位，公开表按低 24 位索引
    [InlineData("0x00000000000c30e1", "SM8250")]
    [InlineData("28c0e1", "SM8550")]
    [InlineData("0x0c30e2", "")]                // 表外
    [InlineData("not-hex", "")]
    [InlineData("", "")]
    [InlineData(null, "")]
    public void QualcommChipFromMsmId_MasksToLow24Bits(string? input, string expected)
    {
        Assert.Equal(expected, DeviceIdentity.QualcommChipFromMsmId(input));
    }

    [Theory]
    [InlineData("SM8250", "SM8250 (Snapdragon 865)")]
    [InlineData("sm8250 lite", "SM8250 (Snapdragon 865)")]   // 先从整串里抽出型号再查营销名
    [InlineData("prog_sm8550.melf", "SM8550 (Snapdragon 8 Gen 2)")]
    [InlineData("SM9999", "SM9999")]                          // 已知型号但没有营销名 → 原样返回
    [InlineData("garbage", "garbage")]
    [InlineData("", "")]
    public void QualcommChipDisplayName_AppendsMarketingName(string? input, string expected)
    {
        Assert.Equal(expected, DeviceIdentity.QualcommChipDisplayName(input));
    }

    [Fact]
    public void DetectProgrammerChip_PrefersPathHints()
    {
        // VIP 会话里进命令态会吃掉 programmer 的看门狗窗口，只能靠路径线索认型号
        var chip = DeviceIdentity.DetectProgrammerChip(
            Path.Combine(_directory, "prog.melf"),
            new[] { "kunkun_sm8350_pack", "SM8150" });

        Assert.Equal("SM8350", chip);
    }

    [Fact]
    public void DetectProgrammerChip_FallsBackToPathThenBinary()
    {
        // 路径里没有型号 → 扫文件内容的 ASCII 元数据；这里 SM8250 出现 3 次、SM8150 2 次
        var path = Path.Combine(_directory, "prog.melf");
        var text = string.Concat(Enumerable.Repeat("SM8150 ", 2))
                 + string.Concat(Enumerable.Repeat("SM8250 ", 3));
        File.WriteAllText(path, text, Encoding.ASCII);

        Assert.Equal("SM8250", DeviceIdentity.DetectProgrammerChip(path));
    }

    [Fact]
    public void DetectProgrammerChip_KeepsFirstOnTie()
    {
        // C++ 侧是「严格更大才替换」：并列时必须保留先出现的，否则不同遍历顺序会给出不同结果
        var path = Path.Combine(_directory, "prog.melf");
        File.WriteAllText(path, "SM8150 SM8150 SM8250 SM8250", Encoding.ASCII);

        Assert.Equal("SM8150", DeviceIdentity.DetectProgrammerChip(path));
    }

    [Fact]
    public void DetectProgrammerChip_ReturnsEmptyWhenUnreadable()
    {
        // 文件不存在 / 打不开 → 静默返回空，不能抛（调用方在连接流程里，抛了就断）
        Assert.Equal("", DeviceIdentity.DetectProgrammerChip(Path.Combine(_directory, "missing.melf")));
    }
}

/// <summary>
/// <c>SwtEdl/Types.cs</c> 的回归用例：状态文案与 stateKey 的**完备性**。
///
/// <para>stateKey 是给外部 headless 驱动当协议值比对的，改一个既有 id 等于破坏兼容；
/// 新增状态忘了配文案则会显示成"未知状态"。这里用枚举遍历把两个方向都堵住。</para>
/// </summary>
public class SwtEdlTypesTests
{
    [Fact]
    public void StorageName_UsesFirehoseVocabulary()
    {
        Assert.Equal("ufs", EdlTypes.StorageName(StorageType.Ufs));
        Assert.Equal("emmc", EdlTypes.StorageName(StorageType.Emmc));
    }

    [Fact]
    public void StateTextAndKey_CoverEveryEnumValue()
    {
        foreach (SessionState state in Enum.GetValues(typeof(SessionState)))
        {
            var text = EdlTypes.StateText(state);
            var key = EdlTypes.StateKey(state);

            Assert.False(string.IsNullOrWhiteSpace(text), $"{state} 缺中文文案");
            Assert.NotEqual("未知状态", text);
            Assert.NotEqual("unknown", key);
            Assert.All(key.ToCharArray(), c => Assert.True(char.IsAsciiLetterOrDigit(c), $"{state} 的 stateKey 含非 ASCII 字符：{key}"));
        }
    }

    [Theory]
    [InlineData(SessionState.Disconnected, "未连接", "disconnected")]
    [InlineData(SessionState.Ready, "EDL 已就绪", "ready")]
    [InlineData(SessionState.OplusVipVerification, "OPPO VIP 验证", "oplusVipVerification")]
    [InlineData(SessionState.Failed, "操作失败", "failed")]
    public void StateTextAndKey_MatchStableContract(SessionState state, string text, string key)
    {
        Assert.Equal(text, EdlTypes.StateText(state));
        Assert.Equal(key, EdlTypes.StateKey(state));
    }

    [Fact]
    public void StateTextAndKey_FallBackForUndefinedValue()
    {
        var bogus = (SessionState)999;
        Assert.Equal("未知状态", EdlTypes.StateText(bogus));
        Assert.Equal("unknown", EdlTypes.StateKey(bogus));
    }

    [Fact]
    public void EdlCancel_IsResettable()
    {
        // 用 EdlCancel 而不是 CancellationToken 就是为了「每个新操作都能清掉上一轮的取消状态」
        var cancel = new EdlCancel();
        Assert.False(cancel.IsCancelled);

        cancel.Cancel();
        Assert.True(cancel.IsCancelled);

        cancel.Reset();
        Assert.False(cancel.IsCancelled);
    }
}
