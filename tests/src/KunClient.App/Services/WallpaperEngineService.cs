using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace KunFlashTool.App.Services;

/// <summary>一张壁纸最终能拿来当背景的素材类型。</summary>
public enum WallpaperSourceKind
{
    /// <summary>没有可用素材。</summary>
    None,
    /// <summary>视频（mp4/webm…），MediaElement 直接播。</summary>
    Video,
    /// <summary>动图（GIF），由 DynamicBackground 逐帧播放。</summary>
    AnimatedGif,
    /// <summary>静态图片。</summary>
    StaticImage,
}

/// <summary>
/// Wallpaper Engine 动态壁纸发现服务。
/// <para>
/// Wallpaper Engine 的壁纸按 <c>project.json</c> 里的 <c>type</c> 分三类：
/// </para>
/// <list type="bullet">
/// <item><c>video</c> —— 视频壁纸（*.mp4 / *.webm）。本工具的动态背景本身就是 MediaElement，
/// 这一类可以**直接**拿来当背景，画质最好。</item>
/// <item><c>scene</c> —— 使用私有打包格式 <c>scene.pkg</c>（体积可达数百 MB），只有
/// Wallpaper Engine 自己解得开，第三方无法还原渲染（涉及其私有场景渲染器与着色器）。</item>
/// <item><c>web</c> —— HTML 壁纸，依赖 Wallpaper Engine 内置浏览器内核。</item>
/// </list>
/// <para>
/// <b>scene / web 的可行兜底：官方预览素材。</b>这两类壁纸的目录里 Wallpaper Engine 自己会写一份
/// <c>preview.gif</c>（动图，优先）或 <c>preview.jpg</c>（静态图），字段名见 <c>project.json</c> 的
/// <c>preview</c>。它虽然不是原始素材（清晰度有限、不能像 WE 那样带交互），但**画面就是这张壁纸**，
/// 动图还能动 —— 比"直接判定为不可用"有用得多，所以本服务把它作为 scene / web 的降级来源。
/// </para>
/// <para>
/// 这里负责：定位 Steam 库 → 找到创意工坊 AppID <c>431960</c>（Wallpaper Engine）目录
/// → 解析每张壁纸的 project.json → 抽出真正可用的素材路径。
/// </para>
/// </summary>
public static class WallpaperEngineService
{
    /// <summary>Wallpaper Engine 在 Steam 上的 AppID，创意工坊子目录名就是它。</summary>
    private const string WeAppId = "431960";

    /// <summary>MediaElement（Media Foundation）能吃下的视频容器。</summary>
    private static readonly string[] VideoExtensions =
        { ".mp4", ".webm", ".avi", ".mkv", ".mov", ".wmv" };

    /// <summary>WPF 自带解码器能读的图片格式（**不含 .webp**：WPF 默认解不了，硬塞会退回渐变底）。</summary>
    private static readonly string[] ImageExtensions =
        { ".gif", ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff" };

    /// <summary>预览素材的候选文件名（project.json 未给出 preview 字段时的兜底顺序）。</summary>
    private static readonly string[] PreviewCandidates =
        { "preview.gif", "preview.jpg", "preview.jpeg", "preview.png" };

    /// <summary>一张壁纸的扫描结果。</summary>
    public sealed class WallpaperItem
    {
        public string Title { get; init; } = "";
        /// <summary>project.json 里的 type 原值：video / scene / web / 空。</summary>
        public string Type { get; init; } = "";
        /// <summary>可直接当背景的素材绝对路径（视频 / 动图 / 静态图）；不可用时为 null。</summary>
        public string? SourcePath { get; init; }
        public WallpaperSourceKind SourceKind { get; init; }
        /// <summary>true = 用的是官方预览素材，而非壁纸的原始素材（scene / web 的降级路径）。</summary>
        public bool FromPreview { get; init; }
        public string Folder { get; init; } = "";
        /// <summary>从哪个扫描根目录发现的（用于提示文件位置）。</summary>
        public string Source { get; init; } = "";

        public bool Usable => SourceKind != WallpaperSourceKind.None
                              && !string.IsNullOrEmpty(SourcePath)
                              && File.Exists(SourcePath);

        public string TypeText => SourceKind switch
        {
            WallpaperSourceKind.Video => "视频 · 可直接使用",
            WallpaperSourceKind.AnimatedGif => FromPreview ? "场景/网页 · 官方预览动图" : "动图",
            WallpaperSourceKind.StaticImage => FromPreview ? "场景/网页 · 官方预览图（静态）" : "图片 · 静态",
            _ => Type switch
            {
                "scene" => "场景 · 未找到预览素材",
                "web" => "网页 · 未找到预览素材",
                "video" => "视频 · 文件缺失",
                _ => string.IsNullOrEmpty(Type) ? "未知类型" : Type,
            },
        };
    }

    /// <summary>扫描本机所有已知位置；可用的排在前面，其余按名称排序。</summary>
    public static List<WallpaperItem> Discover()
    {
        var items = new List<WallpaperItem>();
        foreach (var root in CandidateProjectRoots())
            ScanRoot(root, items);
        return Dedup(items);
    }

    /// <summary>只扫描用户手动指定的目录（应对非 Steam 默认安装位置）。</summary>
    public static List<WallpaperItem> DiscoverIn(string folder)
    {
        var items = new List<WallpaperItem>();
        if (Directory.Exists(folder)) ScanRoot(folder, items);
        return Dedup(items);
    }

    /// <summary>本机是否装了 Wallpaper Engine（用于界面给出更贴切的提示）。</summary>
    public static bool IsInstalled()
    {
        foreach (var root in CandidateProjectRoots())
            if (Directory.Exists(root)) return true;
        return false;
    }

    /// <summary>
    /// 把「用户手选的一个路径」解析成真正能用的背景素材。
    /// <para>
    /// 【为什么必须有这个统一入口】「选择文件」那条私有背景路径过去是**先存路径、后解码**，
    /// 而且只按扩展名判断能不能用。于是用户一旦选到 WE 壁纸目录里的
    /// <c>preview.gif / preview.jpg</c>（官方预览素材，通常只有 256~512px 宽），
    /// 或者干脆选了**整个壁纸文件夹**（File.Exists 为 false），就会得到一个
    /// 「糊得发虚、比例还对不上」（用户 2026-09-23 原话）甚至根本加载不出来的背景。
    /// 这里改成与「从 Wallpaper Engine 选择」完全同一条读取口径：
    /// </para>
    /// <list type="bullet">
    /// <item>传**目录**，或传 WE 壁纸目录里的任意文件 → 走 <see cref="ParseFolder"/>：
    /// 优先原始视频（<c>type=video</c>），没有视频才退到官方预览素材，并在
    /// <see cref="WallpaperItem.FromPreview"/> 上标记"这是低清降级素材"。</item>
    /// <item>传普通图片 / 视频文件 → 原样返回，<c>FromPreview=false</c>。</item>
    /// <item>传不认识的扩展名（如 WPF 解不了的 <c>.webp</c>）→ 返回 null，由调用方给出提示。</item>
    /// </list>
    /// </summary>
    public static WallpaperItem? ResolveOne(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return null;
        try
        {
            if (Directory.Exists(path))
                return ParseFolder(path, Path.GetFileName(path.TrimEnd('\\', '/')), path);

            if (!File.Exists(path)) return null;

            var full = Normalize(path) ?? path;
            var dir = Path.GetDirectoryName(full);

            // 文件在某个含 project.json 的目录里 → 这是 WE 壁纸的一部分（常见于用户手选 preview.gif）。
            // 先按整张壁纸解析：能给出**原始视频**就用原始视频，比用户点中的那张预览图清晰得多。
            if (!string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "project.json")))
            {
                var byProject = ParseFolder(dir!, Path.GetFileName(dir!), dir!);
                if (byProject?.Usable == true) return byProject;
            }

            var kind = IsVideo(full) ? WallpaperSourceKind.Video
                     : Path.GetExtension(full).ToLowerInvariant() == ".gif" ? WallpaperSourceKind.AnimatedGif
                     : IsImage(full) ? WallpaperSourceKind.StaticImage
                     : WallpaperSourceKind.None;
            if (kind == WallpaperSourceKind.None) return null;

            return new WallpaperItem
            {
                Title = Path.GetFileNameWithoutExtension(full),
                Type = "file",
                SourcePath = full,
                SourceKind = kind,
                FromPreview = false,
                Folder = dir ?? "",
                Source = dir ?? "",
            };
        }
        catch { return null; }
    }

    // 注意：这里曾有一个 TryGetWebEntry（判断 type=web 并返回 index.html）。
    // 2026-09-23 起「网页类实时渲染」已被整体移除（它依赖 WebView2 → 单项 23.6MB 的
    // Microsoft.Windows.SDK.NET.dll，用户要求体积尽可能小），web 类壁纸一律按官方预览素材处理，
    // 所以那个入口没有任何调用方，已删除。不要再把它加回来。
    // （它用到的 ResolveWallpaperFolder 也随之删除 —— 现在「把路径归位成壁纸目录」这件事
    //   由 ResolveOne / ParseFolder 直接负责：判断依据从"父目录"升级成了"父目录里有没有 project.json"。）

    private static List<WallpaperItem> Dedup(List<WallpaperItem> items)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var result = new List<WallpaperItem>();
        foreach (var it in items)
        {
            // 去重键必须规范化：Steam 注册表里的 SteamPath 是正斜杠、HKLM 是反斜杠，
            // 否则 e:/steam\... 与 E:\steam\... 会被当成两张不同的壁纸，整份列表翻倍。
            var key = Normalize(it.SourcePath ?? it.Folder) ?? it.Folder;
            if (!string.IsNullOrEmpty(key) && seen.Add(key)) result.Add(it);
        }
        result.Sort((a, b) =>
        {
            var byUsable = b.Usable.CompareTo(a.Usable);
            if (byUsable != 0) return byUsable;
            // 同为可用时，视频（原始素材）排在降级的预览素材之前
            var byKind = a.SourceKind.CompareTo(b.SourceKind);
            return byKind != 0
                ? byKind
                : string.Compare(a.Title, b.Title, StringComparison.CurrentCulture);
        });
        return result;
    }

    // ====== 目录发现 ======

    private static IEnumerable<string> CandidateProjectRoots()
    {
        foreach (var steam in SteamRoots())
        {
            foreach (var lib in LibraryRoots(steam))
            {
                // 创意工坊订阅的壁纸
                yield return Path.Combine(lib, "steamapps", "workshop", "content", WeAppId);
                // 用户自己在 Wallpaper Engine 里「创建壁纸」生成的工程
                yield return Path.Combine(lib, "steamapps", "common", "wallpaper_engine", "projects", "myprojects");
                yield return Path.Combine(lib, "steamapps", "common", "wallpaper_engine", "projects", "defaultprojects");
            }
        }
    }

    private static List<string> SteamRoots()
    {
        var list = new List<string>();
        void Add(string? p)
        {
            p = Normalize(p);
            if (string.IsNullOrWhiteSpace(p) || !Directory.Exists(p)) return;
            if (!list.Contains(p!, StringComparer.OrdinalIgnoreCase)) list.Add(p!);
        }

        // 注册表是最可靠的来源：Steam 可以装在任意盘/任意目录
        Add(ReadRegistry(Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath"));
        Add(ReadRegistry(Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"));
        Add(ReadRegistry(Registry.LocalMachine, @"SOFTWARE\Valve\Steam", "InstallPath"));
        // 兜底：两个默认安装位置
        Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam"));
        Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Steam"));
        return list;
    }

    private static string? ReadRegistry(RegistryKey hive, string subKey, string valueName)
    {
        try
        {
            using var key = hive.OpenSubKey(subKey);
            return key?.GetValue(valueName) as string;
        }
        catch { return null; }
    }

    /// <summary>
    /// 解析 <c>steamapps\libraryfolders.vdf</c>，取出该 Steam 下挂载的全部库目录。
    /// 兼容新旧两种格式：
    ///   旧：  "1"  "D:\\SteamLibrary"
    ///   新：  "1" { "path" "D:\\SteamLibrary" ... }
    /// 直接用正则抓所有 <c>"path" "..."</c>，并对旧格式兜底抓纯路径值。
    /// </summary>
    private static List<string> LibraryRoots(string steamRoot)
    {
        var libs = new List<string> { steamRoot };
        try
        {
            var vdf = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
            if (!File.Exists(vdf)) return libs;

            var text = File.ReadAllText(vdf);
            var matches = Regex.Matches(text, "\"path\"\\s*\"([^\"]+)\"", RegexOptions.IgnoreCase);
            foreach (Match m in matches)
                AddLib(libs, m.Groups[1].Value);

            // 旧版 VDF 没有 path 键，直接是 "序号" "盘符路径"
            if (matches.Count == 0)
            {
                foreach (Match m in Regex.Matches(text, "\"\\d+\"\\s*\"([A-Za-z]:[^\"]+)\""))
                    AddLib(libs, m.Groups[1].Value);
            }
        }
        catch { }
        return libs;
    }

    private static void AddLib(List<string> libs, string raw)
    {
        var p = Normalize(raw.Replace("\\\\", "\\")) ?? "";
        if (p.Length == 0 || !Directory.Exists(p)) return;
        if (!libs.Contains(p, StringComparer.OrdinalIgnoreCase)) libs.Add(p);
    }

    /// <summary>
    /// 路径规范化。Steam 注册表里的 <c>SteamPath</c> 用正斜杠（<c>e:/steam</c>），
    /// HKLM 的 <c>InstallPath</c> 与 VDF 里的库路径用反斜杠；不统一的话同一份安装会被
    /// 当成两个扫描根，每张壁纸都会被列两遍（视频类因为去重键走了 GetFullPath 才侥幸没重）。
    /// </summary>
    private static string? Normalize(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return path;
        try { return Path.GetFullPath(path); }
        catch { return path; }
    }

    // ====== 壁纸解析 ======

    private static void ScanRoot(string root, List<WallpaperItem> items)
    {
        if (!Directory.Exists(root)) return;

        // ① 用户直接选了「某一张壁纸」的文件夹：根目录自己就带 project.json 或素材
        var self = ParseFolder(root, Path.GetFileName(root), root);
        if (self != null) { items.Add(self); return; }

        // ② 正常情况下根目录下是若干张壁纸的文件夹（创意工坊目录名是数字 ID）
        string[] dirs;
        try { dirs = Directory.GetDirectories(root); }
        catch { return; }

        foreach (var dir in dirs)
        {
            try
            {
                var item = ParseFolder(dir, Path.GetFileName(dir), root);
                if (item != null) items.Add(item);
            }
            catch { }
        }
    }

    private static WallpaperItem? ParseFolder(string folder, string fallbackTitle, string source)
    {
        var title = fallbackTitle;
        var type = "";
        string? rawVideo = null;      // project.json 的 file 字段指向的视频（原始素材）
        string? previewName = null;   // project.json 的 preview 字段

        var projectJson = Path.Combine(folder, "project.json");
        if (File.Exists(projectJson))
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(projectJson));
                var r = doc.RootElement;

                if (r.TryGetProperty("title", out var t) && t.ValueKind == JsonValueKind.String)
                {
                    var v = t.GetString();
                    if (!string.IsNullOrWhiteSpace(v)) title = v!;
                }
                if (r.TryGetProperty("type", out var ty) && ty.ValueKind == JsonValueKind.String)
                    type = (ty.GetString() ?? "").Trim().ToLowerInvariant();
                if (r.TryGetProperty("preview", out var pv) && pv.ValueKind == JsonValueKind.String)
                    previewName = pv.GetString();
                if (r.TryGetProperty("file", out var f) && f.ValueKind == JsonValueKind.String)
                    rawVideo = ResolveRelative(folder, f.GetString());
            }
            catch { /* project.json 损坏就退回下面的「扫文件夹找素材」 */ }
        }

        if (rawVideo == null || !IsVideo(rawVideo))
            rawVideo = FindFirstVideo(folder);

        // ---- 决定最终可用的素材 ----
        string? path = null;
        var kind = WallpaperSourceKind.None;
        var fromPreview = false;

        if (rawVideo != null)
        {
            path = rawVideo;
            kind = WallpaperSourceKind.Video;
        }
        else
        {
            // scene / web（以及任何没有视频的壁纸）：退到 Wallpaper Engine 官方的预览素材
            var previewPath = FindPreview(folder, previewName);
            if (previewPath != null)
            {
                path = previewPath;
                fromPreview = true;
                kind = Path.GetExtension(previewPath).ToLowerInvariant() == ".gif"
                    ? WallpaperSourceKind.AnimatedGif
                    : WallpaperSourceKind.StaticImage;
            }
        }

        // 既没有可识别类型、也找不到任何素材 → 不是一张壁纸目录，跳过
        if (string.IsNullOrEmpty(type) && path == null) return null;

        return new WallpaperItem
        {
            Title = string.IsNullOrWhiteSpace(title) ? "(未命名壁纸)" : title,
            Type = type,
            SourcePath = path,
            SourceKind = kind,
            FromPreview = fromPreview,
            Folder = Normalize(folder) ?? folder,
            Source = source,
        };
    }

    /// <summary>把 project.json 里的相对路径解析成绝对路径；文件不存在时返回 null。</summary>
    private static string? ResolveRelative(string folder, string? relative)
    {
        if (string.IsNullOrWhiteSpace(relative)) return null;
        try
        {
            var abs = Path.GetFullPath(Path.Combine(folder, relative));
            return File.Exists(abs) ? abs : null;
        }
        catch { return null; }
    }

    /// <summary>
    /// 找 Wallpaper Engine 的官方预览素材：优先 project.json 的 preview 字段，
    /// 否则按固定候选名探测（preview.gif → preview.jpg → …）。只认 WPF 能解码的格式。
    /// </summary>
    private static string? FindPreview(string folder, string? previewName)
    {
        var byField = ResolveRelative(folder, previewName);
        if (byField != null && IsImage(byField)) return byField;

        foreach (var candidate in PreviewCandidates)
        {
            try
            {
                var abs = Path.Combine(folder, candidate);
                if (File.Exists(abs)) return abs;
            }
            catch { }
        }
        return null;
    }

    private static string? FindFirstVideo(string folder)
    {
        try
        {
            return Directory.GetFiles(folder, "*.*", SearchOption.TopDirectoryOnly)
                .FirstOrDefault(IsVideo);
        }
        catch { return null; }
    }

    private static bool IsVideo(string path) =>
        Array.IndexOf(VideoExtensions, Path.GetExtension(path).ToLowerInvariant()) >= 0;

    private static bool IsImage(string path) =>
        Array.IndexOf(ImageExtensions, Path.GetExtension(path).ToLowerInvariant()) >= 0;
}
