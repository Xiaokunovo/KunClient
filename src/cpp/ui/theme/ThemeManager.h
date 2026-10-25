#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ThemeManager — 运行时主题引擎
//
//  移植自 tests/src/KunClient.App/ThemeManager.cs + DesignTokens.xaml。
//  把当前主题色板暴露为访问器：QWidget 子类通过 ThemeManager 取色，
//  避免在每个控件里写死色值（写死 = 深色主题下的一块浅色补丁）。
//
//  ⚠️ 项目红线（与 DesignTokens.xaml / ThemeManager.cs 一致）：
//    · 无实心填充按钮 —— 主/危险动作只靠**描边粗细 + 字重**区分；
//    · 圆角只允许 4 和 8 两档（其余取值不得出现在本框架里）。
//  令牌因此只提供这两档 radii。
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>

class QApplication;

namespace kc::ui {

/// 设计令牌：颜色。名称与 C# token 表逐条对应（去掉 Theme. 前缀）。
enum class ColorToken {
    Bg, BgTint,
    Surface, SurfaceAlt, SurfaceElevated,
    CardBg, CardBorder,
    Text, TextMuted, TextFaint,
    Border, BorderSubtle,
    Accent, Accent2, AccentText, AccentText2, AccentSubtle, AccentSoft,
    AccentHover, AccentPressed,
    Hover, Press,
    InputBg, InputBorder, InputHover, InputFocus,
    Success, SuccessBg,
    Warning, WarningBg, WarningText,
    Danger, DangerBg,
    Ok, OkBg,
    Info, InfoBg,
    LogInfo, LogSuccess, LogWarning, LogError,
    ProgTrack, ProgressText,
    PanelHeadBg, GridLine, TabStripBg, StatusBarBg,
    BtnBorder, BtnText, BtnHoverBg, GroupHeader,
    GlassEdgeTop, GlassEdgeBottom, GlassSolid,
    ShadowColor, RowHover, RowSelected,
};

/// 设计令牌：圆角。ColorOS 17 设计语言（2026-10-14 起取代旧 4/8 两档）：
/// 大圆角是「流体云」卡片语言的骨架，按控件体量分五档取用。
enum class RadiusToken {
    /// 8px —— 小控件 / 徽标 / 进度条 / 标签胶囊。
    Small = 8,
    /// 12px —— 按钮 / 输入框 / 导航项 / 列表行。
    Medium = 12,
    /// 16px —— 卡片 / 面板容器。
    Large = 16,
    /// 22px —— 大卡 / 弹窗。
    XLarge = 22,
    /// 28px —— 英雄卡 / 页面根容器。
    XXLarge = 28,
};

/// 设计令牌：间距刻度（8 的倍数，工具版面用）。
enum class SpaceToken {
    Xs = 4,
    Sm = 8,
    Md = 12,
    Lg = 16,
    Xl = 24,
};

/// 设计令牌：字号（pt）。
enum class FontToken {
    Caption = 9,
    Small = 10,
    Body = 11,
    Subtitle = 12,
    Title = 14,
};

/// 设计令牌：字重（对应 C# FontWeights）。
enum class WeightToken {
    Normal = 400,
    Medium = 500,
    SemiBold = 600,
    Bold = 700,
};

/// 运行时主题引擎单例。
/// 所有页面通过它取色、取圆角，并监听 themeChanged 重绘。
class ThemeManager : public QObject {
    Q_OBJECT

public:
    static ThemeManager& instance();

    bool isDark() const { return m_dark; }
    void setDark(bool dark);
    void toggle();

    /// 强调色（2026-10-24）。传无效色（默认构造的 QColor）= 恢复主题内置色。
    /// 设了之后：
    ///   · Accent / Accent2 / Hover / Pressed / Subtle / Soft / InputFocus / Info
    ///     全部由它派生（见 applyAccentOverride）；
    ///   · QSS 里的 %ACCENT% / %ACCENT2% / %ACCENT_DEEP% / %ACCENT_SUBTLE%
    ///     以及 rgba(%ACCENT_R%, …) 三件套都跟着替换。
    /// 🔴 AccentText **不派生**：它是「压在主色上的文字色」，深色主题是深蓝黑、
    ///    浅色主题是纯白，由设计定死。跟着主色算会让默认外观就变样。
    void setAccent(const QColor& accent);
    QColor accent() const { return m_accent; }

    /// 按当前主题加载 :/styles/dark.qss 或 :/styles/light.qss 并 apply 到 app。
    /// 加载失败时静默保留既有样式（不抛）。
    void apply(QApplication& app);

    /// 重新套用当前样式表 —— 「卡片不透明度 / 界面透明」改动后必须调：
    /// 那两个值是以 alpha 形式替换进 QSS 文本的（Qt QSS 没有 CSS 变量），
    /// 不重放样式表就还是旧值 = 滑条看着动了、界面没变。
    void refreshStyleSheet();

    // ── 令牌访问器 ────────────────────────────────────────────────────────
    QColor color(ColorToken token) const;

    /// 按 QSS 里的键名取色（如 "Theme.Accent"）—— 供既有代码沿用字符串键。
    QColor colorByKey(const QString& key) const;

    /// 圆角像素值（Small=4 / Large=8）。
    int radius(RadiusToken token) const { return static_cast<int>(token); }

    /// 间距刻度像素值。
    int space(SpaceToken token) const { return static_cast<int>(token); }

    /// 字号 pt。
    int fontSize(FontToken token) const { return static_cast<int>(token); }

    /// 字重（可直接喂给 QFont::setWeight）。
    int weight(WeightToken token) const { return static_cast<int>(token); }

    /// 强调色/危险色文字压在底色上的对比文字色（AccentText）。
    QColor onAccent() const { return color(ColorToken::AccentText); }

signals:
    /// 主题切换后发出（dark = 新主题是否深色）。
    void themeChanged(bool dark);

private:
    ThemeManager();
    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    void loadTokenTable();

    /// 用 m_accent 覆盖 Accent 系列 token（m_accent 无效时什么都不做）。
    /// loadTokenTable() 末尾会调它 —— 切主题时派生规则不同（浅色主题的
    /// 「深一档」和深色主题的「亮一档」方向相反），必须重算。
    void applyAccentOverride();

    /// 把 QSS 里的强调色占位符换成当前值。
    QString expandAccent(const QString& qss) const;

    /// 把 QSS 里的 %GLASS_PAGE% / %GLASS_CARD% / %GLASS_PANEL% 换成实际 alpha。
    QString expandGlassAlpha(const QString& qss) const;

    bool m_dark = false;
    /// 用户自定义的强调色。无效 = 用主题内置的 #0A84FF。
    QColor m_accent;
    // 逐 token 一张查表：索引 = static_cast<int>(ColorToken)。
    QColor m_colors[static_cast<int>(ColorToken::RowSelected) + 1];
    // 字符串键 → 颜色（供 colorByKey 用）。
    QHash<QString, QColor> m_byKey;
};

} // namespace kc::ui
