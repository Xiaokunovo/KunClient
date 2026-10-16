using KunFlashTool.Core.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 「用户名不得是数字」护栏（2026-09-29）。
///
/// 这条规则有三份实现必须**逐字对齐**，任何一份漂了都会出现「一边放行一边打回」：
///   · server/server.js        → <c>isAllDigitsUsername()</c> / <c>usernameError()</c>（真正的门槛）
///   · server/console/index.html → <c>usernameErr()</c>（控制台即时反馈）
///   · src/KunClient.Core/Services/UsernameRule.cs → 本文件测的这个（客户端即时反馈）
///
/// 🔴 之所以要有单测：**全角数字**（`１２３`）和**罗马数字**（`Ⅻ`）是这条规则最容易漏的部分。
/// 用 ASCII 的 `\d` / `[0-9]` / <c>char.IsDigit</c> 去判，它们会静默通过 —— 而用户提这条要求
/// 的目的就是「别让账号名看起来像编号」，全角数字看起来一样是编号。
/// 服务端只有「管理员建号」那条路径没有字符集校验，是唯一能让全角名漏出去的入口（探针里单独打过）。
/// </summary>
public sealed class UsernameRuleTests
{
    // ---------- 必须拦：纯数字（各种写法） ----------
    [Theory]
    [InlineData("123456")]        // 最常见
    [InlineData("0")]
    [InlineData("007")]           // 前导零仍是纯数字
    [InlineData("000000000000000000000000")]  // 极长
    [InlineData("１２３４５６")]   // 🔴 全角数字（Nd，JS 的 \d 判不出来）
    [InlineData("١٢٣")]           // 🔴 阿拉伯-印度数字（Nd）
    [InlineData("Ⅻ")]             // 🔴 罗马数字（Nl）—— char.IsDigit 会放过它
    [InlineData("²")]             // 🔴 上标（No）—— char.IsDigit 会放过它
    [InlineData("1２3")]          // 🔴 半角+全角数字混排：看着是数字，Unicode 上整串都是 Nd
                                  //    （写这个用例时我一开始以为它该放行 —— 实测才发现全角 ２
                                  //     的类别就是 Nd，所以「混宽度」不等于「混了非数字」）
    public void AllDigits_IsRejected(string name)
    {
        Assert.Equal(UsernameRule.AllDigitsMessage, UsernameRule.Error(name));
        Assert.True(UsernameRule.IsAllDigits(name), $"{name} 应被判定为纯数字");
    }

    // ---------- 必须放行：只要混进一个非数字字符 ----------
    [Theory]
    [InlineData("123456a")]
    [InlineData("a123456")]
    [InlineData("123_456")]
    [InlineData("123-456")]
    [InlineData("123.456")]
    [InlineData("123@456")]
    [InlineData("小坤123")]
    public void MixedContent_IsAllowed(string name)
    {
        Assert.Null(UsernameRule.Error(name));
        Assert.False(UsernameRule.IsAllDigits(name));
    }

    // ---------- 原有字符集规则不能被改坏 ----------
    [Theory]
    [InlineData("a")]             // 太短（<2）
    [InlineData("有 空格")]
    [InlineData("user!")]         // 不在白名单的符号
    [InlineData("hash#tag")]
    public void BadCharset_ReportsCharsetMessage_NotDigitsMessage(string name)
    {
        // 关键：报的必须是字符集那条 —— 如果这里返回「不能是纯数字」，
        // 说明校验顺序被改乱了，用户会收到误导性的原因。
        Assert.Equal(UsernameRule.CharsetMessage, UsernameRule.Error(name));
    }

    [Fact]
    public void TooLong_IsRejected()
    {
        Assert.Equal(UsernameRule.CharsetMessage, UsernameRule.Error(new string('a', 25)));
        Assert.Null(UsernameRule.Error(new string('a', 24)));   // 24 位是上限，必须放行
    }

    [Fact]
    public void Empty_IsNotTreatedAsAllDigits()
    {
        // 空值属于「没填」，由各表单的必填校验负责报「请输入用户名」。
        // 这里如果返回 true，用户会看到「不能是纯数字」这种驴唇不对马嘴的提示。
        Assert.False(UsernameRule.IsAllDigits(""));
        Assert.False(UsernameRule.IsAllDigits("   "));
        Assert.False(UsernameRule.IsAllDigits(null));
        Assert.Equal(UsernameRule.CharsetMessage, UsernameRule.Error(""));
    }

    [Fact]
    public void SurroundingWhitespace_IsTrimmedBeforeCheck()
    {
        // 与三个入口的实际行为一致（前端都先 .trim() 再送）：
        // " 123456 " 必须判为纯数字，否则加两个空格就绕过去了。
        Assert.True(UsernameRule.IsAllDigits(" 123456 "));
        Assert.Equal(UsernameRule.AllDigitsMessage, UsernameRule.Error(" 123456 "));
    }

    [Fact]
    public void SurrogatePlaneDigits_AreDetected()
    {
        // U+1D7CF（数学粗体 1）在补充平面，单个 char 判成 Surrogate 就会漏 —— 必须按 Rune 遍历。
        // JS 侧的 \p{Nd} 能识别它，两边要一致。
        const string mathBoldOne = "\uD835\uDFCF";
        Assert.True(UsernameRule.IsAllDigits(mathBoldOne), "补充平面的数字必须判为数字");
    }
}
