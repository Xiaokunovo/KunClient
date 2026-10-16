using System.Linq;
using KunFlashTool.App.Dialogs;
using Xunit;

namespace KunFlashTool.Core.Tests;

/// <summary>
/// 云端引导库索引解析：OPLUS 签名通道的整套字段（digest/signature）必须原样传到下载流程。
/// <para>
/// 服务端把 <c>&lt;机型&gt;</c> + <c>&lt;机型&gt;.digest</c> + <c>&lt;机型&gt;.sig</c> 归成一条
/// （见 server/lib/firehoseIndex.js），客户端点一次「下载引导」就要把三件一起下下来。
/// 这几个字段一旦在解析里被吃掉，整套下载就退化成只下 programmer —— 症状跟原来
/// 「签名通道用不了」一模一样，且只有真机才暴露，所以在这里钉住。
/// </para>
/// </summary>
public sealed class CloudLoaderIndexTests
{
    private const string AuthSetJson = """
        {
          "success": true,
          "data": [
            { "brand": "oppo", "model": "450", "name": "oppo/450", "size": 1048576,
              "digest": "oppo/450.digest", "digestSize": 128,
              "signature": "oppo/450.sig", "signatureSize": 64 },
            { "brand": "mi", "model": "865", "name": "mi/865", "size": 695344 }
          ],
          "count": 2
        }
        """;

    [Fact]
    public void ParseIndex_CarriesDigestAndSignatureNames()
    {
        var (success, items) = CloudLoaderPickerDialog.CloudLoaderItem.ParseIndex(AuthSetJson);

        Assert.True(success);
        Assert.Equal(2, items.Count);

        var oplus = items.Single(x => x.Name == "oppo/450");
        Assert.Equal("oppo/450.digest", oplus.Digest);
        Assert.Equal("oppo/450.sig", oplus.Signature);
        Assert.True(oplus.HasAuthSet);
        Assert.Equal("签名", oplus.ChannelText);
        Assert.Equal(1048576, oplus.Size);

        var plain = items.Single(x => x.Name == "mi/865");
        Assert.Equal("", plain.Digest);
        Assert.Equal("", plain.Signature);
        Assert.False(plain.HasAuthSet);
        Assert.Equal("通用", plain.ChannelText);
    }

    [Fact]
    public void ParseIndex_LegacyIndexWithoutAuthFields_StillParses()
    {
        // 老服务端（没归并签名表）返回的条目没有 digest/signature 字段 —— 不能因此整页空掉。
        const string legacy = """{"success":true,"data":[{"brand":"mi","model":"865","name":"mi/865","size":695344}]}""";

        var (success, items) = CloudLoaderPickerDialog.CloudLoaderItem.ParseIndex(legacy);

        Assert.True(success);
        var item = Assert.Single(items);
        Assert.Equal("mi/865", item.Name);
        Assert.Equal("", item.Digest);
        Assert.Equal("", item.Signature);
        Assert.False(item.HasAuthSet);
    }

    [Fact]
    public void ParseIndex_ServerFailure_ReturnsFailureWithEmptyList()
    {
        var (success, items) = CloudLoaderPickerDialog.CloudLoaderItem.ParseIndex(
            """{"success":false,"message":"boom"}""");

        Assert.False(success);
        Assert.Empty(items);
    }
}
