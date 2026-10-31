// 主题包（kc/config/ThemePack）的回归测试。
//
// 主题包是**拿来分享的文件**，所以两类问题最要命：
//   · 往返丢字段 —— 导出的包再导回来，外观变了；
//   · 非法值不拦 —— 别人手改过的包把界面搞成不可用（比如透明度填 5.0）。
// 这两类都不会崩，只会「看起来怪」，所以必须用断言钉住。

#include "kc/config/ThemePack.h"
#include "kc/config/AppSettings.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL " << what << '\n';
    }
}

void checkStr(const QString& got, const QString& want, const char* what)
{
    if (got == want) {
        std::cout << "PASS " << what << '\n';
    } else {
        ++failures;
        std::cerr << "FAIL: " << what << "\n  got:  [" << got.toStdString()
                  << "]\n  want: [" << want.toStdString() << "]\n";
    }
}

bool nearly(double a, double b) { return std::abs(a - b) < 1e-9; }

} // namespace

int main()
{
    using namespace kc::config;

    // ── ① JSON 往返：每个字段都要能读回来 ──────────────────────────────────
    {
        ThemePack p;
        p.name = QStringLiteral("测试包");
        p.description = QStringLiteral("一句话说明");
        p.author = QStringLiteral("小坤");
        p.dark = true;
        p.accentColor = QStringLiteral("#123456");
        p.cardOpacity = 0.42;
        p.uiTransparency = 0.73;
        p.glassTint = 0.61;
        p.glassFrost = 33.5;
        p.glassRefraction = 0.44;
        p.glassDispersion = 0.25;
        p.glassHighlight = 0.88;
        p.islandTheme = QStringLiteral("glass");
        p.islandRadius = 33;
        p.islandColumns = 7;
        p.islandDim = false;
        p.islandLiteMotion = true;
        p.consoleWidgets = false;

        QByteArray json;
        QString err;
        check(themePackToJson(p, &json, &err), "pack serialises");
        check(!json.isEmpty(), "serialised json is not empty");

        ThemePack back;
        check(themePackFromJson(json, &back, &err), "pack deserialises");
        checkStr(back.name, p.name, "name round-trips");
        checkStr(back.description, p.description, "description round-trips");
        checkStr(back.author, p.author, "author round-trips");
        check(back.dark == p.dark, "dark round-trips");
        checkStr(back.accentColor, p.accentColor, "accent colour round-trips");
        check(nearly(back.cardOpacity, p.cardOpacity), "card opacity round-trips");
        check(nearly(back.uiTransparency, p.uiTransparency), "ui transparency round-trips");
        check(nearly(back.glassTint, p.glassTint), "glass tint round-trips");
        check(nearly(back.glassFrost, p.glassFrost), "glass frost round-trips");
        check(nearly(back.glassRefraction, p.glassRefraction), "glass refraction round-trips");
        check(nearly(back.glassDispersion, p.glassDispersion), "glass dispersion round-trips");
        check(nearly(back.glassHighlight, p.glassHighlight), "glass highlight round-trips");
        checkStr(back.islandTheme, p.islandTheme, "island theme round-trips");
        check(back.islandRadius == p.islandRadius, "island radius round-trips");
        check(back.islandColumns == p.islandColumns, "island columns round-trips");
        check(back.islandDim == p.islandDim, "island dim round-trips");
        check(back.islandLiteMotion == p.islandLiteMotion, "island lite motion round-trips");
        check(back.consoleWidgets == p.consoleWidgets, "console widgets round-trips");
    }

    // ── ② 非法值必须被拒绝，且错误要指出**哪个字段** ──────────────────────
    {
        QString err;

        ThemePack noName;
        check(!validateThemePack(noName, &err), "pack without a name is rejected");
        check(err.contains(QStringLiteral("名称")), "error names the missing field");

        ThemePack badOpacity;
        badOpacity.name = QStringLiteral("x");
        badOpacity.cardOpacity = 5.0;
        check(!validateThemePack(badOpacity, &err), "out-of-range opacity is rejected");
        check(err.contains(QStringLiteral("不透明度")), "error names the opacity field");

        ThemePack badAccent;
        badAccent.name = QStringLiteral("x");
        badAccent.accentColor = QStringLiteral("red");
        check(!validateThemePack(badAccent, &err), "non-hex accent colour is rejected");
        check(err.contains(QStringLiteral("强调色")), "error names the accent field");

        ThemePack badTheme;
        badTheme.name = QStringLiteral("x");
        badTheme.islandTheme = QStringLiteral("rainbow");
        check(!validateThemePack(badTheme, &err), "unknown island theme is rejected");

        // 空强调色是合法的（= 用主题内置色）
        ThemePack emptyAccent;
        emptyAccent.name = QStringLiteral("x");
        emptyAccent.accentColor = QString();
        check(validateThemePack(emptyAccent, &err), "empty accent colour is allowed");
    }

    // ── ③ 向前兼容：缺字段走默认值，多余字段忽略 ──────────────────────────
    {
        // 只给最小字段（手写的老版本包）
        const QByteArray minimal = R"({"schema":1,"name":"只有名字"})";
        ThemePack p;
        QString err;
        check(themePackFromJson(minimal, &p, &err), "minimal pack parses");
        checkStr(p.name, QStringLiteral("只有名字"), "minimal pack keeps the name");
        check(nearly(p.cardOpacity, 0.86), "missing field falls back to default");

        // schema 比当前新 → 明确拒绝（不要按老结构硬读）
        const QByteArray newer = R"({"schema":999,"name":"来自未来"})";
        ThemePack p2;
        check(!themePackFromJson(newer, &p2, &err), "future schema is rejected");
        check(err.contains(QStringLiteral("更新")), "error explains the version problem");

        // 不是 JSON
        ThemePack p3;
        check(!themePackFromJson(QByteArray("not json"), &p3, &err),
              "non-JSON content is rejected");
        check(!err.isEmpty(), "non-JSON rejection explains why");
    }

    // ── ④ 内置预设：都合法、名字唯一、风格真的不同 ────────────────────────
    {
        const QVector<ThemePack> packs = builtinThemePacks();
        check(packs.size() >= 5, "at least five builtin packs");

        QString err;
        for (const ThemePack& p : packs) {
            if (!validateThemePack(p, &err)) {
                std::cerr << "  invalid builtin: " << p.name.toStdString()
                          << " — " << err.toStdString() << '\n';
            }
            check(validateThemePack(p, &err), "every builtin pack is valid");
            check(!p.name.trimmed().isEmpty(), "every builtin pack has a name");
            check(!p.description.trimmed().isEmpty(), "every builtin pack has a description");
        }

        // 名字不能重复（界面上要按名字选）
        for (int i = 0; i < packs.size(); ++i) {
            for (int j = i + 1; j < packs.size(); ++j) {
                if (packs.at(i).name == packs.at(j).name) {
                    std::cerr << "  duplicate name: " << packs.at(i).name.toStdString() << '\n';
                }
                check(packs.at(i).name != packs.at(j).name, "builtin pack names are unique");
            }
        }

        // 「风格真的不同」：至少要有深色和浅色两种，且玻璃浓度跨度足够大
        bool hasDark = false, hasLight = false;
        double minTint = 1.0, maxTint = 0.0;
        for (const ThemePack& p : packs) {
            if (p.dark) hasDark = true; else hasLight = true;
            minTint = qMin(minTint, p.glassTint);
            maxTint = qMax(maxTint, p.glassTint);
        }
        check(hasDark && hasLight, "builtin packs cover both dark and light");
        check(maxTint - minTint > 0.4, "builtin packs span a real range of glass density");

        // 按名字查找
        ThemePack found;
        check(findBuiltinThemePack(QStringLiteral("深空"), &found), "find by name works");
        check(found.dark, "found pack carries its own values");
        check(!findBuiltinThemePack(QStringLiteral("不存在"), &found),
              "unknown name is not found");
    }

    // ── ⑤ 抓当前设置 → 应用回去：值要一致 ─────────────────────────────────
    {
        kc::AppSettings& s = kc::AppSettings::current();

        // 先记下原值，测完恢复（这是个全局单例，别污染后面的用例）
        const double savedTint = s.glassTint;
        const double savedOpacity = s.cardOpacity;
        const QString savedAccent = s.accentColor;

        s.glassTint = 0.31;
        s.cardOpacity = 0.55;
        s.accentColor = QStringLiteral("#ABCDEF");

        const ThemePack captured = captureCurrentThemePack();
        check(nearly(captured.glassTint, 0.31), "capture picks up glass tint");
        check(nearly(captured.cardOpacity, 0.55), "capture picks up card opacity");
        checkStr(captured.accentColor, QStringLiteral("#ABCDEF"), "capture picks up accent");

        // 改掉再应用回来
        s.glassTint = 0.99;
        s.cardOpacity = 0.11;
        applyThemePackToSettings(captured);
        check(nearly(s.glassTint, 0.31), "apply restores glass tint");
        check(nearly(s.cardOpacity, 0.55), "apply restores card opacity");
        checkStr(s.accentColor, QStringLiteral("#ABCDEF"), "apply restores accent");

        // 恢复
        s.glassTint = savedTint;
        s.cardOpacity = savedOpacity;
        s.accentColor = savedAccent;
    }

    // ── ⑥ 文件名清洗 ──────────────────────────────────────────────────────
    {
        const QString f1 = suggestedThemePackFileName(QStringLiteral("深空"));
        check(f1.endsWith(QStringLiteral(".kcskin")), "file name has the right extension");
        checkStr(f1, QStringLiteral("深空.kcskin"), "normal name is preserved");

        // 带路径分隔符 / 非法字符的要被清洗（否则会写到别的目录去）
        const QString f2 = suggestedThemePackFileName(QStringLiteral("a/b:c*d"));
        check(!f2.contains(QLatin1Char('/')), "path separators are stripped");
        check(!f2.contains(QLatin1Char(':')), "colons are stripped");
        check(!f2.contains(QLatin1Char('*')), "asterisks are stripped");

        // 空名字要有兜底
        const QString f3 = suggestedThemePackFileName(QString());
        check(!f3.isEmpty(), "empty name still yields a file name");
        check(f3.endsWith(QStringLiteral(".kcskin")), "fallback keeps the extension");
    }

    if (failures == 0)
        std::cout << "ThemePackTest: all checks passed\n";
    else
        std::cerr << "ThemePackTest: " << failures << " check(s) failed\n";
    return failures == 0 ? 0 : 1;
}
