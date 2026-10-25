#include "ui/theme/ThemeManager.h"

#include "kc/config/AppSettings.h"   // 玻璃透明度：卡片不透明度 / 界面透明

#include <QApplication>
#include <QFile>

#include <algorithm>

namespace kc::ui {
namespace {

/// QSS 里的玻璃参数占位符（Qt 的 QSS 不支持 CSS 变量，只能在加载时替换成数字）。
/// 用它们的地方见 resources/*.qss 的 glass 段。
constexpr const char* kAlphaPage  = "%GLASS_PAGE%";    // 页面根底色（界面透明）
constexpr const char* kAlphaCard  = "%GLASS_CARD%";    // 卡片 / 顶栏（卡片不透明度 × CardBg 基准）
constexpr const char* kAlphaPanel = "%GLASS_PANEL%";   // 页签条 / 状态栏（× SurfaceAlt 基准）

// 2026-10-17 液态玻璃材质：卡片不再是「一层平涂的半透明白」，而是
// 「上亮下暗的垂直渐变 + 顶边高光」。iOS 的玻璃体积感全靠这两样，
// 平涂只会读成「磨砂塑料」。渐变两端由同一个 cardOpacity 派生，
// 滑条一拉两端同步走，不会出现「越调越不像玻璃」。
constexpr const char* kAlphaCardHi = "%GLASS_CARD_HI%";   // 卡片渐变亮端（× 1.18）
constexpr const char* kAlphaCardLo = "%GLASS_CARD_LO%";   // 卡片渐变暗端（× 0.82）
constexpr const char* kAlphaPageHi = "%GLASS_PAGE_HI%";   // 页面底渐变亮端
constexpr const char* kAlphaPageLo = "%GLASS_PAGE_LO%";   // 页面底渐变暗端

struct TokenEntry {
    ColorToken token;
    const char* key;   // QSS 键名（Theme.xxx），与 DesignTokens.xaml / ThemeManager.cs 同步
    quint32 argb;      // 0xAARRGGBB
};

/// 线性插值混合（t = 0 取 a，1 取 b）。
QColor mixColor(const QColor& a, const QColor& b, qreal t)
{
    const qreal k = std::clamp(t, 0.0, 1.0);
    return QColor(qRound(a.red() + (b.red() - a.red()) * k),
                  qRound(a.green() + (b.green() - a.green()) * k),
                  qRound(a.blue() + (b.blue() - a.blue()) * k));
}

QColor lighten(const QColor& c, qreal amount) { return mixColor(c, Qt::white, amount); }
QColor darken(const QColor& c, qreal amount) { return mixColor(c, Qt::black, amount); }

/// 把颜色往主题底色里「沉」下去，得到一块低饱和的衬底（AccentSubtle）。
/// 直接 darken 会得到同色相的深色，在浅色主题里会很脏；混底才是对的做法。
QColor tintToward(const QColor& c, const QColor& bg, qreal amount)
{
    return mixColor(bg, c, std::clamp(amount, 0.0, 1.0));
}

// ═════════ 深色主题（「石墨台面」）═════════
// 与 ThemeManager.cs 的 DarkTokens 逐条同步。
const TokenEntry kDarkTokens[] = {
    {ColorToken::Bg,              "Theme.Bg",              0xFF17191C},
    {ColorToken::BgTint,          "Theme.BgTint",          0xB017191C},
    {ColorToken::Surface,         "Theme.Surface",         0x9E1E2126},
    {ColorToken::SurfaceAlt,      "Theme.SurfaceAlt",      0xA623262C},
    {ColorToken::SurfaceElevated, "Theme.SurfaceElevated", 0xA6262A30},
    {ColorToken::CardBg,          "Theme.CardBg",          0x9E1E2126},
    {ColorToken::CardBorder,      "Theme.CardBorder",      0xFF3A3F47},
    {ColorToken::Text,            "Theme.TextPrimary",     0xFFE6E8EB},
    {ColorToken::TextMuted,       "Theme.TextSecondary",   0xFFA8AEB8},
    {ColorToken::TextFaint,       "Theme.TextTertiary",    0xFF7A818C},
    {ColorToken::Border,          "Theme.Border",          0xFF3A3F47},
    {ColorToken::BorderSubtle,    "Theme.BorderSubtle",    0xFF2C3037},
    {ColorToken::Accent,          "Theme.Accent",          0xFF0A84FF},
    {ColorToken::Accent2,         "Theme.Accent2",         0xFF5AB0FF},
    {ColorToken::AccentText,      "Theme.AccentText",      0xFF04203D},
    {ColorToken::AccentText2,     "Theme.AccentText2",     0xFF8EC8FF},
    {ColorToken::AccentSubtle,    "Theme.AccentSubtle",    0xFF14283F},
    {ColorToken::AccentSoft,      "Theme.AccentSoft",      0x330A84FF},
    {ColorToken::AccentHover,     "Theme.AccentHover",     0xFF3D9BFF},
    {ColorToken::AccentPressed,   "Theme.AccentPressed",   0xFF0A6FD8},
    {ColorToken::Hover,           "Theme.Hover",           0xFF262A31},
    {ColorToken::Press,           "Theme.Press",           0xFF303640},
    {ColorToken::InputBg,         "Theme.InputBg",         0xD11A1D22},
    {ColorToken::InputBorder,     "Theme.InputBorder",     0xFF3A3F47},
    {ColorToken::InputHover,      "Theme.InputHover",      0xFF55606E},
    {ColorToken::InputFocus,      "Theme.InputFocus",      0xFF0A84FF},
    {ColorToken::Success,         "Theme.Success",         0xFF46B87A},
    {ColorToken::SuccessBg,       "Theme.SuccessBg",       0xFF16281E},
    {ColorToken::Warning,         "Theme.Warning",         0xFFD8A23F},
    {ColorToken::WarningBg,       "Theme.WarningBg",       0xFF2B2517},
    {ColorToken::WarningText,     "Theme.WarningText",     0xFFD8A23F},
    {ColorToken::Danger,          "Theme.Danger",          0xFFE86A5C},
    {ColorToken::DangerBg,        "Theme.DangerBg",        0xFF2E1D1B},
    {ColorToken::Ok,              "Theme.OkGreen",         0xFF46B87A},
    {ColorToken::OkBg,            "Theme.OkGreenBg",       0xFF16281E},
    {ColorToken::Info,            "Theme.Info",            0xFF0A84FF},
    {ColorToken::InfoBg,          "Theme.InfoBg",          0xFF14283F},
    {ColorToken::LogInfo,         "Theme.LogInfo",         0xFF5AB0FF},
    {ColorToken::LogSuccess,      "Theme.LogSuccess",      0xFF46B87A},
    {ColorToken::LogWarning,      "Theme.LogWarning",      0xFFD8A23F},
    {ColorToken::LogError,        "Theme.LogError",        0xFFE86A5C},
    {ColorToken::ProgTrack,       "Theme.ProgTrack",       0xFF262B33},
    {ColorToken::ProgressText,    "Theme.ProgressText",    0xFFFFFFFF},
    {ColorToken::PanelHeadBg,     "Theme.PanelHeadBg",     0xBF20242B},
    {ColorToken::GridLine,        "Theme.GridLine",        0xFF2C3239},
    {ColorToken::TabStripBg,      "Theme.TabStripBg",      0xBF1B2026},
    {ColorToken::StatusBarBg,     "Theme.StatusBarBg",     0xBF1C2128},
    {ColorToken::BtnBorder,       "Theme.BtnBorder",       0xFF2C5F91},
    {ColorToken::BtnText,         "Theme.BtnText",         0xFF8EC8FF},
    {ColorToken::BtnHoverBg,      "Theme.BtnHoverBg",      0xFF22303F},
    {ColorToken::GroupHeader,     "Theme.GroupHeader",     0xFF8EC8FF},
    {ColorToken::GlassEdgeTop,    "Theme.GlassEdgeTop",    0x4DFFFFFF},
    {ColorToken::GlassEdgeBottom, "Theme.GlassEdgeBottom", 0x59000000},
    {ColorToken::GlassSolid,      "Theme.GlassSolid",      0xF21E2126},
    {ColorToken::ShadowColor,     "Theme.ShadowColor",     0x40000000},
    {ColorToken::RowHover,        "Theme.RowHover",        0xFF242A33},
    {ColorToken::RowSelected,     "Theme.RowSelected",     0xFF1E3350},
};

// ═════════ 浅色主题（「经典蓝白」），默认 ═════════
const TokenEntry kLightTokens[] = {
    {ColorToken::Bg,              "Theme.Bg",              0xFFFAFBFF},
    {ColorToken::BgTint,          "Theme.BgTint",          0xA0FAFBFF},
    {ColorToken::Surface,         "Theme.Surface",         0x94FFFFFF},
    {ColorToken::SurfaceAlt,      "Theme.SurfaceAlt",      0x7AF4F7FE},
    {ColorToken::SurfaceElevated, "Theme.SurfaceElevated", 0x9EFFFFFF},
    {ColorToken::CardBg,          "Theme.CardBg",          0x94FFFFFF},
    {ColorToken::CardBorder,      "Theme.CardBorder",      0xFFD5DEEB},
    {ColorToken::Text,            "Theme.TextPrimary",     0xFF1F2329},
    {ColorToken::TextMuted,       "Theme.TextSecondary",   0xFF55606C},
    {ColorToken::TextFaint,       "Theme.TextTertiary",    0xFF939EAB},
    {ColorToken::Border,          "Theme.Border",          0xFFD5DEEB},
    {ColorToken::BorderSubtle,    "Theme.BorderSubtle",    0xFFE4EAF4},
    {ColorToken::Accent,          "Theme.Accent",          0xFF0A84FF},
    {ColorToken::Accent2,         "Theme.Accent2",         0xFF3E9BFF},
    {ColorToken::AccentText,      "Theme.AccentText",      0xFFFFFFFF},
    {ColorToken::AccentText2,     "Theme.AccentText2",     0xFF0A6BD6},
    {ColorToken::AccentSubtle,    "Theme.AccentSubtle",    0xFFE9F2FF},
    {ColorToken::AccentSoft,      "Theme.AccentSoft",      0x330A84FF},
    {ColorToken::AccentHover,     "Theme.AccentHover",     0xFF3E9BFF},
    {ColorToken::AccentPressed,   "Theme.AccentPressed",   0xFF0069D9},
    {ColorToken::Hover,           "Theme.Hover",           0xFFEEF4FE},
    {ColorToken::Press,           "Theme.Press",           0xFFE2EDFC},
    {ColorToken::InputBg,         "Theme.InputBg",         0xE6FFFFFF},
    {ColorToken::InputBorder,     "Theme.InputBorder",     0xFFCFDDEE},
    {ColorToken::InputHover,      "Theme.InputHover",      0xFFA9C7E6},
    {ColorToken::InputFocus,      "Theme.InputFocus",      0xFF0A84FF},
    {ColorToken::Success,         "Theme.Success",         0xFF34A06A},
    {ColorToken::SuccessBg,       "Theme.SuccessBg",       0xFFEBF7F0},
    {ColorToken::Warning,         "Theme.Warning",         0xFFC07A16},
    {ColorToken::WarningBg,       "Theme.WarningBg",       0xFFFDF5E8},
    {ColorToken::WarningText,     "Theme.WarningText",     0xFFC07A16},
    {ColorToken::Danger,          "Theme.Danger",          0xFFD0503F},
    {ColorToken::DangerBg,        "Theme.DangerBg",        0xFFFCEFED},
    {ColorToken::Ok,              "Theme.OkGreen",         0xFF34A06A},
    {ColorToken::OkBg,            "Theme.OkGreenBg",       0xFFEBF7F0},
    {ColorToken::Info,            "Theme.Info",            0xFF0A84FF},
    {ColorToken::InfoBg,          "Theme.InfoBg",          0xFFE9F2FF},
    {ColorToken::LogInfo,         "Theme.LogInfo",         0xFF0A6BD6},
    {ColorToken::LogSuccess,      "Theme.LogSuccess",      0xFF34A06A},
    {ColorToken::LogWarning,      "Theme.LogWarning",      0xFFC07A16},
    {ColorToken::LogError,        "Theme.LogError",        0xFFD0503F},
    {ColorToken::ProgTrack,       "Theme.ProgTrack",       0xFFEDF3FA},
    {ColorToken::ProgressText,    "Theme.ProgressText",    0xFFFFFFFF},
    {ColorToken::PanelHeadBg,     "Theme.PanelHeadBg",     0xCCF4F7FE},
    {ColorToken::GridLine,        "Theme.GridLine",        0xFFE9EFF7},
    {ColorToken::TabStripBg,      "Theme.TabStripBg",      0xCCF0F4FF},
    {ColorToken::StatusBarBg,     "Theme.StatusBarBg",     0xCCF0F4FF},
    {ColorToken::BtnBorder,       "Theme.BtnBorder",       0xFFB9D5F5},
    {ColorToken::BtnText,         "Theme.BtnText",         0xFF0A6BD6},
    {ColorToken::BtnHoverBg,      "Theme.BtnHoverBg",      0xFFF3F8FF},
    {ColorToken::GroupHeader,     "Theme.GroupHeader",     0xFF2A3441},
    {ColorToken::GlassEdgeTop,    "Theme.GlassEdgeTop",    0xF2FFFFFF},
    {ColorToken::GlassEdgeBottom, "Theme.GlassEdgeBottom", 0x40000000},
    {ColorToken::GlassSolid,      "Theme.GlassSolid",      0xF2FFFFFF},
    {ColorToken::ShadowColor,     "Theme.ShadowColor",     0x14000000},
    {ColorToken::RowHover,        "Theme.RowHover",        0xFFF2F7FE},
    {ColorToken::RowSelected,     "Theme.RowSelected",     0xFFDFEBFB},
};

} // namespace

ThemeManager& ThemeManager::instance() {
    static ThemeManager mgr;
    return mgr;
}

ThemeManager::ThemeManager() {
    loadTokenTable();
}

void ThemeManager::loadTokenTable() {
    const TokenEntry* table = m_dark ? kDarkTokens : kLightTokens;
    const size_t count = m_dark ? (sizeof(kDarkTokens) / sizeof(kDarkTokens[0]))
                                : (sizeof(kLightTokens) / sizeof(kLightTokens[0]));
    m_byKey.clear();
    for (size_t i = 0; i < count; ++i) {
        const TokenEntry& e = table[i];
        const QColor c = QColor::fromRgba(e.argb);
        m_colors[static_cast<int>(e.token)] = c;
        m_byKey.insert(QString::fromLatin1(e.key), c);
    }
    applyAccentOverride();
}

void ThemeManager::applyAccentOverride()
{
    if (!m_accent.isValid()) return;

    const QColor a = m_accent;
    // 直接写 token 表：它是 QSS 占位符的取值来源，也是 colorByKey 的来源
    // （下面统一把字符串表重建一次，避免两处不一致）。
    const auto setToken = [this](ColorToken token, const QColor& c) {
        m_colors[static_cast<int>(token)] = c;
    };

    // 深色主题：主色偏亮（压在深底上要够跳），派生色一律往亮走。
    // 浅色主题：主色要压暗（压在白底上要够沉），「深一档」是往下压。
    const QColor accent2 = m_dark ? lighten(a, 0.30) : lighten(a, 0.14);
    const QColor hover = m_dark ? lighten(a, 0.14) : darken(a, 0.06);
    const QColor pressed = m_dark ? darken(a, 0.10) : darken(a, 0.16);
    const QColor text2 = m_dark ? lighten(a, 0.52) : darken(a, 0.20);
    const QColor subtle = m_dark ? tintToward(a, QColor(0x17, 0x19, 0x1C), 0.18)
                                 : tintToward(a, Qt::white, 0.12);

    setToken(ColorToken::Accent, a);
    setToken(ColorToken::Accent2, accent2);
    setToken(ColorToken::AccentHover, hover);
    setToken(ColorToken::AccentPressed, pressed);
    setToken(ColorToken::AccentText2, text2);
    setToken(ColorToken::AccentSubtle, subtle);
    setToken(ColorToken::AccentSoft, QColor(a.red(), a.green(), a.blue(), 0x33));
    setToken(ColorToken::InputFocus, a);
    setToken(ColorToken::Info, a);
    setToken(ColorToken::InfoBg, subtle);

    // 字符串表同步（colorByKey / 自绘控件取色都走它）。
    // 键名与 token 的对应关系在 token 表里，这里按 token 反查一次即可。
    const TokenEntry* table = m_dark ? kDarkTokens : kLightTokens;
    const size_t count = m_dark ? (sizeof(kDarkTokens) / sizeof(kDarkTokens[0]))
                                : (sizeof(kLightTokens) / sizeof(kLightTokens[0]));
    for (size_t i = 0; i < count; ++i) {
        const TokenEntry& e = table[i];
        m_byKey.insert(QString::fromLatin1(e.key), m_colors[static_cast<int>(e.token)]);
    }
}

void ThemeManager::setAccent(const QColor& accent)
{
    const QColor next = accent.isValid() ? accent : QColor();
    if (next.rgba() == m_accent.rgba()) return;

    m_accent = next;
    loadTokenTable();        // 重置整张表 + 重新应用覆盖
    refreshStyleSheet();     // 重放 QSS（%ACCENT% 是替换进文本的）
    emit themeChanged(m_dark);
}

QString ThemeManager::expandAccent(const QString& qss) const
{
    const QColor a = color(ColorToken::Accent);
    const QColor a2 = color(ColorToken::Accent2);
    // %ACCENT_DEEP% 只出现在浅色 QSS 里：白底上要一个更沉的主色才够对比。
    // 直接用 AccentText2（浅色 = 深一档的主色、深色 = 亮一档）语义刚好对上。
    const QColor deep = color(ColorToken::AccentText2);
    const QColor subtle = color(ColorToken::AccentSubtle);

    const auto hex = [](const QColor& c) { return c.name(QColor::HexRgb).toUpper(); };

    QString out = qss;
    // 🔴 先长后短：%ACCENT2_R% 必须在 %ACCENT% 之前换掉，否则会被切成
    //    "<主色>2_R%" 这种残渣（纯文本替换没有词边界）。
    out.replace(QLatin1String("%ACCENT2_R%"), QString::number(a2.red()));
    out.replace(QLatin1String("%ACCENT2_G%"), QString::number(a2.green()));
    out.replace(QLatin1String("%ACCENT2_B%"), QString::number(a2.blue()));
    out.replace(QLatin1String("%ACCENT2%"), hex(a2));

    out.replace(QLatin1String("%ACCENT_R%"), QString::number(a.red()));
    out.replace(QLatin1String("%ACCENT_G%"), QString::number(a.green()));
    out.replace(QLatin1String("%ACCENT_B%"), QString::number(a.blue()));

    out.replace(QLatin1String("%ACCENT_SUBTLE%"), hex(subtle));
    out.replace(QLatin1String("%ACCENT_DEEP%"), hex(deep));
    out.replace(QLatin1String("%ACCENT%"), hex(a));
    return out;
}

void ThemeManager::setDark(bool dark) {
    if (m_dark == dark) return;
    m_dark = dark;
    loadTokenTable();
    emit themeChanged(m_dark);
}

void ThemeManager::toggle() {
    setDark(!m_dark);
}

void ThemeManager::apply(QApplication& app) {
    // 临时诊断开关：KC_NOSTYLE=1 时跳过样式表。
    if (qEnvironmentVariableIsSet("KC_NOSTYLE")) return;
    const QString path = m_dark ? QStringLiteral(":/styles/dark.qss")
                                : QStringLiteral(":/styles/light.qss");
    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // 先换强调色占位符，再换玻璃 alpha —— 两步都是纯文本替换，顺序无关，
        // 但分开写便于各自维护（强调色是 2026-10-24 新增的一层）。
        app.setStyleSheet(expandAccent(expandGlassAlpha(QString::fromUtf8(f.readAll()))));
    }
    // 加载失败时保留既有样式，绝不抛。
}

// ─────────────────────────────────────────────────────────────────────────────
//  玻璃透明度：QSS 占位符 → 实际 alpha（2026-10-12）
//
//  为什么要在加载时替换：Qt 的 QSS **不支持 CSS 变量**，而「卡片不透明度 /
//  界面透明」两个滑条的值是运行时可变的 —— 静态样式表写死就永远是死的。
//
//  语义逐条对齐 C# MainWindow.ApplyUiTransparency / ApplyCardOpacity：
//    · 界面透明 = 0 → 页面根完全不透明（看不到背景）；= 1 → 全透出（背景全露）；
//    · 卡片不透明度 = 1 → 令牌自带的玻璃基准 alpha 原样（玻璃还在）；= 0 → 真全透明。
//      基准 alpha 取自令牌表（CardBg / SurfaceAlt），主题一换自动跟着变，
//      不再手写数字（写死 = 换主题就失配）。
// ─────────────────────────────────────────────────────────────────────────────
QString ThemeManager::expandGlassAlpha(const QString& qss) const
{
    const auto& s = kc::AppSettings::current();
    const double cardOpacity = qBound(0.0, s.cardOpacity, 1.0);
    const double uiTransparency = qBound(0.0, s.uiTransparency, 1.0);

    const int cardBase = qMax(1, color(ColorToken::CardBg).alpha());
    const int panelBase = qMax(1, color(ColorToken::SurfaceAlt).alpha());

    QString out = qss;
    const int pageA = qRound((1.0 - uiTransparency) * 255.0);
    const int cardA = qRound(cardOpacity * cardBase);

    out.replace(QLatin1String(kAlphaPage), QString::number(pageA));
    out.replace(QLatin1String(kAlphaCard), QString::number(cardA));
    out.replace(QLatin1String(kAlphaPanel),
                QString::number(qRound(cardOpacity * panelBase)));

    // 渐变两端：只动「同一个值」的 ±18%，两端永远以 cardOpacity 为轴，
    // 拉到 0 时整张卡片彻底消失（不是留下一条灰边）。
    out.replace(QLatin1String(kAlphaCardHi), QString::number(qBound(0, qRound(cardA * 1.18), 255)));
    out.replace(QLatin1String(kAlphaCardLo), QString::number(qBound(0, qRound(cardA * 0.82), 255)));
    out.replace(QLatin1String(kAlphaPageHi), QString::number(qBound(0, qRound(pageA * 1.06), 255)));
    out.replace(QLatin1String(kAlphaPageLo), QString::number(qBound(0, qRound(pageA * 0.94), 255)));
    return out;
}

void ThemeManager::refreshStyleSheet()
{
    // 滑条改动后必须重新套一次样式表：alpha 是替换进 QSS 文本的，不重放就还是旧值。
    if (qApp) apply(*qApp);
}

QColor ThemeManager::color(ColorToken token) const {
    const int idx = static_cast<int>(token);
    if (idx < 0 || idx >= static_cast<int>(sizeof(m_colors) / sizeof(m_colors[0])))
        return QColor(Qt::transparent);
    return m_colors[idx];
}

QColor ThemeManager::colorByKey(const QString& key) const {
    auto it = m_byKey.constFind(key);
    return it != m_byKey.constEnd() ? it.value() : QColor(Qt::transparent);
}

} // namespace kc::ui
