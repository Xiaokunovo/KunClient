using KunFlashTool.App.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 会员等级推断的回归护栏（2026-10-09 逆向报告 D4 之后加）。
///
/// 原状：<c>Level</c> 里写的是「不是 vip、也不是 svip → 一律按 vip」（外加一个恒假的 <c>1 == 0</c>）。
/// 动态实测：服务端下发 <c>vipLevel = "normal"</c> + 一个未来的 <c>vipExpireAt</c>，
/// 客户端 <c>Level</c> 返回 <b>vip</b>；再配合 <c>+99999-…</c> 扩展年份
/// （<c>TryParseExpire</c> 解析成 <c>DateTime.MaxValue</c>），显示成**永久 VIP**。
///
/// 影响界定（别夸大）：这只驱动 UI 显示与额度提示，真实授权仍在服务端
/// （<c>/api/app/check</c> + <c>/consume</c>，fail-closed）。但"服务端回什么客户端就显示什么 VIP"
/// 依然是要修的：它会让用户以为自己有权限，并把报障引到错误的方向。
///
/// 判据：断言打在**纯函数** <c>AccountText.EffectiveLevel</c> 上 —— 不碰
/// <c>MembershipService</c> 的静态状态，免得单测顺手发出真实 HTTP 请求。
/// </summary>
public sealed class VipLevelGuardTests
{
    [Theory]
    [InlineData("vip")]
    [InlineData("svip")]
    [InlineData("VIP")]        // 大小写 / 空格都不该影响判定
    [InlineData(" SvIp ")]
    public void EffectiveLevel_KeepsKnownLevels(string level)
        => Assert.Equal(level.Trim().ToLowerInvariant(), AccountText.EffectiveLevel(level, fromServer: true));

    /// <summary>核心断言：服务端**显式**下发 normal 时必须回落 normal（原来会升成 vip）。</summary>
    [Theory]
    [InlineData("normal")]
    [InlineData("SUPERVIP")]
    [InlineData("vip2")]
    [InlineData("")]
    [InlineData(null)]
    public void EffectiveLevel_UnknownOrNormalFallsBackToNormal(string? level)
        => Assert.Equal("normal", AccountText.EffectiveLevel(level, fromServer: true));

    /// <summary>兼容分支：服务端这次登录根本没下发等级字段（老接口 / 只有 vip_expire_at）→ 仍按 vip。</summary>
    [Fact]
    public void EffectiveLevel_LegacyAccountWithoutLevelField_StaysVip()
        => Assert.Equal("vip", AccountText.EffectiveLevel("normal", fromServer: false));

    /// <summary>
    /// 扩展年份写法必须仍然解析成「永久」（这是历史 bug 的正解，不能因为改等级就砍掉）；
    /// 同时钉住"永久"不等于"等级可伪造"——两者现在由 <c>EffectiveLevel</c> 分开裁决。
    /// </summary>
    [Theory]
    [InlineData("+99999-01-01")]
    [InlineData("+12345-06-06")]
    public void TryParseExpire_ExpandedYear_IsPermanent(string expire)
    {
        Assert.True(AccountText.TryParseExpire(expire, out var end));
        Assert.Equal(DateTime.MaxValue, end);
    }

    [Theory]
    [InlineData("garbage")]
    [InlineData("")]
    [InlineData(null)]
    public void TryParseExpire_Unparsable_IsFalse(string? expire)
        => Assert.False(AccountText.TryParseExpire(expire, out _));
}
