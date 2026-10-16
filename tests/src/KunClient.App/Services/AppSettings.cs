using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace KunFlashTool.App.Services
{
    public class AppSettings
    {
        // 设置文件必须放在**不被退出清理删除**的状态目录里（见 AppStatePaths 注释）：
        // %LOCALAPPDATA%\KunClient 整个目录会在关闭工具时被删掉，
        // 而 settings.json 里的 LegalAcceptedVersion 一旦丢失，
        // 每次启动 App.OnStartup 都会重新弹合规条款（不同意直接 Shutdown()）。
        private static string SettingsDir => KunFlashTool.Core.Configuration.AppStatePaths.Root;
        private static string SettingsPath => KunFlashTool.Core.Configuration.AppStatePaths.Combine("settings.json");

        private static AppSettings? _current;
        public static AppSettings Current => _current ??= Load();

        public string BackgroundImagePath { get; set; } = "";
        public string BackgroundAnimation { get; set; } = "KenBurns";
        public double BackgroundOpacity { get; set; } = 0.35;

        /// <summary>
        /// 背景「真实模糊」强度（像素半径，0 = 不模糊）。
        /// 玻璃面板的磨砂感来自它：壁纸先被这支效果模糊，再透过半透明面板露出来。
        /// 视频 / GIF 背景逐帧变，逐帧重模糊是纯浪费，一律自动降级为不模糊
        /// （见 <c>DynamicBackground.SyncBackdropBlur</c>）；软件渲染（无 GPU 加速）同样跳过。
        /// </summary>
        public double BackgroundBlur { get; set; } = 24;

        /// <summary>
        /// 背景显示方式：Fill = 等比放大裁剪铺满窗口（默认，无留白）；
        /// Uniform = 等比完整显示（可能上下或左右留空）；Stretch = 拉伸填满（可能变形）。
        /// </summary>
        public string BackgroundFit { get; set; } = "Fill";

        /// <summary>卡片界面不透明度（1.0 = 不透明，越小越透出动态背景）</summary>
        public double CardOpacity { get; set; } = 0.86;

        /// <summary>
        /// 界面整体透明度（全局，0 = 完全不透明看不到背景，1 = 全透明完全透出背景）。
        /// 对应页面根背景 <c>Theme.BgTint</c> 的 alpha —— 此前该 alpha 在
        /// DesignTokens.xaml / ThemeManager.cs 里写死（浅色 0x9E、深色 0xAB），
        /// 用户无法调节。
        /// 默认改为 1.0（页面不再盖白底）：之前默认 0.38 会在所有页面上糊一层
        /// 62% 不透明的白，把动态背景整个挡住——用户反馈的「页面上层那层白色」就是它。
        /// 想要白底可在「设置 → 全局界面透明度」里往回调。
        /// </summary>
        public double UiTransparency { get; set; } = 1.0;

        /// <summary>
        /// 配置结构版本。<c>&lt; 2</c> 表示这份配置产生在「页面默认盖一层白底」的年代，
        /// 升级时要把那层白底撤掉（见 <see cref="Migrate"/>）。
        /// 默认值必须是 1 而不是 2：老配置文件里根本没有这个字段，反序列化时
        /// 属性初始值会生效——写成 2 的话老配置会被误判成「已升级」，迁移直接跳过。
        /// </summary>
        public int SettingsVersion { get; set; } = 1;

        /// <summary>上次选引导程序用过的目录，下次打开选择器直接落在这里。</summary>
        public string LastLoaderDir { get; set; } = "";

        /// <summary>
        /// 侧栏显示形态：<c>"expanded"</c>（默认：图标 + 文字，宽 200）或
        /// <c>"collapsed"</c>（仅图标，宽 72，给内容区让出 128px）。
        /// 由侧栏底部的「文字 / 图标」缩放开关控制，见 <c>MainWindow.ApplySidebarMode</c>。
        /// 老配置没有该字段 → 反序列化后取初始值 "expanded"，行为不变。
        /// </summary>
        public string SidebarMode { get; set; } = "expanded";

        // ── 小米 EDL 通道（流程里的 fastboot 命令，改完记住） ──
        // 这些都是**用户可编辑**的命令，不是写死的机型方案：
        // 各机型 bootloader 的命令支持度不一样，默认值只是占位，用户改过之后以他的为准。
        /// <summary>① 读 blob（= 设备的 token）的 fastboot 命令（不含 fastboot 本体）。
        /// 参考 FDL 计算端：fastboot getvar token。</summary>
        public string XiaomiEdlReadCommand { get; set; } = "getvar token";

        /// <summary>
        /// ② <b>已废弃，不再被读取。</b>原来表示「blob 与 sig 的拼接顺序」，
        /// 但 2026-09-29 实机确认 **stage 的文件里只有 sig、不含 blob**，也就没有顺序可言。
        /// 键保留是为了让老 <c>settings.json</c> 少一个「找不到的字段」，改成什么都无效。
        /// </summary>
        public int XiaomiEdlPackOrder { get; set; } = 0;

        /// <summary>③ 把签名文件送进设备暂存区的命令（fastboot stage，不写任何分区）。</summary>
        public string XiaomiEdlWriteCommand { get; set; } = "stage";

        /// <summary>④ 进入 EDL 的 fastboot 命令。</summary>
        public string XiaomiEdlEdlCommand { get; set; } = "oem edl";

        /// <summary>
        /// 用户已同意的法务文本版本（免责声明 / 用户协议 / 隐私政策）。
        /// 与 <c>DisclaimerDialog.LegalVersion</c> 比较：小于它就在启动时强制重新阅读并同意。
        /// 默认 0 = 从未同意过（老配置文件没有该字段，反序列化后就是这个初始值）。
        /// </summary>
        public int LegalAcceptedVersion { get; set; } = 0;

        private static AppSettings Load()
        {
            try
            {
                // 新位置 → 旧 KunClient 目录 → 更早的 KunFlashTool 目录：老用户升级后设置不丢
                var path = KunFlashTool.Core.Configuration.AppStatePaths.FindExisting("settings.json");

                if (path != null && File.Exists(path))
                {
                    var json = File.ReadAllText(path);
                    var loaded = JsonSerializer.Deserialize<AppSettings>(json) ?? new AppSettings();
                    Migrate(loaded);
                    return loaded;
                }
            }
            catch { }
            return new AppSettings();
        }

        /// <summary>
        /// 老配置升级：v1 的界面透明度默认（也常被手动调到）会在页面上盖一层白底，
        /// 把动态背景糊住。用户要的就是撤掉这层，所以升级到 v2 时统一设为 1.0（全透出）。
        /// 之后用户在「设置 → 全局界面透明度」里再怎么调都以他的值为准，不会被改回去。
        /// </summary>
        private static void Migrate(AppSettings s)
        {
            if (s.SettingsVersion < 2)
            {
                s.UiTransparency = 1.0;
                s.SettingsVersion = 2;
            }

            if (s.SettingsVersion < 3)
            {
                // v3：背景模糊强度默认值从 0 改成 24（玻璃面板要靠它才有磨砂底）。
                // 这个字段一直没有 UI，老配置里的 0 只是字段默认值的残留，
                // 没人是「故意调成 0」的 —— 所以 <=0 一律升到新默认；
                // 之后用户若在设置里把它调回 0，以他的值为准，不会再被改。
                if (s.BackgroundBlur <= 0) s.BackgroundBlur = 24;
                s.SettingsVersion = 3;
            }
        }

        public void Save()
        {
            try
            {
                Directory.CreateDirectory(SettingsDir);
                var options = new JsonSerializerOptions { WriteIndented = true };
                File.WriteAllText(SettingsPath, JsonSerializer.Serialize(this, options));
            }
            catch { }
        }

        /// <summary>
        /// 返回背景文件的真实路径，并在路径失效时尝试自愈。
        /// <para>
        /// 旧实现只做 <c>File.Exists</c> 判断，一旦用户把背景文件改名/移动
        /// （例如 <c>精灵公主-星尘.mp4</c> → <c>精灵公主-星雨.mp4</c>），
        /// 就会判定"没有自定义背景"并静默隐藏，用户看到的现象正是
        /// "设置好背景，关闭重开后就消失了"。这里在同目录下找同扩展名、
        /// 文件名高度近似的**唯一**候选并回写配置，避免背景无端丢失。
        /// </para>
        /// <para>
        /// 目录同样要自愈：用户手选的可能是一整个 WE 壁纸文件夹，<c>File.Exists</c> 对目录
        /// 恒为 false —— 那种情况下按 WE 工程口径解析出真正能用的素材（优先原始视频）。
        /// </para>
        /// </summary>
        public string ResolveBackgroundPath()
        {
            var saved = BackgroundImagePath;
            if (string.IsNullOrWhiteSpace(saved)) return "";
            if (File.Exists(saved))
            {
                // 已经是一张能直接用的素材就照用。唯一的例外：它是 WE 的**官方预览素材**
                // （preview.gif / preview.jpg，通常只有 256~512px 宽），而同目录的壁纸工程里
                // 有**原始视频** —— 那就换成原始视频。用户当初多半是手选了预览图，
                // 看到的正是「背景糊得发虚、比例也对不上」（2026-09-23 反馈）。
                var direct = WallpaperEngineService.ResolveOne(saved);
                if (direct?.FromPreview == true)
                {
                    var dir = Path.GetDirectoryName(saved);
                    var better = WallpaperEngineService.ResolveOne(dir);
                    if (better?.Usable == true && !better.FromPreview
                        && !string.IsNullOrEmpty(better.SourcePath))
                    {
                        BackgroundImagePath = better.SourcePath;
                        Save();
                        return better.SourcePath;
                    }
                }
                return saved;
            }

            // 存的是**目录**（用户手选了整张 WE 壁纸文件夹，或选的是 myprojects 里的某个工程）：
            // File.Exists 对目录恒为 false，旧实现会直接判定"没有自定义背景"并静默关掉背景。
            // 这里按与「从 Wallpaper Engine 选择」相同的口径解析出真正能播的素材（优先原始视频），
            // 解析失败才继续走下面的"改名自愈"。
            if (Directory.Exists(saved))
            {
                var item = WallpaperEngineService.ResolveOne(saved);
                if (item?.Usable == true && !string.IsNullOrEmpty(item.SourcePath))
                {
                    BackgroundImagePath = item.SourcePath;
                    Save();
                    return item.SourcePath;
                }
            }

            var repaired = TryRepairBackgroundPath(saved);
            if (!string.IsNullOrEmpty(repaired))
            {
                BackgroundImagePath = repaired;
                Save();
            }
            return repaired;
        }

        /// <summary>背景路径是否配置了、但实际文件已找不到（用于在设置界面给出明确提示）。</summary>
        public bool BackgroundMissing =>
            !string.IsNullOrWhiteSpace(BackgroundImagePath) && string.IsNullOrEmpty(ResolveBackgroundPath());

        /// <summary>
        /// 在同目录内寻找被改名的背景文件：同扩展名 + 名称相似度 ≥ 0.6 且候选唯一。
        /// 阈值取 0.6 足以覆盖单字改动（尘→雨：0.92），又不会把无关文件误判为同一资源。
        /// </summary>
        private static string TryRepairBackgroundPath(string savedPath)
        {
            try
            {
                var dir = Path.GetDirectoryName(savedPath);
                var ext = Path.GetExtension(savedPath);
                if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir) || string.IsNullOrEmpty(ext))
                    return "";

                var stem = Path.GetFileNameWithoutExtension(savedPath);
                string? best = null;
                var bestScore = 0.0;
                var ambiguous = false;

                foreach (var file in Directory.EnumerateFiles(dir, "*" + ext))
                {
                    var score = Similarity(stem, Path.GetFileNameWithoutExtension(file));
                    if (score > bestScore + 0.0001)
                    {
                        bestScore = score;
                        best = file;
                        ambiguous = false;
                    }
                    else if (Math.Abs(score - bestScore) <= 0.0001)
                    {
                        ambiguous = true;
                    }
                }

                return best == null || ambiguous || bestScore < 0.6 ? "" : best;
            }
            catch
            {
                return "";
            }
        }

        /// <summary>基于最长公共子序列的名称相似度（0~1）。</summary>
        private static double Similarity(string a, string b)
        {
            if (a.Length == 0 || b.Length == 0) return 0;

            var dp = new int[a.Length + 1, b.Length + 1];
            for (var i = 1; i <= a.Length; i++)
            {
                for (var j = 1; j <= b.Length; j++)
                {
                    dp[i, j] = a[i - 1] == b[j - 1]
                        ? dp[i - 1, j - 1] + 1
                        : Math.Max(dp[i - 1, j], dp[i, j - 1]);
                }
            }

            return 2.0 * dp[a.Length, b.Length] / (a.Length + b.Length);
        }
    }
}
