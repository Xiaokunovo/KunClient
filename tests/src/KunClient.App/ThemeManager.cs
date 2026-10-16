using System;
using System.Windows;
using System.Windows.Media;

namespace KunFlashTool.App;

/// <summary>
/// 运行时主题引擎（Windows 11 Fluent 设计语言）。
/// 把当前主题色板作为冻结画笔注入应用资源作用域；
/// 所有页面/对话框/共享样式都通过 DynamicResource 跟随主题切换。
/// </summary>
public static class ThemeManager
{
    public enum Theme { Light, Dark }

    private static Theme _current = Theme.Light;
    public static Theme CurrentTheme => _current;

    public static event Action<Theme>? ThemeChanged;

    // ═════════ 深色主题（「石墨台面」—— 同一套工具箱版式的暗色适配） ═════════
    // 版式与浅色完全一致，只换色：窗底/面板/描边整体压深，
    // 强调色仍是同一支天空蓝 #7AB8F3 —— 它在深底上足够亮，不需要换色号，
    // 只是「压在强调色上的文字」要从深海军蓝翻成近黑（AccentText）。
    private static readonly (string Key, Color Value)[] DarkTokens =
    [
        ("Theme.Bg",              Color.FromRgb(0x17, 0x19, 0x1C)),
        ("Theme.BgTint",          Color.FromArgb(0xB0, 0x17, 0x19, 0x1C)),
        // 玻璃底色：alpha < 255 是**故意的** —— 壁纸先被 BackgroundBlur 模糊，
        // 再透过这些底色露出来才是「磨砂」。ApplyCardOpacity 在这层 alpha 上再乘用户滑条。
        ("Theme.Surface",         Color.FromArgb(0x9E, 0x1E, 0x21, 0x26)),
        // 🔴 2026-10-08 液态玻璃（iOS 观感）：面板不透明度大幅下调 —— 原来 0xCC/0xD1 是「近实心」，
        //    磨砂底根本透不出来，那才是用户说的「液态玻璃没实现」。现在深色 62%/65%。
        ("Theme.SurfaceAlt",      Color.FromArgb(0xA6, 0x23, 0x26, 0x2C)),
        ("Theme.SurfaceElevated", Color.FromArgb(0xA6, 0x26, 0x2A, 0x30)),
        ("Theme.TitleBarBg",      Color.FromArgb(0xB8, 0x20, 0x23, 0x29)),
        ("Theme.Border",          Color.FromRgb(0x3A, 0x3F, 0x47)),
        ("Theme.BorderSubtle",    Color.FromRgb(0x2C, 0x30, 0x37)),
        ("Theme.CardBorder",      Color.FromRgb(0x3A, 0x3F, 0x47)),
        ("Theme.CardBg",          Color.FromArgb(0x9E, 0x1E, 0x21, 0x26)),
        ("Theme.TextPrimary",     Color.FromRgb(0xE6, 0xE8, 0xEB)),
        ("Theme.TextSecondary",   Color.FromRgb(0xA8, 0xAE, 0xB8)),
        ("Theme.TextTertiary",    Color.FromRgb(0x7A, 0x81, 0x8C)),
        ("Theme.Accent", Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.Accent2", Color.FromRgb(0x9C, 0xCB, 0xFA)),
        ("Theme.AccentText", Color.FromRgb(0x0B, 0x1B, 0x2A)),
        ("Theme.AccentText2", Color.FromRgb(0x9C, 0xCB, 0xFA)),
        ("Theme.AccentSubtle", Color.FromRgb(0x1B, 0x2A, 0x3C)),
        ("Theme.AccentSoft", Color.FromArgb(0x33, 0x7A, 0xB8, 0xF3)),
        ("Theme.AccentHover", Color.FromRgb(0x93, 0xC4, 0xF8)),
        ("Theme.AccentPressed", Color.FromRgb(0x5F, 0xA6, 0xEE)),
        ("Theme.Hover",           Color.FromRgb(0x26, 0x2A, 0x31)),
        ("Theme.Press",           Color.FromRgb(0x30, 0x36, 0x40)),
        ("Theme.InputBg",         Color.FromArgb(0xD1, 0x1A, 0x1D, 0x22)),
        ("Theme.InputBorder",     Color.FromRgb(0x3A, 0x3F, 0x47)),
        ("Theme.InputHover",      Color.FromRgb(0x55, 0x60, 0x6E)),
        ("Theme.InputFocus", Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.Success",         Color.FromRgb(0x46, 0xB8, 0x7A)),
        ("Theme.SuccessBg",       Color.FromRgb(0x16, 0x28, 0x1E)),
        ("Theme.Warning",         Color.FromRgb(0xD8, 0xA2, 0x3F)),
        ("Theme.WarningBg",       Color.FromRgb(0x2B, 0x25, 0x17)),
        ("Theme.Danger",          Color.FromRgb(0xE8, 0x6A, 0x5C)),
        ("Theme.DangerBg",        Color.FromRgb(0x2E, 0x1D, 0x1B)),
        ("Theme.OkGreen",         Color.FromRgb(0x46, 0xB8, 0x7A)),
        ("Theme.OkGreenBg",       Color.FromRgb(0x16, 0x28, 0x1E)),
        ("Theme.Info",            Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.InfoBg",          Color.FromRgb(0x1B, 0x25, 0x36)),
        ("Theme.LogInfo",         Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.LogSuccess",      Color.FromRgb(0x46, 0xB8, 0x7A)),
        ("Theme.LogWarning",      Color.FromRgb(0xD8, 0xA2, 0x3F)),
        ("Theme.LogError",        Color.FromRgb(0xE8, 0x6A, 0x5C)),
        ("Theme.ProgTrack",       Color.FromRgb(0x26, 0x2B, 0x33)),
        ("Theme.AccentBlue",      Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.WarningText",     Color.FromRgb(0xD8, 0xA2, 0x3F)),
        ("Theme.ShadowColor",     Color.FromArgb(0x40, 0x00, 0x00, 0x00)),
        ("Theme.RowHover",        Color.FromRgb(0x24, 0x2A, 0x33)),
        ("Theme.RowSelected",     Color.FromRgb(0x1E, 0x33, 0x50)),
        // 工具箱版式专用（必须与 DesignTokens.xaml 同步，否则深色下会留浅色补丁）
        ("Theme.PanelHeadBg",     Color.FromArgb(0xBF, 0x20, 0x24, 0x2B)),
        ("Theme.GridLine",        Color.FromRgb(0x2C, 0x32, 0x39)),
        ("Theme.TabStripBg",      Color.FromArgb(0xBF, 0x1B, 0x20, 0x26)),
        ("Theme.StatusBarBg",     Color.FromArgb(0xBF, 0x1C, 0x21, 0x28)),
        ("Theme.ProgressText",    Color.FromRgb(0xFF, 0xFF, 0xFF)),
        ("Theme.BtnBorder",       Color.FromRgb(0x3E, 0x5A, 0x78)),
        ("Theme.BtnText",         Color.FromRgb(0xA8, 0xC8, 0xE8)),
        ("Theme.BtnHoverBg",      Color.FromRgb(0x22, 0x30, 0x3F)),
        ("Theme.GroupHeader",     Color.FromRgb(0x9C, 0xCB, 0xFA)),
        // 液态玻璃（与 DesignTokens.xaml 同步；描边渐变由 BuildGlassEdge 用这些取值重建）
        ("Theme.GlassEdgeTop",    Color.FromArgb(0x4D, 0xFF, 0xFF, 0xFF)),
        ("Theme.GlassEdgeBottom", Color.FromArgb(0x59, 0x00, 0x00, 0x00)),
        ("Theme.GlassSolid",      Color.FromArgb(0xF2, 0x1E, 0x21, 0x26)),
    ];

    // ═════════ 浅色主题（「经典蓝白」色板，默认） ═════════
    // 血统：经典桌面刷机工具 —— 冷灰窗底 #F0F1F3 + 纯白面板 + 1px 灰描边 + 饱和蓝 #2F6FD0。
    private static readonly (string Key, Color Value)[] LightTokens =
    [
        ("Theme.Bg",              Color.FromRgb(0xFA, 0xFB, 0xFF)),
        ("Theme.BgTint",          Color.FromArgb(0xA0, 0xFA, 0xFB, 0xFF)),
        // 玻璃底色：alpha < 255 是**故意的** —— 壁纸先被 BackgroundBlur 模糊，
        // 再透过这些底色露出来才是「磨砂」。ApplyCardOpacity 在这层 alpha 上再乘用户滑条。
        ("Theme.Surface",         Color.FromArgb(0x94, 0xFF, 0xFF, 0xFF)),
        ("Theme.SurfaceAlt",      Color.FromArgb(0x7A, 0xF4, 0xF7, 0xFE)),
        ("Theme.SurfaceElevated", Color.FromArgb(0x9E, 0xFF, 0xFF, 0xFF)),
        ("Theme.TitleBarBg",      Color.FromArgb(0xC7, 0xF4, 0xF7, 0xFE)),
        ("Theme.Border",          Color.FromRgb(0xD5, 0xDE, 0xEB)),
        ("Theme.BorderSubtle",    Color.FromRgb(0xE4, 0xEA, 0xF4)),
        ("Theme.CardBorder",      Color.FromRgb(0xD5, 0xDE, 0xEB)),
        ("Theme.CardBg",          Color.FromArgb(0x94, 0xFF, 0xFF, 0xFF)),
        ("Theme.TextPrimary",     Color.FromRgb(0x1F, 0x23, 0x29)),
        ("Theme.TextSecondary",   Color.FromRgb(0x55, 0x60, 0x6C)),
        ("Theme.TextTertiary",    Color.FromRgb(0x93, 0x9E, 0xAB)),
        ("Theme.Accent", Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.Accent2", Color.FromRgb(0x5F, 0xA6, 0xEE)),
        ("Theme.AccentText", Color.FromRgb(0x10, 0x30, 0x4F)),
        ("Theme.AccentText2", Color.FromRgb(0x3E, 0x8F, 0xE0)),
        ("Theme.AccentSubtle", Color.FromRgb(0xEA, 0xF3, 0xFE)),
        ("Theme.AccentSoft", Color.FromArgb(0x33, 0x7A, 0xB8, 0xF3)),
        ("Theme.AccentHover", Color.FromRgb(0x5F, 0xA6, 0xEE)),
        ("Theme.AccentPressed", Color.FromRgb(0x4E, 0x93, 0xDC)),
        ("Theme.Hover",           Color.FromRgb(0xEE, 0xF4, 0xFE)),
        ("Theme.Press",           Color.FromRgb(0xE2, 0xED, 0xFC)),
        ("Theme.InputBg",         Color.FromArgb(0xE6, 0xFF, 0xFF, 0xFF)),
        ("Theme.InputBorder",     Color.FromRgb(0xCF, 0xDD, 0xEE)),
        ("Theme.InputHover",      Color.FromRgb(0xA9, 0xC7, 0xE6)),
        ("Theme.InputFocus", Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.Success",         Color.FromRgb(0x34, 0xA0, 0x6A)),
        ("Theme.SuccessBg",       Color.FromRgb(0xEB, 0xF7, 0xF0)),
        ("Theme.Warning",         Color.FromRgb(0xC0, 0x7A, 0x16)),
        ("Theme.WarningBg",       Color.FromRgb(0xFD, 0xF5, 0xE8)),
        ("Theme.Danger",          Color.FromRgb(0xD0, 0x50, 0x3F)),
        ("Theme.DangerBg",        Color.FromRgb(0xFC, 0xEF, 0xED)),
        ("Theme.OkGreen",         Color.FromRgb(0x34, 0xA0, 0x6A)),
        ("Theme.OkGreenBg",       Color.FromRgb(0xEB, 0xF7, 0xF0)),
        ("Theme.Info",            Color.FromRgb(0x3E, 0x8F, 0xE0)),
        ("Theme.InfoBg",          Color.FromRgb(0xEA, 0xF3, 0xFE)),
        ("Theme.LogInfo",         Color.FromRgb(0x3E, 0x8F, 0xE0)),
        ("Theme.LogSuccess",      Color.FromRgb(0x34, 0xA0, 0x6A)),
        ("Theme.LogWarning",      Color.FromRgb(0xC0, 0x7A, 0x16)),
        ("Theme.LogError",        Color.FromRgb(0xD0, 0x50, 0x3F)),
        ("Theme.ProgTrack",       Color.FromRgb(0xED, 0xF3, 0xFA)),
        ("Theme.AccentBlue",      Color.FromRgb(0x7A, 0xB8, 0xF3)),
        ("Theme.WarningText",     Color.FromRgb(0xC0, 0x7A, 0x16)),
        ("Theme.ShadowColor",     Color.FromArgb(0x14, 0x00, 0x00, 0x00)),
        ("Theme.RowHover",        Color.FromRgb(0xF2, 0xF7, 0xFE)),
        ("Theme.RowSelected",     Color.FromRgb(0xDF, 0xEB, 0xFB)),
        // 工具箱版式专用（必须与 DesignTokens.xaml 同步）
        ("Theme.PanelHeadBg",     Color.FromArgb(0xCC, 0xF4, 0xF7, 0xFE)),
        ("Theme.GridLine",        Color.FromRgb(0xE9, 0xEF, 0xF7)),
        ("Theme.TabStripBg",      Color.FromArgb(0xCC, 0xF0, 0xF4, 0xFF)),
        ("Theme.StatusBarBg",     Color.FromArgb(0xCC, 0xF0, 0xF4, 0xFF)),
        ("Theme.ProgressText",    Color.FromRgb(0xFF, 0xFF, 0xFF)),
        ("Theme.BtnBorder",       Color.FromRgb(0xB7, 0xCB, 0xDE)),
        ("Theme.BtnText",         Color.FromRgb(0x5B, 0x7F, 0xA0)),
        ("Theme.BtnHoverBg",      Color.FromRgb(0xF3, 0xF8, 0xFF)),
        ("Theme.GroupHeader",     Color.FromRgb(0x3F, 0x55, 0x70)),
        // 液态玻璃（与 DesignTokens.xaml 同步；描边渐变由 BuildGlassEdge 用这些取值重建）
        ("Theme.GlassEdgeTop",    Color.FromArgb(0xF2, 0xFF, 0xFF, 0xFF)),
        ("Theme.GlassEdgeBottom", Color.FromArgb(0x40, 0x00, 0x00, 0x00)),
        ("Theme.GlassSolid",      Color.FromArgb(0xF2, 0xFF, 0xFF, 0xFF)),
    ];

    public static void ApplyTheme(Theme theme)
    {
        _current = theme;
        var dict = Application.Current.Resources;
        var tokens = theme == Theme.Dark ? DarkTokens : LightTokens;

        Color accent = default;
        foreach (var (key, color) in tokens)
        {
            var brush = new SolidColorBrush(color);
            brush.Freeze();
            if (dict.Contains(key)) dict.Remove(key);
            dict.Add(key, brush);

            if (key == "Theme.Accent") accent = color;
        }

        // Fluent 无渐变：用纯色强调色重建（保留键以兼容既有引用）
        var flat = new LinearGradientBrush(accent, accent, new Point(0, 0), new Point(1, 1));
        flat.Freeze();
        if (dict.Contains("Theme.AccentGradient")) dict.Remove("Theme.AccentGradient");
        dict.Add("Theme.AccentGradient", flat);

        // 液态玻璃三支描边：配方完全一样（顶高光 → 中段主题描边色 → 底暗影），
        // 只是中段取哪一支不同。DesignTokens.xaml 里存的是浅色初值，
        // 每次换主题都用当前主题的取值整体重建 —— 否则深色下会留一圈浅色描边。
        //
        // 中段在「没有壁纸」时要加深（见 SetGlassContrastBoost）：玻璃的边是压在壁纸上的，
        // 没壁纸时面板与窗底都是近白，不加深就整圈都读不出来。
        var mid = Pick(tokens, "Theme.Border");
        if (_contrastBoost) mid = Darken(mid, 0.22);

        SetGradient(dict, "Glass.Edge",
            BuildGlassEdge(Pick(tokens, "Theme.GlassEdgeTop"), mid,
                           Pick(tokens, "Theme.GlassEdgeBottom")));
        SetGradient(dict, "Glass.BtnEdge",
            BuildGlassEdge(Pick(tokens, "Theme.GlassEdgeTop"), Pick(tokens, "Theme.BtnBorder"),
                           Pick(tokens, "Theme.GlassEdgeBottom")));
        SetGradient(dict, "Glass.InputEdge",
            BuildGlassEdge(Pick(tokens, "Theme.GlassEdgeTop"), Pick(tokens, "Theme.InputBorder"),
                           Pick(tokens, "Theme.GlassEdgeBottom")));

        if (_contrastBoost)
        {
            BoostSurfaces(dict, tokens);
            ApplyAmbientBase(dict);
        }

        ThemeChanged?.Invoke(theme);
    }

    /// <summary>
    /// 无壁纸时的「环境底色」（2026-10-08，iOS 液态玻璃）。
    /// <para>iOS 的玻璃之所以成立，是因为它背后**永远有内容**（浅灰底 + 柔和色晕）。
    /// 我们没设壁纸时窗底是纯白 <c>#FAFBFF</c>，半透明白面板压上去就是「白上叠白」——
    /// 磨砂、镜面高光、外阴影**三样全都读不出来**。这就是「液态玻璃没实现」的最后一块病根：
    /// 不是玻璃没做，是**背后没东西**。</para>
    /// <para>把基色压到浅灰蓝（深色主题压到近黑），玻璃才有东西可以透、可以衬。
    /// ⚠️ 只在「没有壁纸」时生效 —— 有壁纸时壁纸自己就是那层内容，再压色会把壁纸弄脏。</para>
    /// </summary>
    private static void ApplyAmbientBase(ResourceDictionary dict)
    {
        var dark = _current == Theme.Dark;
        // 窗底只压一点点（色晕那层在 DynamicBackground 里，见 BuildAmbientBloom）
        SetBrush(dict, "Theme.Bg",
            dark ? Color.FromRgb(0x12, 0x14, 0x1A) : Color.FromRgb(0xF7, 0xF9, 0xFD));
        // BgTint 是**页面根**的底色，压在色晕之上 —— 它自己的 alpha 直接决定色晕还剩多少能透出来。
        // 原来 0xA0(63%) 会把色晕稀释掉 63%，玻璃看着还是白的；这里降到 0x59(35%)，
        // 色晕才能真的当「背后那张图」用。
        SetBrush(dict, "Theme.BgTint",
            dark ? Color.FromArgb(0x59, 0x12, 0x14, 0x1A) : Color.FromArgb(0x59, 0xF7, 0xF9, 0xFD));
    }

    private static void SetBrush(ResourceDictionary dict, string key, Color c)
    {
        var brush = new SolidColorBrush(c);
        brush.Freeze();
        if (dict.Contains(key)) dict.Remove(key);
        dict.Add(key, brush);
    }

    /// <summary>
    /// 「没有壁纸时的玻璃兜底」开关（2026-10-08，用户反馈「液态玻璃效果没有实现」）。
    /// <para>玻璃的观感靠三支描边压在**壁纸**上：设了壁纸时上沿高光、下沿暗影都读得出来；
    /// 一旦没设壁纸，面板（Surface 0xD9FFFFFF）和窗底（Bg #FAFBFF）都是近白 ——
    /// 顶边白对白、底边暗影太淡、中段那圈描边又只有 1px，于是整块面板看起来就是一片死白。
    /// 用户看到的现象正是「玻璃没实现」。</para>
    /// <para>打开后做两件事（都只在**当前主题**的取值上做，alpha 一律不动 ——
    /// 卡片透明度滑条是在 alpha 上乘的，动 alpha 会跟它打架）：</para>
    /// <list type="bullet">
    ///   <item><description><c>Glass.Edge</c> 的中段描边加深 → 面板边界读得出来；</description></item>
    ///   <item><description>面板底色 RGB 下压一档 → 卡片与窗底不再是同一块白，有「浮起来」的层次。</description></item>
    /// </list>
    /// <para>由 <c>MainWindow.ApplyDynamicBackground</c> 按「有没有壁纸」调用；值没变时直接返回，
    /// 所以不会和 ThemeChanged → 重新应用主题形成回环。</para>
    /// </summary>
    public static void SetGlassContrastBoost(bool on)
    {
        if (_contrastBoost == on) return;
        _contrastBoost = on;
        ApplyTheme(_current);
    }

    private static bool _contrastBoost;

    /// <summary>把面板类底色的 RGB 各下压一档（alpha 保持原样）。</summary>
    private static void BoostSurfaces(ResourceDictionary dict, (string Key, Color Value)[] tokens)
    {
        foreach (var key in new[] { "Theme.Surface", "Theme.CardBg", "Theme.SurfaceAlt", "Theme.SurfaceElevated" })
        {
            var c = Pick(tokens, key);
            if (c.A == 0) continue;                 // token 表里没有这个键 → 别造一个透明刷子
            var brush = new SolidColorBrush(Darken(c, 0.05));
            brush.Freeze();
            if (dict.Contains(key)) dict.Remove(key);
            dict.Add(key, brush);
        }
    }

    /// <summary>按比例把颜色调暗，alpha 不动。</summary>
    private static Color Darken(Color c, double amount)
    {
        var k = 1 - Math.Clamp(amount, 0, 1);
        return Color.FromArgb(c.A, (byte)(c.R * k), (byte)(c.G * k), (byte)(c.B * k));
    }

    /// <summary>从当前主题的 token 表里按键取色（取不到给透明，绝不抛）。</summary>
    private static Color Pick((string Key, Color Value)[] tokens, string key)
    {
        foreach (var (k, c) in tokens)
        {
            if (string.Equals(k, key, StringComparison.Ordinal)) return c;
        }
        return Colors.Transparent;
    }

    /// <summary>
    /// 玻璃描边的统一配方：上沿高光 → 0.07 起过渡到中段描边色 → 0.88 起压向底沿暗影。
    /// 1px 边框上，上/下边取到的是渐变两端（高光/暗影），
    /// 左右边取到的是一整条「上亮下暗」的过渡 —— 正是玻璃侧壁受光的样子。
    /// </summary>
    private static LinearGradientBrush BuildGlassEdge(Color top, Color mid, Color bottom)
    {
        var brush = new LinearGradientBrush { StartPoint = new Point(0, 0), EndPoint = new Point(0, 1) };
        brush.GradientStops.Add(new GradientStop(top, 0));
        brush.GradientStops.Add(new GradientStop(mid, 0.07));
        brush.GradientStops.Add(new GradientStop(mid, 0.88));
        brush.GradientStops.Add(new GradientStop(bottom, 1));
        brush.Freeze();
        return brush;
    }

    private static void SetGradient(ResourceDictionary dict, string key, Brush brush)
    {
        if (dict.Contains(key)) dict.Remove(key);
        dict.Add(key, brush);
    }

    public static void Toggle() => ApplyTheme(_current == Theme.Light ? Theme.Dark : Theme.Light);

    /// <summary>切一次会切到哪个主题（供过渡层提前拿目标底色）。</summary>
    public static Theme NextTheme => _current == Theme.Light ? Theme.Dark : Theme.Light;

    /// <summary>
    /// 只读地取某个主题下某个 token 的颜色，不改动应用资源。
    /// 供主题切换的"柔洗"过渡层提前取目标底色用 —— 必须先知道要去哪，才能往那个颜色过渡。
    /// </summary>
    public static Color PeekColor(Theme theme, string key)
    {
        var tokens = theme == Theme.Dark ? DarkTokens : LightTokens;
        foreach (var (k, c) in tokens)
        {
            if (string.Equals(k, key, StringComparison.Ordinal)) return c;
        }
        return Colors.Transparent;
    }

    /// <summary>默认浅色主题；后续可扩展为读取系统深浅色偏好。</summary>
    public static Theme DetectSystemTheme() => Theme.Light;
}
