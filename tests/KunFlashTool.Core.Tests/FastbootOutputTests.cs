using KunFlashTool.Core.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// `fastboot devices` 解析护栏（2026-09-29）。
///
/// 🔴 为什么必须有单测：这条 bug **编译期完全看不出来**。
///   `line.Split('\t', ' ', StringSplitOptions.RemoveEmptyEntries)` 看起来是「按 Tab 或空格拆」，
///   实际绑定到 <c>Split(char separator, int count, StringSplitOptions)</c> ——
///   第二个参数 <c>' '</c> 被**隐式转成 int 32**，语义变成「**只在 \t 上拆、最多 32 段**」。
///   空格分隔的回包只拆出 1 段 → `parts.Length &gt;= 2` 不成立 → 设备在列表里却报「未检测到」。
///
///   实机现场（用户截图）：日志明明打着
///   <code>a1940a31    fastboot</code>
///   下一行却是「ERROR 未检测到 Fastboot 设备」。0 错 0 警、评审也看不出来，
///   只有把「空格分隔」这一种输入钉成用例才拦得住。
///
/// 另外要守住的：不能把 <c>&lt; waiting for any device &gt;</c> 当成设备，
/// 以及 `adb devices` 那种「第二段是 device/unauthorized」的行不能误判成 fastboot 设备。
/// </summary>
public sealed class FastbootOutputTests
{
    // ---------- 🔴 核心：Tab 与空格**都必须**能拆出来 ----------

    [Theory]
    [InlineData("a1940a31\tfastboot\r\n")]          // 真机常见：Tab + CRLF
    [InlineData("a1940a31    fastboot\r\n")]        // 🔴 用户这台：多个**空格** + CRLF（老代码在这里挂）
    [InlineData("a1940a31 fastboot\n")]             // 单个空格 + LF
    [InlineData("a1940a31 \t fastboot \r\n")]       // 空格 Tab 混排 + 前后留白
    [InlineData("  a1940a31\tfastboot  ")]          // 首尾空白
    public void Serial_IsFound_WhateverTheSeparator(string output)
    {
        Assert.Equal("a1940a31", FastbootOutput.FirstFastbootSerial(output));
    }

    // ---------- 噪声行必须跳过 ----------

    [Theory]
    [InlineData("< waiting for any device >\na1940a31\tfastboot\n")]   // fastboot 等待提示
    [InlineData("< waiting for any device >\n\na1940a31  fastboot\n")] // 提示 + 空行 + 空格分隔
    [InlineData("\na1940a31 fastboot\n")]                               // 前置空行
    public void NoiseLine_IsSkipped(string output)
    {
        Assert.Equal("a1940a31", FastbootOutput.FirstFastbootSerial(output));
    }

    // ---------- 没有设备时必须是空串，不能硬凑一个出来 ----------

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("\n")]
    [InlineData("< waiting for any device >\n")]      // 只有等待提示
    [InlineData("a1940a31\n")]                        // 只有一段：不是设备行
    [InlineData("List of devices attached\n")]        // adb devices 的表头
    [InlineData("a1940a31\tdevice\n")]                // 🔴 adb 的形态：第二段是 device，不是 fastboot
    [InlineData("a1940a31\tunauthorized\n")]          // adb 未授权
    public void NoFastbootDevice_ReturnsEmpty(string? output)
    {
        Assert.Equal(string.Empty, FastbootOutput.FirstFastbootSerial(output));
    }

    // ---------- 多台设备取第一台 ----------

    [Fact]
    public void MultipleDevices_ReturnsFirst()
    {
        Assert.Equal("a1940a31",
            FastbootOutput.FirstFastbootSerial("a1940a31\tfastboot\nb2233b22\tfastboot\n"));
    }

    /// <summary>
    /// 第一行是 adb 形态、第二行才是 fastboot —— 必须跳过第一行取到第二行，
    /// 不能因为「第一行有两段」就返回 `a1940a31`。
    /// </summary>
    [Fact]
    public void AdbLineFirst_StillFindsFastbootLine()
    {
        Assert.Equal("b2233b22",
            FastbootOutput.FirstFastbootSerial("a1940a31\tdevice\nb2233b22\tfastboot\n"));
    }

    // ---------- 日志用的可见转义 ----------

    [Fact]
    public void Escaped_MakesWhitespaceVisible()
    {
        Assert.Equal("a1940a31\\tfastboot\\r\\n",
            FastbootOutput.Escaped("a1940a31\tfastboot\r\n"));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    public void Escaped_EmptyShowsPlaceholder(string? text)
    {
        Assert.Equal("（无输出）", FastbootOutput.Escaped(text));
    }
}
