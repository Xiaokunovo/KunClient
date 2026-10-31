#include "kc/config/ThemePack.h"

#include "kc/config/AppSettings.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace kc::config {

namespace {

/// 读一个 double 字段，缺失时保留默认值。
double readDouble(const QJsonObject& o, const char* key, double fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : fallback;
}

int readInt(const QJsonObject& o, const char* key, int fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toInt() : fallback;
}

bool readBool(const QJsonObject& o, const char* key, bool fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : fallback;
}

QString readString(const QJsonObject& o, const char* key, const QString& fallback)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : fallback;
}

/// 检查一个 double 是否落在区间内；不在就写一条具体错误。
bool inRange(double v, double lo, double hi, const char* label, QString* error)
{
    if (v >= lo && v <= hi) return true;
    if (error) {
        *error = QStringLiteral("%1 超出范围（%2 ~ %3，实际 %4）")
                     .arg(QString::fromUtf8(label))
                     .arg(lo).arg(hi).arg(v);
    }
    return false;
}

} // namespace

bool validateThemePack(const ThemePack& pack, QString* error)
{
    if (pack.name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("主题包名称不能为空");
        return false;
    }
    if (pack.schema < 1) {
        if (error) *error = QStringLiteral("主题包版本号无效（%1）").arg(pack.schema);
        return false;
    }

    // 强调色：空 = 用默认；非空必须是 #rrggbb。
    const QString accent = pack.accentColor.trimmed();
    if (!accent.isEmpty()) {
        static const QRegularExpression re(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
        if (!re.match(accent).hasMatch()) {
            if (error) *error = QStringLiteral("强调色格式不对（要 #rrggbb，实际 %1）").arg(accent);
            return false;
        }
    }

    if (!inRange(pack.cardOpacity, 0.0, 1.0, "卡片不透明度", error)) return false;
    if (!inRange(pack.uiTransparency, 0.0, 1.0, "界面透明度", error)) return false;
    if (!inRange(pack.glassTint, 0.0, 1.0, "玻璃染色浓度", error)) return false;
    if (!inRange(pack.glassFrost, 0.0, 60.0, "玻璃磨砂半径", error)) return false;
    if (!inRange(pack.glassRefraction, 0.0, 1.0, "玻璃边缘折射", error)) return false;
    if (!inRange(pack.glassDispersion, 0.0, 1.0, "玻璃色散", error)) return false;
    if (!inRange(pack.glassHighlight, 0.0, 1.0, "玻璃顶沿高光", error)) return false;

    if (!inRange(pack.islandRadius, 10.0, 40.0, "灵动岛圆角", error)) return false;
    if (!inRange(pack.islandColumns, 4.0, 8.0, "灵动岛面板列数", error)) return false;

    const QString theme = pack.islandTheme.trimmed();
    if (theme != QStringLiteral("ink") && theme != QStringLiteral("brand")
        && theme != QStringLiteral("glass")) {
        if (error) *error = QStringLiteral("灵动岛配色不认（%1），只支持 ink / brand / glass").arg(theme);
        return false;
    }
    return true;
}

bool themePackToJson(const ThemePack& pack, QByteArray* out, QString* error)
{
    if (!validateThemePack(pack, error)) return false;

    QJsonObject o;
    o.insert(QStringLiteral("schema"), pack.schema);
    o.insert(QStringLiteral("name"), pack.name);
    o.insert(QStringLiteral("description"), pack.description);
    o.insert(QStringLiteral("author"), pack.author);

    o.insert(QStringLiteral("dark"), pack.dark);
    o.insert(QStringLiteral("accentColor"), pack.accentColor);
    o.insert(QStringLiteral("cardOpacity"), pack.cardOpacity);
    o.insert(QStringLiteral("uiTransparency"), pack.uiTransparency);

    o.insert(QStringLiteral("glassTint"), pack.glassTint);
    o.insert(QStringLiteral("glassFrost"), pack.glassFrost);
    o.insert(QStringLiteral("glassRefraction"), pack.glassRefraction);
    o.insert(QStringLiteral("glassDispersion"), pack.glassDispersion);
    o.insert(QStringLiteral("glassHighlight"), pack.glassHighlight);

    o.insert(QStringLiteral("islandTheme"), pack.islandTheme);
    o.insert(QStringLiteral("islandRadius"), pack.islandRadius);
    o.insert(QStringLiteral("islandColumns"), pack.islandColumns);
    o.insert(QStringLiteral("islandDim"), pack.islandDim);
    o.insert(QStringLiteral("islandLiteMotion"), pack.islandLiteMotion);

    o.insert(QStringLiteral("consoleWidgets"), pack.consoleWidgets);

    if (out) *out = QJsonDocument(o).toJson(QJsonDocument::Indented);
    return true;
}

bool themePackFromJson(const QByteArray& json, ThemePack* out, QString* error)
{
    if (!out) return false;

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError) {
        if (error) {
            *error = QStringLiteral("文件不是合法 JSON（第 %1 字节：%2）")
                         .arg(perr.offset).arg(perr.errorString());
        }
        return false;
    }
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("文件内容不是 JSON 对象");
        return false;
    }
    const QJsonObject o = doc.object();

    // schema 版本检查：比当前新就明确拒绝（而不是按老结构硬读、把配置读坏）。
    const int schema = readInt(o, "schema", 0);
    if (schema > kThemePackSchema) {
        if (error) {
            *error = QStringLiteral("这个主题包来自更新的版本（格式 %1，本程序支持到 %2）")
                         .arg(schema).arg(kThemePackSchema);
        }
        return false;
    }

    ThemePack p;
    p.schema = (schema > 0) ? schema : kThemePackSchema;
    p.name = readString(o, "name", QString());
    p.description = readString(o, "description", QString());
    p.author = readString(o, "author", QString());

    p.dark = readBool(o, "dark", p.dark);
    p.accentColor = readString(o, "accentColor", p.accentColor);
    p.cardOpacity = readDouble(o, "cardOpacity", p.cardOpacity);
    p.uiTransparency = readDouble(o, "uiTransparency", p.uiTransparency);

    p.glassTint = readDouble(o, "glassTint", p.glassTint);
    p.glassFrost = readDouble(o, "glassFrost", p.glassFrost);
    p.glassRefraction = readDouble(o, "glassRefraction", p.glassRefraction);
    p.glassDispersion = readDouble(o, "glassDispersion", p.glassDispersion);
    p.glassHighlight = readDouble(o, "glassHighlight", p.glassHighlight);

    p.islandTheme = readString(o, "islandTheme", p.islandTheme);
    p.islandRadius = readInt(o, "islandRadius", p.islandRadius);
    p.islandColumns = readInt(o, "islandColumns", p.islandColumns);
    p.islandDim = readBool(o, "islandDim", p.islandDim);
    p.islandLiteMotion = readBool(o, "islandLiteMotion", p.islandLiteMotion);

    p.consoleWidgets = readBool(o, "consoleWidgets", p.consoleWidgets);

    // 读完必须再校验一次：文件里的值可能超范围（手改过的、或别的工具生成的）。
    if (!validateThemePack(p, error)) return false;

    *out = p;
    return true;
}

ThemePack captureCurrentThemePack()
{
    const kc::AppSettings& s = kc::AppSettings::current();
    ThemePack p;
    p.name = QString();
    p.description = QString();
    p.author = QString();

    p.dark = false;   // 由调用方按 ThemeManager 当前状态填（这里拿不到）
    p.accentColor = s.accentColor;
    p.cardOpacity = s.cardOpacity;
    p.uiTransparency = s.uiTransparency;

    p.glassTint = s.glassTint;
    p.glassFrost = s.glassFrost;
    p.glassRefraction = s.glassRefraction;
    p.glassDispersion = s.glassDispersion;
    p.glassHighlight = s.glassHighlight;

    p.islandTheme = s.islandTheme;
    p.islandRadius = s.islandRadius;
    p.islandColumns = s.islandColumns;
    p.islandDim = s.islandDim;
    p.islandLiteMotion = s.islandLiteMotion;

    p.consoleWidgets = s.consoleWidgets;
    return p;
}

void applyThemePackToSettings(const ThemePack& pack)
{
    kc::AppSettings& s = kc::AppSettings::current();

    s.accentColor = pack.accentColor;
    s.cardOpacity = pack.cardOpacity;
    s.uiTransparency = pack.uiTransparency;

    s.glassTint = pack.glassTint;
    s.glassFrost = pack.glassFrost;
    s.glassRefraction = pack.glassRefraction;
    s.glassDispersion = pack.glassDispersion;
    s.glassHighlight = pack.glassHighlight;

    s.islandTheme = pack.islandTheme;
    s.islandRadius = pack.islandRadius;
    s.islandColumns = pack.islandColumns;
    s.islandDim = pack.islandDim;
    s.islandLiteMotion = pack.islandLiteMotion;

    s.consoleWidgets = pack.consoleWidgets;
    // 🔴 不 save()、不 applyIslandLive()、不 refreshStyleSheet()：
    //    落盘与重绘的时机由调用方决定（设置页是「点保存才生效」，
    //    导入主题包可能想先预览再确认）。这里只改内存。
}

QVector<ThemePack> builtinThemePacks()
{
    QVector<ThemePack> list;

    // ① 默认 —— 就是当前出厂观感，浅色 + 标准玻璃。
    {
        ThemePack p;
        p.name = QStringLiteral("默认");
        p.description = QStringLiteral("出厂观感：浅色 + 标准液态玻璃");
        list.append(p);
    }

    // ② 深空 —— 深色 + 冷蓝强调 + 更浓的玻璃。
    {
        ThemePack p;
        p.name = QStringLiteral("深空");
        p.description = QStringLiteral("深色冷调：玻璃更浓、边缘折射更明显");
        p.dark = true;
        p.accentColor = QStringLiteral("#5B8DEF");
        p.cardOpacity = 0.72;
        p.uiTransparency = 0.92;
        p.glassTint = 0.72;
        p.glassFrost = 30.0;
        p.glassRefraction = 0.68;
        p.glassDispersion = 0.45;
        p.glassHighlight = 1.0;
        p.islandTheme = QStringLiteral("glass");
        p.islandRadius = 30;
        p.islandColumns = 6;
        list.append(p);
    }

    // ③ 暖沙 —— 浅色 + 琥珀强调 + 低玻璃浓度（看着更「纸感」）。
    {
        ThemePack p;
        p.name = QStringLiteral("暖沙");
        p.description = QStringLiteral("浅色暖调：玻璃很淡，像纸面而不是玻璃");
        p.dark = false;
        p.accentColor = QStringLiteral("#C77B3C");
        p.cardOpacity = 0.94;
        p.uiTransparency = 0.88;
        p.glassTint = 0.28;
        p.glassFrost = 8.0;
        p.glassRefraction = 0.22;
        p.glassDispersion = 0.12;
        p.glassHighlight = 0.7;
        p.islandTheme = QStringLiteral("brand");
        p.islandRadius = 22;
        list.append(p);
    }

    // ④ 极简 —— 接近无玻璃，中性灰强调。适合「只看数字、不要花活」的场景。
    {
        ThemePack p;
        p.name = QStringLiteral("极简");
        p.description = QStringLiteral("近乎无玻璃、中性灰：信息优先");
        p.dark = false;
        p.accentColor = QStringLiteral("#6B7280");
        p.cardOpacity = 1.0;
        p.uiTransparency = 1.0;
        p.glassTint = 0.12;
        p.glassFrost = 0.0;
        p.glassRefraction = 0.0;
        p.glassDispersion = 0.0;
        p.glassHighlight = 0.0;
        p.islandTheme = QStringLiteral("ink");
        p.islandRadius = 16;
        p.islandColumns = 8;
        p.consoleWidgets = false;   // 极简就不摆三块卡
        list.append(p);
    }

    // ⑤ 浓墨 —— 深色 + 低透明 + 高对比。夜间 / 弱光环境用。
    {
        ThemePack p;
        p.name = QStringLiteral("浓墨");
        p.description = QStringLiteral("深色高对比：背景几乎不透，文字更实");
        p.dark = true;
        p.accentColor = QStringLiteral("#7AB8F3");
        p.cardOpacity = 0.98;
        p.uiTransparency = 0.55;
        p.glassTint = 0.85;
        p.glassFrost = 40.0;
        p.glassRefraction = 0.5;
        p.glassDispersion = 0.3;
        p.glassHighlight = 0.85;
        p.islandTheme = QStringLiteral("ink");
        p.islandRadius = 20;
        p.islandDim = true;
        p.islandLiteMotion = true;   // 弱光环境下动效少一点更舒服
        list.append(p);
    }

    return list;
}

bool findBuiltinThemePack(const QString& name, ThemePack* out)
{
    for (const ThemePack& p : builtinThemePacks()) {
        if (p.name == name) {
            if (out) *out = p;
            return true;
        }
    }
    return false;
}

QString suggestedThemePackFileName(const QString& name)
{
    QString base = name.trimmed();
    // 文件名里不能有 \ / : * ? " < > |，也不能有路径分隔符
    base.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]+")), QStringLiteral("_"));
    if (base.isEmpty()) base = QStringLiteral("theme");
    return base + QString::fromLatin1(kThemePackExtension);
}

} // namespace kc::config
