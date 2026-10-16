using KunFlashTool.Core.SwtEdl.Protocol;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 安全启动机型「认证前只收 nop / sig」这一 Firehose 错误的识别与文案回归。
///
/// <para>判据来源：设备真实回帧 <c>ERROR: Only nop and sig tag can be recevied before
/// authentication. (注意设备固件里 "recevied" 就在拼错，我们必须按原文匹配)</c>。
/// 命中后 <c>Configure</c> / 所有走 <c>CheckAck</c> 的命令都要给出**可执行**的提示，
/// 而不是笼统的「返回 NAK」——否则用户根本不知道要勾 OPPO VIP 走授权通道。</para>
/// </summary>
public class SwtEdlFirehoseAuthTests
{
    // 设备固件的原文（含其拼写错误 "recevied"）——匹配必须是「包含 + 忽略大小写」。
    private const string DeviceLine =
        "ERROR: Only nop and sig tag can be recevied before authentication.";

    private static FirehoseResponse NakWith(params string[] messages)
    {
        var response = new FirehoseResponse { Nak = true };
        foreach (var m in messages) response.Messages.Add(m);
        return response;
    }

    [Fact]
    public void IsAuthRequired_TrueForDeviceLine()
    {
        Assert.True(FirehoseClient.IsAuthRequired(NakWith(DeviceLine)));
    }

    [Theory]
    [InlineData("Only nop and sig tag can be recevied before authentication")]
    [InlineData("only nop and sig tag can be recevied before authentication.")]
    [InlineData("prefix ERROR: Only nop and sig tag can be recevied before authentication. suffix")]
    public void IsAuthRequired_TrueIgnoringCaseAndSurroundingText(string message)
    {
        Assert.True(FirehoseClient.IsAuthRequired(NakWith(message)));
    }

    [Fact]
    public void IsAuthRequired_TrueWhenMarkerIsOneOfSeveralMessages()
    {
        Assert.True(FirehoseClient.IsAuthRequired(NakWith("some log", DeviceLine, "another log")));
    }

    [Theory]
    [InlineData("NAK")]
    [InlineData("ERROR: Invalid attributes")]
    [InlineData("Only nop and sig tag can be received before authentication")] // 拼写"正确"反而不匹配设备原帧
    [InlineData("")]
    public void IsAuthRequired_FalseForUnrelatedMessages(string message)
    {
        Assert.False(FirehoseClient.IsAuthRequired(NakWith(message)));
    }

    [Fact]
    public void IsAuthRequired_FalseWhenNoMessages()
    {
        Assert.False(FirehoseClient.IsAuthRequired(new FirehoseResponse { Nak = true }));
    }

    [Fact]
    public void AuthRequiredMessage_CarriesOperationAndActionableGuidance()
    {
        var text = FirehoseClient.AuthRequiredMessage("Firehose Configure");

        Assert.Contains("Firehose Configure", text);
        Assert.Contains("签名认证", text);
        Assert.Contains("OPPO VIP", text);
        Assert.Contains("Digest", text);
        Assert.Contains("Sign", text);
    }

    [Fact]
    public void AuthRequiredMessage_IsNotBareNak()
    {
        var text = FirehoseClient.AuthRequiredMessage("读取存储信息");
        Assert.DoesNotContain("目标返回 NAK", text);
    }
}
