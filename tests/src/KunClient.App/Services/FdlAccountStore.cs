using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace KunFlashTool.App.Services;

/// <summary>
/// 平台 FDL 账号的本地留存：**用户名 + 平台令牌**，不再存密码。
/// <para>
/// 🔴 **不写进程序集**。文档第六节明确要求"切勿提供管理员账号，否则等同交出管理后台权限"，
/// 也就是说这个账号是**可随时停用/改密**的对外凭据 —— 硬编码进 exe 就等于把凭据发出去，
/// 混淆与加壳都挡不住（值必须在运行时出现）。
/// 所以：用户在对话框里填一次，用 <see cref="ProtectedData"/>（DPAPI CurrentUser）加密落盘，
/// 换个 Windows 用户/机器就解不开。
/// </para>
/// <para>
/// 🔴🔴 **2026-10-09 起不再落盘密码**（逆向报告 §8.4 / D7 之后改，与 KunClient 侧同口径）：
/// 原来是「用户名 + 明文密码」直接存，DPAPI(CurrentUser) + 固定 entropy 保护。
/// 但 DPAPI CurrentUser 对**以该用户身份运行的任何代码**都不构成防护 ——
/// 而这恰恰是刷机/EDL 工具的威胁模型（用户会主动运行来路不明的刷机包/驱动/一键工具）。
/// 现在只存平台返回的 token（24h 有效）与到期时间；过期后提示用户重填密码，
/// 不再"为了少输一次密码"把长期有效的密码留在磁盘上。
/// </para>
/// <para>
/// 落盘位置用 <c>AppStatePaths</c>（<c>%LOCALAPPDATA%\KunClientState</c>）——
/// 那个目录**不会**被"退出清理"删掉，否则每次启动都要重填。
/// </para>
/// </summary>
public static class FdlAccountStore
{
    private static string StorageFile =>
        KunFlashTool.Core.Configuration.AppStatePaths.Combine("fdl-account.dat");

    private static readonly byte[] Entropy = Encoding.Unicode.GetBytes("KunClientFdl2026");

    /// <summary>
    /// 保存账号。密码参数**只用于兼容旧调用点，不再落盘**。
    /// <para>
    /// 想留住会话请传 <paramref name="token"/> 与 <paramref name="expiryUtc"/>（平台登录返回的令牌）。
    /// </para>
    /// </summary>
    public static void Save(string username, string password = "", string token = "", DateTime? expiryUtc = null)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(username))
            {
                Clear();
                return;
            }
            // 🔴 Password 恒写空串：这个字段还在只是为了兼容**已存在**的 fdl-account.dat
            // （反序列化时缺字段会失败）。它不再被写入任何真实密码。
            Write(new AccountData
            {
                Username = username.Trim(),
                Password = "",
                Token = token ?? "",
                ExpiryUtc = expiryUtc ?? DateTime.MinValue,
            });
        }
        catch { }
    }

    /// <summary>加密写盘。单独抽出来是为了让 <see cref="Load"/> 里的迁移也能复用同一条写路径。</summary>
    private static void Write(AccountData data)
    {
        var json = JsonSerializer.Serialize(data);
        var dir = Path.GetDirectoryName(StorageFile);
        if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(StorageFile, Convert.ToBase64String(
            ProtectedData.Protect(Encoding.Unicode.GetBytes(json), Entropy, DataProtectionScope.CurrentUser)));
    }

    /// <summary>读回账号；读不到（没存过 / 换了机器 / DPAPI 解不开）返回 null。</summary>
    public static AccountData? Load()
    {
        try
        {
            var file = KunFlashTool.Core.Configuration.AppStatePaths.FindExisting("fdl-account.dat");
            if (file == null || !File.Exists(file)) return null;
            var bytes = Convert.FromBase64String(File.ReadAllText(file).Trim());
            var json = Encoding.Unicode.GetString(
                ProtectedData.Unprotect(bytes, Entropy, DataProtectionScope.CurrentUser));
            var data = JsonSerializer.Deserialize<AccountData>(json);
            if (data == null || string.IsNullOrWhiteSpace(data.Username)) return null;

            // 迁移：老版本存过明文密码 → 立刻重写一份不含密码的。
            // 不然"改了代码"但磁盘上那份旧文件永远留着密码，等于没修。
            if (!string.IsNullOrEmpty(data.Password))
            {
                data.Password = "";
                try { Write(data); } catch { }
            }
            return data;
        }
        catch
        {
            return null;
        }
    }

    public static void Clear()
    {
        try { if (File.Exists(StorageFile)) File.Delete(StorageFile); } catch { }
    }

    public class AccountData
    {
        public string Username { get; set; } = "";

        /// <summary>已废弃：**永远为空**。保留字段只为兼容旧版 fdl-account.dat 的反序列化。</summary>
        public string Password { get; set; } = "";

        /// <summary>平台登录返回的令牌（24h 有效）。</summary>
        public string Token { get; set; } = "";

        /// <summary>令牌到期时间（UTC）；MinValue = 没有可用令牌。</summary>
        public DateTime ExpiryUtc { get; set; } = DateTime.MinValue;
    }
}
