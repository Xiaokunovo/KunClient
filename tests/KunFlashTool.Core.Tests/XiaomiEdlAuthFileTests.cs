using System;
using KunFlashTool.Core.Services;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 小米 EDL 鉴权文件（`fastboot stage` 的那个 bin）格式护栏（2026-09-29）。
///
/// 🔴 这条被用户纠正过**两次**，是最容易改回去的地方：
///   ① 以为「文件格式是 bin」= 随便什么二进制 → 写成了 `blob + sig` 拼接（292 字节）；
///   ② 用户点明「**bin 文件只有 sign**」。
///
/// 实机证据（`blob + sig`）：设备回
/// <code>FAILED (remote: 'Edl Token Verify Failed')</code>
/// 原因：设备**自己**持有它刚通过 `getvar token` 发出去的那枚 token，
/// 它要的只是「这枚 token 的签名」。前面垫 36 字节 = 整个签名错位，必然验签失败。
///
/// 所以这里钉死：**产出就是入参 sig 本身，一个字节都不多、不少、不重排。**
/// 任何「顺手把 blob 也带上，万一有用呢」的想法都会被这几条断言打回。
/// </summary>
public sealed class XiaomiEdlAuthFileTests
{
    /// <summary>造一段像真签名的 256 字节（RSA-2048）数据。</summary>
    private static byte[] FakeSignature(int n = 256)
    {
        var b = new byte[n];
        for (int i = 0; i < n; i++) b[i] = (byte)(i * 7 + 1);
        return b;
    }

    // ---------- 🔴 核心：输出 == sig，不含 blob ----------

    [Fact]
    public void Build_IsExactlyTheSignature()
    {
        var sig = FakeSignature();
        var payload = XiaomiEdlAuthFile.Build(sig);

        Assert.Equal(sig.Length, payload.Length);          // 不多一个字节
        Assert.Equal(sig, payload);                        // 内容逐字节相同
    }

    /// <summary>
    /// **回归护栏**：如果哪天有人把 blob 又拼回去，长度会变成 `36 + 256`，这条会立刻炸。
    /// 用的是实机那串 token 解出来的真实字节数（48 个 base64 字符 = 36 字节）。
    /// </summary>
    [Fact]
    public void Build_DoesNotPrependTheBlob()
    {
        const int realBlobBytes = 36;      // 实机 `VQEBIAEQ1idRIwAAAAAAAAAAAAAAAAMGYWxpb3RoAgTxGPsi` → 36 B
        var sig = FakeSignature();
        var payload = XiaomiEdlAuthFile.Build(sig);

        Assert.NotEqual(realBlobBytes + sig.Length, payload.Length);
        Assert.Equal(sig.Length, payload.Length);
    }

    /// <summary>
    /// 文件开头必须就是签名开头。实机踩坑时的对比：
    /// 错的（带 blob）开头是 `55010120 0110d627 5123…`（= token 的魔数 + 设备码），
    /// 对的应该直接是签名自己。
    /// </summary>
    [Fact]
    public void Build_HeaderIsNotTheTokenMagic()
    {
        var sig = FakeSignature();
        var payload = XiaomiEdlAuthFile.Build(sig);

        // token 的魔数 / 特征（实机 blob 前 4 字节）
        var tokenMagic = new byte[] { 0x55, 0x01, 0x01, 0x20 };
        Assert.False(payload.Length >= 4 &&
                     payload[0] == tokenMagic[0] && payload[1] == tokenMagic[1] &&
                     payload[2] == tokenMagic[2] && payload[3] == tokenMagic[3],
                     "文件开头是 token 魔数 = blob 又被拼回去了");
    }

    /// <summary>返回的是副本：改了产出不该反过来改动入参。</summary>
    [Fact]
    public void Build_ReturnsCopy_NotSameArray()
    {
        var sig = FakeSignature();
        var payload = XiaomiEdlAuthFile.Build(sig);

        Assert.NotSame(sig, payload);
        payload[0] ^= 0xFF;
        Assert.NotEqual(payload[0], sig[0]);
    }

    [Fact]
    public void Build_Null_Throws()
    {
        Assert.Throws<ArgumentNullException>(() => XiaomiEdlAuthFile.Build(null!));
    }

    // ---------- 尺寸预警 ----------

    [Fact]
    public void SizeWarning_CorrectLength_IsSilent()
    {
        Assert.Null(XiaomiEdlAuthFile.SizeWarning(XiaomiEdlAuthFile.ExpectedSignatureBytes));
        Assert.Null(XiaomiEdlAuthFile.SizeWarning(256));
    }

    [Theory]
    [InlineData(128)]     // RSA-1024：长度不对
    [InlineData(292)]     // 🔴 `blob + sig` 那次的长度 —— 预警必须点出来
    [InlineData(512)]     // 太长
    [InlineData(1)]
    public void SizeWarning_WrongLength_SpeaksUp(int n)
    {
        var w = XiaomiEdlAuthFile.SizeWarning(n);
        Assert.NotNull(w);
        Assert.Contains(n.ToString(), w);          // 文案里要带上实际字节数
        Assert.Contains("256", w);                 // 也要给出期望值
    }

    [Theory]
    [InlineData(0)]
    [InlineData(-1)]
    public void SizeWarning_Empty_IsCalledOut(int n)
    {
        Assert.NotNull(XiaomiEdlAuthFile.SizeWarning(n));
    }
}
