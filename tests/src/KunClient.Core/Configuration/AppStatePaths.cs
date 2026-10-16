using System;
using System.IO;

namespace KunFlashTool.Core.Configuration
{
    /// <summary>
    /// 「长期状态」目录：<c>%LOCALAPPDATA%\KunClientState</c>。
    /// <para>
    /// 为什么单独挪一个目录出来：退出清理会把 <c>%LOCALAPPDATA%\KunClient</c>
    /// **整个删掉**（用户要求关闭工具不留痕迹）。而里面混着两类东西——
    /// 一类是纯缓存（云端背景、云端驱动、scrcpy、platform-tools、WebView2 缓存），删了无感；
    /// 另一类是**必须跨会话存活**的状态：
    /// <list type="bullet">
    /// <item><c>settings.json</c> 里的 <c>LegalAcceptedVersion</c>：删了每次启动都要重新读一遍合规条款，不同意直接 Shutdown()。</item>
    /// <item><c>login.dat</c>：登录态，删了每次启动都要重新登录。</item>
    /// <item><c>KunClient.config.json</c>：登录框里改过的服务器地址，删了会静默回落到内置默认地址。</item>
    /// <item><c>logs\EdlLog*.txt</c>：EDL 原生日志（协议原文 + 引擎日志）。它是用户反馈
    ///   「刷机失败 / 读分区失败」时唯一的证据，写进被清理的目录等于刚写完就删。</item>
    /// </list>
    /// 所以把这些挪到本目录（**不在清理范围内**），其余继续留在被清理的 KunClient 目录里。
    /// </para>
    /// <para>
    /// 迁移是**只读兼容**：新位置没有文件时，仍然会去旧目录读一次，
    /// 这样老用户升级到本版本不会被强制重新登录 / 重新同意条款。
    /// </para>
    /// </summary>
    public static class AppStatePaths
    {
        /// <summary>状态目录名。注意与缓存目录名 <c>KunClient</c> 区分开——后者退出即删。</summary>
        public const string DirName = "KunClientState";

        /// <summary>被退出清理删除的旧缓存目录名，仅用于迁移读取。</summary>
        public const string LegacyDirName = "KunClient";

        /// <summary>更早的历史目录名（曾命名 KunFlashTool），仅用于迁移读取。</summary>
        public const string LegacyDirName2 = "KunFlashTool";

        private static string LocalAppData =>
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

        /// <summary>状态目录（<c>%LOCALAPPDATA%\KunClientState</c>），返回前确保已存在。</summary>
        public static string Root
        {
            get
            {
                var dir = Path.Combine(LocalAppData, DirName);
                try { Directory.CreateDirectory(dir); } catch { }
                return dir;
            }
        }

        /// <summary>状态目录下的文件路径。</summary>
        public static string Combine(string fileName) => Path.Combine(Root, fileName);

        /// <summary>
        /// 可复用**大件缓存**根目录：<c>%LOCALAPPDATA%\KunClientState\cache</c>。
        /// <para>🔴 2026-10-08（用户反馈「下载速度太慢了现在」）：platform-tools(adb/fastboot, 5.1MB)
        /// 与 scrcpy(11.3MB) 原来放在 <c>%TEMP%\KunClient</c> / <c>%LOCALAPPDATA%\KunClient</c>，
        /// 而 <c>App.OnExit</c> 会把这两个目录**整目录删掉** —— 等于**每次启动都要重下 16.4MB**。
        /// 单连接口径下那是每次启动等 ~48 秒（实测 0.34 MB/s），这才是「下载太慢」的主因，
        /// 光靠客户端多连接分片（8~20s）也救不回来。</para>
        /// <para>「关闭工具不留痕迹」的本意是不留**使用痕迹**（日志、配置、临时产物），
        /// 而 adb/fastboot/scrcpy 是下载重建的**公共工具链**：不含任何用户数据、对所有用户字节相同。
        /// 留着它不影响隐私，却省掉每次启动的十几 MB。所以这两样搬进本目录（不受退出清理影响），
        /// 其余（云端背景/驱动、刷机临时产物、crash.log）继续留在被清理的目录里。</para>
        /// </summary>
        public static string CacheRoot
        {
            get
            {
                var dir = Path.Combine(Root, "cache");
                try { Directory.CreateDirectory(dir); } catch { }
                return dir;
            }
        }

        /// <summary>大件缓存里的某个子目录（返回前确保已存在，不保证里面有东西）。</summary>
        public static string Cache(string name)
        {
            var dir = Path.Combine(CacheRoot, name);
            try { Directory.CreateDirectory(dir); } catch { }
            return dir;
        }

        /// <summary>
        /// 老位置（**退出即被清理**）下的同名缓存目录：<c>%TEMP%\KunClient\&lt;name&gt;</c>
        /// 与 <c>%LOCALAPPDATA%\KunClient\&lt;name&gt;</c>。只用于「升级时把已有工具搬过来，别白下一次」。
        /// </summary>
        public static string[] LegacyCaches(string name) => new[]
        {
            Path.Combine(Path.GetTempPath(), LegacyDirName, name),
            Path.Combine(LocalAppData, LegacyDirName, name),
            Path.Combine(Path.GetTempPath(), LegacyDirName2, name),
            Path.Combine(LocalAppData, LegacyDirName2, name),
        };

        /// <summary>
        /// 解析大件缓存目录，必要时从老位置**搬**过来（只搬一次；新位置已有就直接用）。
        /// <paramref name="probeFile"/> 是「这个目录算装好了」的判定文件
        /// （tools 用 <c>adb.exe</c>、scrcpy 用 <c>scrcpy.exe</c>）。
        /// 搬失败一律静默 —— 大不了照常重新下载，绝不能因为迁移失败让功能起不来。
        /// </summary>
        public static string ResolveCache(string name, string legacyName, string probeFile)
        {
            var target = Cache(name);
            try { if (File.Exists(Path.Combine(target, probeFile))) return target; } catch { }

            foreach (var legacy in LegacyCaches(legacyName))
            {
                try
                {
                    if (!File.Exists(Path.Combine(legacy, probeFile))) continue;
                    CopyDirectory(legacy, target);
                    return target;
                }
                catch { }
            }
            return target;
        }

        private static void CopyDirectory(string source, string target)
        {
            foreach (var dir in Directory.GetDirectories(source, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(target, Path.GetRelativePath(source, dir)));

            foreach (var file in Directory.GetFiles(source, "*", SearchOption.AllDirectories))
                File.Copy(file, Path.Combine(target, Path.GetRelativePath(source, file)), overwrite: true);
        }

        /// <summary>旧目录（会被退出清理删掉）下同名文件的路径，用于迁移读取。</summary>
        public static string Legacy(string fileName) =>
            Path.Combine(LocalAppData, LegacyDirName, fileName);

        /// <summary>更早历史目录下的同名文件路径，用于迁移读取。</summary>
        public static string Legacy2(string fileName) =>
            Path.Combine(LocalAppData, LegacyDirName2, fileName);

        /// <summary>
        /// 按「新位置 → 旧位置 → 更早位置」的顺序找第一个存在的文件。
        /// 找不到返回 null —— 调用方据此决定用默认值还是走迁移。
        /// </summary>
        public static string? FindExisting(string fileName)
        {
            foreach (var p in new[] { Combine(fileName), Legacy(fileName), Legacy2(fileName) })
            {
                try { if (File.Exists(p)) return p; } catch { }
            }
            return null;
        }
    }
}
