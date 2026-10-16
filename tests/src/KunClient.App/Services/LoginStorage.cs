using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace KunFlashTool.App.Services
{
    public static class LoginStorage
    {
        // 登录态必须放在**不被退出清理删除**的状态目录里（见 AppStatePaths 注释）：
        // %LOCALAPPDATA%\KunClient 整个目录会在关闭工具时被删掉，
        // 放那里 = 每次启动都要重新登录。
        private static string StorageFile =>
            KunFlashTool.Core.Configuration.AppStatePaths.Combine("login.dat");

        private static readonly byte[] Entropy = Encoding.Unicode.GetBytes("KunClient2024");

        /// <summary>
        /// 保存登录态。**不保存密码**。
        ///
        /// 🔴 2026-10-09（源码审计后收紧）：原来会把明文密码一起序列化进 <c>login.dat</c>，
        /// 外面套一层 <c>DPAPI(CurrentUser)</c> —— 但 CurrentUser 作用域对「以该用户身份运行的
        /// **任何**进程」都是透明的，而刷机工具的威胁模型恰恰是「本机上还有别人的程序」。
        /// 也就是说那层保护只挡得住记事本，挡不住任何能跑代码的东西。
        /// 现在只保存 <c>用户名 + token + 到期时间</c>：下次启动用 token 免密进入，
        /// token 失效由服务端 401 兜住，重新输一次密码即可。
        /// 代价是登录框不再自动填密码 —— 这是刻意的取舍（见 LoadCredentials 的注释）。
        /// </summary>
        /// <param name="persistLogin">
        /// 是否把登录态落盘。false = 只留在内存（对应界面上不再勾选「记住登录状态」），
        /// 此时会顺手<b>清掉</b>磁盘上已有的凭据，避免"这次没勾、下次却自动填出来"。
        /// </param>
        public static void SaveCredentials(string username, string token, DateTime expiry,
                                           bool persistLogin = true)
        {
            try
            {
                if (!persistLogin)
                {
                    // 不勾选：登录态只留在内存（AuthSession），磁盘上不留任何东西。
                    ClearCredentials();
                    return;
                }

                var data = new SavedLoginData
                {
                    Username = username,
                    Password = "",          // ← 恒为空（见方法注释）。老版本写在盘上的密码会被这次覆盖抹掉。
                    Token = token,
                    Expiry = expiry
                };

                var json = JsonSerializer.Serialize(data);
                var encrypted = ProtectData(json);

                var dir = Path.GetDirectoryName(StorageFile);
                if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir))
                    Directory.CreateDirectory(dir);

                File.WriteAllText(StorageFile, encrypted);
            }
            catch { }
        }

        public static SavedLoginData? LoadCredentials()
        {
            // 迁移读取：新状态目录 → 旧 KunClient 目录（会被退出清理删掉）。
            // 老用户升级到本版本时登录态还在旧目录，这里能接着用，不会被强制重新登录。
            var file = KunFlashTool.Core.Configuration.AppStatePaths.FindExisting("login.dat");
            if (file == null || !File.Exists(file))
                return null;

            try
            {
                var encrypted = File.ReadAllText(file);
                var json = UnprotectData(encrypted);
                var data = JsonSerializer.Deserialize<SavedLoginData>(json);

                if (data == null)
                {
                    ClearCredentials();
                    return null;
                }

                return data;
            }
            catch
            {
                ClearCredentials();
                return null;
            }
        }

        public static void ClearCredentials()
        {
            // 🔴 三级路径都要删：LoadCredentials 走 FindExisting（新 → 旧 → 更旧），
            // 只删新路径的话旧 login.dat 会被读回来 = 退出登录后凭据"复活"。
            var paths = new[]
            {
                KunFlashTool.Core.Configuration.AppStatePaths.Combine("login.dat"),
                KunFlashTool.Core.Configuration.AppStatePaths.Legacy("login.dat"),
                KunFlashTool.Core.Configuration.AppStatePaths.Legacy2("login.dat"),
            };
            foreach (var p in paths)
            {
                try { if (File.Exists(p)) File.Delete(p); } catch { }
            }
        }

        private static string ProtectData(string data)
        {
            var bytes = Encoding.Unicode.GetBytes(data);
            var encrypted = ProtectedData.Protect(bytes, Entropy, DataProtectionScope.CurrentUser);
            return Convert.ToBase64String(encrypted);
        }

        private static string UnprotectData(string encryptedData)
        {
            var bytes = Convert.FromBase64String(encryptedData);
            var decrypted = ProtectedData.Unprotect(bytes, Entropy, DataProtectionScope.CurrentUser);
            return Encoding.Unicode.GetString(decrypted);
        }
    }

    public class SavedLoginData
    {
        public string Username { get; set; } = "";

        /// <summary>
        /// ⚠️ **已废弃**（2026-10-09）。保留字段只是为了能反序列化老版本写下的 login.dat
        /// （删字段会让老文件整个解析失败，用户被迫重新登录）。现在写入时恒为空串，
        /// 读取时调用方也必须忽略它 —— 见 <see cref="LoginStorage.SaveCredentials"/> 的说明。
        /// </summary>
        public string Password { get; set; } = "";

        public string Token { get; set; } = "";
        public DateTime Expiry { get; set; }
    }
}
