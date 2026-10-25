#include "ui/theme/GlassSettings.h"

#include "kc/config/AppSettings.h"

#include <algorithm>
#include <cmath>

namespace kc::ui {
namespace {

/// 四档预设的数值是**照着真机观感调的**，不是随便填的区间端点：
///  · crystal   —— 像一层水膜。磨砂必须很小（>14 就糊成雾），折射要给足，
///                 否则「清透」在观感上等于「没做玻璃」。
///  · standard  —— iOS 26 出厂那档。各参数都在中位，最不容易出错。
///  · frosted   —— 毛玻璃。磨砂拉满、折射压低（糊了以后折射也看不出来，
///                 给高只是白烧 CPU），高光收一点免得发灰。
///  · minimal   —— 折射 / 色散全关，纯「半透明 + 顶高光」。给低配机和不
///                 喜欢花哨效果的人一条退路 —— 这条路径的绘制成本几乎为零。
const QVector<GlassPreset>& presetTable()
{
    static const QVector<GlassPreset> table = [] {
        QVector<GlassPreset> v;

        GlassPreset crystal;
        crystal.id = QStringLiteral("crystal");
        crystal.name = QStringLiteral("清透");
        crystal.hint = QStringLiteral("像一层水膜：几乎全透，边缘折射最明显");
        crystal.style.tint = 0.34;
        crystal.style.frost = 10.0;
        crystal.style.refraction = 0.80;
        crystal.style.dispersion = 0.52;
        crystal.style.highlight = 1.20;
        v.push_back(crystal);

        GlassPreset standard;
        standard.id = QStringLiteral("standard");
        standard.name = QStringLiteral("标准");
        standard.hint = QStringLiteral("iOS 26 出厂观感，最不容易出错的一档");
        standard.style.tint = 0.58;
        standard.style.frost = 22.0;
        standard.style.refraction = 0.55;
        standard.style.dispersion = 0.35;
        standard.style.highlight = 1.00;
        v.push_back(standard);

        GlassPreset frosted;
        frosted.id = QStringLiteral("frosted");
        frosted.name = QStringLiteral("磨砂");
        frosted.hint = QStringLiteral("毛玻璃：糊得最狠，适合花哨壁纸");
        frosted.style.tint = 0.78;
        frosted.style.frost = 40.0;
        frosted.style.refraction = 0.22;
        frosted.style.dispersion = 0.12;
        frosted.style.highlight = 0.78;
        v.push_back(frosted);

        GlassPreset minimal;
        minimal.id = QStringLiteral("minimal");
        minimal.name = QStringLiteral("极简");
        minimal.hint = QStringLiteral("关掉折射与色散，最省性能");
        minimal.style.tint = 0.62;
        minimal.style.frost = 18.0;
        minimal.style.refraction = 0.0;
        minimal.style.dispersion = 0.0;
        minimal.style.highlight = 0.55;
        v.push_back(minimal);

        return v;
    }();
    return table;
}

bool nearly(qreal a, qreal b, qreal eps = 0.02)
{
    return std::abs(a - b) <= eps;
}

} // namespace

const QVector<GlassPreset>& glassPresets() { return presetTable(); }

const GlassPreset* findGlassPreset(const QString& id)
{
    for (const auto& p : presetTable()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

glass::Style currentGlassStyle()
{
    const kc::AppSettings& s = kc::AppSettings::current();
    glass::Style st;
    st.tint = std::clamp(s.glassTint, 0.0, 1.0);
    st.frost = std::clamp(s.glassFrost, 0.0, 48.0);
    st.refraction = std::clamp(s.glassRefraction, 0.0, 1.0);
    st.dispersion = std::clamp(s.glassDispersion, 0.0, 1.0);
    st.highlight = std::clamp(s.glassHighlight, 0.0, 1.6);
    return st;
}

void storeGlassStyle(const glass::Style& style, const QString& presetId)
{
    kc::AppSettings& s = kc::AppSettings::current();
    s.glassTint = std::clamp(style.tint, 0.0, 1.0);
    s.glassFrost = std::clamp(style.frost, 0.0, 48.0);
    s.glassRefraction = std::clamp(style.refraction, 0.0, 1.0);
    s.glassDispersion = std::clamp(style.dispersion, 0.0, 1.0);
    s.glassHighlight = std::clamp(style.highlight, 0.0, 1.6);
    s.glassPreset = presetId.isEmpty() ? QStringLiteral("custom") : presetId;
}

const GlassPreset& applyGlassPreset(const QString& id)
{
    const GlassPreset* p = findGlassPreset(id);
    if (!p) return presetTable().at(1);   // 找不到就退回标准档

    glass::Style st = p->style;
    // 光源位置 / 按压形变是运行时状态，不参与预设。
    st.lightPos = currentGlassStyle().lightPos;
    st.press = 0.0;
    storeGlassStyle(st, p->id);
    return *p;
}

QString matchGlassPreset(const glass::Style& style)
{
    for (const auto& p : presetTable()) {
        if (nearly(style.tint, p.style.tint) && nearly(style.frost, p.style.frost, 0.6)
            && nearly(style.refraction, p.style.refraction) && nearly(style.dispersion, p.style.dispersion)
            && nearly(style.highlight, p.style.highlight, 0.03)) {
            return p.id;
        }
    }
    return QStringLiteral("custom");
}

} // namespace kc::ui
