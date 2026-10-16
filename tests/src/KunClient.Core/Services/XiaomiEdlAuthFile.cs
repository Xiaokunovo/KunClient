using System;
using System.Linq;

namespace KunFlashTool.Core.Services
{
    /// <summary>
    /// 小米 EDL 鉴权文件（`fastboot stage &lt;文件&gt;` 的那个文件）的**唯一构造点**（2026-09-29）。
    ///
    /// 🔴 **文件里只有 sig，没有 blob。**
    ///
    /// 这条被纠正过两次，别改回去：
    ///   · 第一次以为「文件格式是 bin」= 随便什么二进制，于是把 `blob + sig` 直接拼起来（292 字节）；
    ///   · 第二次用户点明「**bin 文件只有 sign**」。
    ///
    /// 为什么不能垫 blob：设备**自己**就持有它刚通过 `getvar token` 发出去的那串 token，
    /// 它要的只是「**这枚 token 的签名**」。在前面垫 36 字节等于把签名整体错位，
    /// 设备读到的就不再是签名 —— 实机回的是：
    /// <code>FAILED (remote: 'Edl Token Verify Failed')</code>
    ///
    /// blob 在整个流程里的作用只剩一条：**复制出去给签名端，换回 sig**。它不进文件。
    ///
    /// 对照 FDL 计算端（「FDL 计算端 - By 东灼」，逆向见项目记忆 §7）：
    /// `getvar token` → 本地算出 `Sign.data` → `stage Sign.data` → `oem edl`。
    /// 那个 `Sign.data` 就是**纯签名**。
    /// </summary>
    public static class XiaomiEdlAuthFile
    {
        /// <summary>
        /// 小米 EDL 签名长度（RSA-2048）。文件大小对不上时几乎必然验签失败，
        /// 所以流程里会拿它给一句预警 —— 但不阻断（不同代际的签名长度理论上可能变）。
        /// </summary>
        public const int ExpectedSignatureBytes = 256;

        /// <summary>
        /// 构造要交给 `fastboot stage` 的文件内容：**就是 sig 本身**，原样、无头、不拼 blob。
        /// 返回的是副本，调用方拿去落盘不会影响入参。
        /// </summary>
        public static byte[] Build(byte[] signature)
        {
            if (signature == null) throw new ArgumentNullException(nameof(signature));
            return signature.ToArray();
        }

        /// <summary>
        /// 尺寸预警：长度不是 <see cref="ExpectedSignatureBytes"/> 时给一句人话，否则返回 null。
        /// 不抛异常 —— 只提醒，用户仍然可以试。
        /// </summary>
        public static string? SizeWarning(int signatureBytes)
        {
            if (signatureBytes == ExpectedSignatureBytes) return null;
            if (signatureBytes <= 0) return "sig 是空的 —— 没有签名就没法进 EDL。";

            return $"注意：sig 是 {signatureBytes} 字节，而小米 EDL 的签名通常是 "
                   + $"{ExpectedSignatureBytes} 字节（RSA-2048）。尺寸不对大概率过不了设备的校验 —— "
                   + "确认粘贴的是**这一台设备当前 blob** 的签名，而不是别的东西。";
        }
    }
}
