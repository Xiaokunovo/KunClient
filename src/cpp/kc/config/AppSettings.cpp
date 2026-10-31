#include "kc/config/AppSettings.h"

#include "kc/config/AppStatePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QThread>
#include <QThreadPool>
#include <QVector>

namespace kc {
namespace {

QString settingsFileName()
{
    // 🔴 别用「字符数组 + 手数长度」拼字面量：这里曾把 13 写成 12，得到
    // "settings.jso"——C++ 版一直在读写一个独立的截断名文件，真正的
    // settings.json（.NET 版写）从未被读到，法务闸门/用户设置全部失联。
    return QStringLiteral("settings.json");
}

// 同目录内寻找被改名的背景文件：同扩展名 + 名称相似度 ≥ 0.6 且候选唯一。
// 阈值取 0.6 足以覆盖单字改动（尘→雨：0.92），又不会把无关文件误判为同一资源。
QString tryRepairBackgroundPath(const QString& savedPath)
{
    const QFileInfo info(savedPath);
    const QString dir = info.absolutePath();
    const QString ext = info.suffix();
    if (dir.isEmpty() || !QDir(dir).exists() || ext.isEmpty()) return {};

    const QString stem = info.completeBaseName();
    QString best;
    double bestScore = 0.0;
    bool ambiguous = false;

    const QFileInfoList files =
        QDir(dir).entryInfoList({ QStringLiteral("*.") + ext }, QDir::Files, QDir::Name);
    for (const QFileInfo& file : files) {
        const double score = AppSettings::similarity(stem, file.completeBaseName());
        if (score > bestScore + 0.0001) {
            bestScore = score;
            best = file.absoluteFilePath();
            ambiguous = false;
        } else if (std::abs(score - bestScore) <= 0.0001) {
            ambiguous = true;
        }
    }

    return (best.isEmpty() || ambiguous || bestScore < 0.6) ? QString() : best;
}

// 双拼写键读取：.NET 版 settings.json 用 PascalCase（"LegalAcceptedVersion"），
// C++ 版写 camelCase（"legalAcceptedVersion"）。老用户升级时两套都得认——
// 先读 camel（本版格式），miss 再读 Pascal（.NET 遗留），设置与法务同意才能带过来。
class KeyReader
{
public:
    explicit KeyReader(const QJsonObject& o) : m_o(o) {}

    // 依序尝试各个拼写，返回首个命中的值；全 miss 返回 Undefined。
    QJsonValue get(std::initializer_list<const char*> keys) const
    {
        for (const char* k : keys) {
            const QJsonValue v = m_o.value(QLatin1String(k));
            if (!v.isUndefined()) return v;
        }
        return {};
    }

    bool isDouble(std::initializer_list<const char*> keys) const
    {
        return get(keys).isDouble();
    }

private:
    const QJsonObject& m_o;
};

} // namespace

AppSettings& AppSettings::current()
{
    static AppSettings instance = load();
    return instance;
}

AppSettings AppSettings::load()
{
    AppSettings s;
    // 新位置 → 旧 KunClient 目录 → 更早的 KunFlashTool 目录：老用户升级后设置不丢。
    // （历史上还产生过截断名 settings.jso——文件名手数错长度的产物，排在最后兜底。）
    QString path = AppStatePaths::FindExisting(settingsFileName());
    if (path.isEmpty()) {
        const QString legacyJso = AppStatePaths::Combine(QStringLiteral("settings.jso"));
        if (QFile::exists(legacyJso)) path = legacyJso;
    }
    if (!path.isEmpty() && QFile::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
            if (doc.isObject()) {
                const QJsonObject o = doc.object();
                const KeyReader r(o);
                auto str = [&](std::initializer_list<const char*> ks, QString& dst) {
                    const QJsonValue v = r.get(ks);
                    if (v.isString()) dst = v.toString();
                };
                auto dbl = [&](std::initializer_list<const char*> ks, double& dst) {
                    const QJsonValue v = r.get(ks);
                    if (v.isDouble()) dst = v.toDouble();
                };
                auto num = [&](std::initializer_list<const char*> ks, int& dst) {
                    const QJsonValue v = r.get(ks);
                    if (v.isDouble()) dst = v.toInt();
                };

                str({"backgroundImagePath", "BackgroundImagePath"}, s.backgroundImagePath);
                str({"backgroundAnimation", "BackgroundAnimation"}, s.backgroundAnimation);
                dbl({"backgroundOpacity", "BackgroundOpacity"}, s.backgroundOpacity);
                dbl({"backgroundBlur", "BackgroundBlur"}, s.backgroundBlur);
                str({"backgroundFit", "BackgroundFit"}, s.backgroundFit);
                dbl({"cardOpacity", "CardOpacity"}, s.cardOpacity);
                dbl({"uiTransparency", "UiTransparency"}, s.uiTransparency);
                num({"settingsVersion", "SettingsVersion"}, s.settingsVersion);
                str({"lastLoaderDir", "LastLoaderDir"}, s.lastLoaderDir);
                str({"sidebarMode", "SidebarMode"}, s.sidebarMode);
                str({"islandTheme", "IslandTheme"}, s.islandTheme);
                num({"islandRadius", "IslandRadius"}, s.islandRadius);
                str({"islandCollapsed", "IslandCollapsed"}, s.islandCollapsed);
                str({"islandVisibility", "IslandVisibility"}, s.islandVisibility);
                {
                    const QJsonValue v = r.get({"islandDim", "IslandDim"});
                    if (v.isBool()) s.islandDim = v.toBool();
                }
                num({"islandColumns", "IslandColumns"}, s.islandColumns);
                {
                    const QJsonValue v = r.get({"islandLiteMotion", "IslandLiteMotion"});
                    if (v.isBool()) s.islandLiteMotion = v.toBool();
                }
                str({"accentColor", "AccentColor"}, s.accentColor);
                str({"glassPreset", "GlassPreset"}, s.glassPreset);
                dbl({"glassTint", "GlassTint"}, s.glassTint);
                dbl({"glassFrost", "GlassFrost"}, s.glassFrost);
                dbl({"glassRefraction", "GlassRefraction"}, s.glassRefraction);
                dbl({"glassDispersion", "GlassDispersion"}, s.glassDispersion);
                dbl({"glassHighlight", "GlassHighlight"}, s.glassHighlight);
                str({"xiaomiEdlReadCommand", "XiaomiEdlReadCommand"}, s.xiaomiEdlReadCommand);
                num({"xiaomiEdlPackOrder", "XiaomiEdlPackOrder"}, s.xiaomiEdlPackOrder);
                str({"xiaomiEdlWriteCommand", "XiaomiEdlWriteCommand"}, s.xiaomiEdlWriteCommand);
                str({"xiaomiEdlEdlCommand", "XiaomiEdlEdlCommand"}, s.xiaomiEdlEdlCommand);
                num({"legalAcceptedVersion", "LegalAcceptedVersion"}, s.legalAcceptedVersion);
                {
                    const QJsonValue v = r.get({"envCheckEnabled", "EnvCheckEnabled"});
                    if (v.isBool()) s.envCheckEnabled = v.toBool();
                }
                num({"engineMtkBaud", "EngineMtkBaud"}, s.engineMtkBaud);
                num({"engineThreads", "EngineThreads"}, s.engineThreads);
                {
                    const QJsonValue v = r.get({"engineCompatMode", "EngineCompatMode"});
                    if (v.isBool()) s.engineCompatMode = v.toBool();
                }
                // iOS 玩法六项（2026-10-29）：缺字段一律保留头文件里的默认值，
                // 所以老 settings.json 读进来不会崩，只是这几项是新的默认。
                {
                    const QJsonValue v = r.get({"assistiveTouch", "AssistiveTouch"});
                    if (v.isBool()) s.assistiveTouch = v.toBool();
                }
                num({"standbyIdleSeconds", "StandbyIdleSeconds"}, s.standbyIdleSeconds);
                {
                    const QJsonValue v = r.get({"focusFlashMode", "FocusFlashMode"});
                    if (v.isBool()) s.focusFlashMode = v.toBool();
                }
                {
                    const QJsonValue v = r.get({"consoleWidgets", "ConsoleWidgets"});
                    if (v.isBool()) s.consoleWidgets = v.toBool();
                }
                {
                    const QJsonValue v = r.get({"preFlashBackup", "PreFlashBackup"});
                    if (v.isBool()) s.preFlashBackup = v.toBool();
                }
                {
                    const QJsonValue v = r.get({"shortcutsPageVisible", "ShortcutsPageVisible"});
                    if (v.isBool()) s.shortcutsPageVisible = v.toBool();
                }
            }
        }
    }
    s.migrate();
    return s;
}

void AppSettings::migrate()
{
    // 老配置升级：v1 的界面透明度默认（也常被手动调到）会在页面上盖一层白底，
    // 把动态背景糊住。用户要的就是撤掉这层，所以升级到 v2 时统一设为 1.0（全透出）。
    // 之后用户在设置里再怎么调都以他的值为准，不会被改回去。
    if (settingsVersion < 2) {
        uiTransparency = 1.0;
        settingsVersion = 2;
    }

    if (settingsVersion < 3) {
        // v3：背景模糊强度默认值从 0 改成 24（玻璃面板要靠它才有磨砂底）。
        // 这个字段一直没有 UI，老配置里的 0 只是字段默认值的残留，没人是「故意调成 0」的
        // —— 所以 <=0 一律升到新默认；之后用户若调回 0，以他的值为准。
        if (backgroundBlur <= 0) backgroundBlur = 24;
        settingsVersion = 3;
    }
}

void AppSettings::save() const
{
    QDir().mkpath(AppStatePaths::Root());

    QJsonObject o;
    o["backgroundImagePath"] = backgroundImagePath;
    o["backgroundAnimation"] = backgroundAnimation;
    o["backgroundOpacity"] = backgroundOpacity;
    o["backgroundBlur"] = backgroundBlur;
    o["backgroundFit"] = backgroundFit;
    o["cardOpacity"] = cardOpacity;
    o["uiTransparency"] = uiTransparency;
    o["settingsVersion"] = settingsVersion;
    o["lastLoaderDir"] = lastLoaderDir;
    o["sidebarMode"] = sidebarMode;
    o["islandTheme"] = islandTheme;
    o["islandRadius"] = islandRadius;
    o["islandCollapsed"] = islandCollapsed;
    o["islandVisibility"] = islandVisibility;
    o["islandDim"] = islandDim;
    o["islandColumns"] = islandColumns;
    o["islandLiteMotion"] = islandLiteMotion;
    o["accentColor"] = accentColor;
    o["glassPreset"] = glassPreset;
    o["glassTint"] = glassTint;
    o["glassFrost"] = glassFrost;
    o["glassRefraction"] = glassRefraction;
    o["glassDispersion"] = glassDispersion;
    o["glassHighlight"] = glassHighlight;
    o["xiaomiEdlReadCommand"] = xiaomiEdlReadCommand;
    o["xiaomiEdlPackOrder"] = xiaomiEdlPackOrder;
    o["xiaomiEdlWriteCommand"] = xiaomiEdlWriteCommand;
    o["xiaomiEdlEdlCommand"] = xiaomiEdlEdlCommand;
    o["legalAcceptedVersion"] = legalAcceptedVersion;
    o["envCheckEnabled"] = envCheckEnabled;
    o["engineMtkBaud"] = engineMtkBaud;
    o["engineThreads"] = engineThreads;
    o["engineCompatMode"] = engineCompatMode;
    // iOS 玩法六项（2026-10-29）
    o["assistiveTouch"] = assistiveTouch;
    o["standbyIdleSeconds"] = standbyIdleSeconds;
    o["focusFlashMode"] = focusFlashMode;
    o["consoleWidgets"] = consoleWidgets;
    o["preFlashBackup"] = preFlashBackup;
    o["shortcutsPageVisible"] = shortcutsPageVisible;

    QFile f(AppStatePaths::Combine(settingsFileName()));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

QString AppSettings::resolveBackgroundPath()
{
    const QString saved = backgroundImagePath;
    if (saved.trimmed().isEmpty()) return {};

    // 已经是一张能直接用的素材就照用。
    if (QFileInfo::exists(saved) && QFileInfo(saved).isFile()) return saved;

    // 存的是**目录**（用户手选了整张壁纸文件夹）：File 口径下会直接判定「没有自定义背景」
    // 并静默关掉背景。这里先不做 WE 工程解析（依赖 WallpaperEngineService），
    // 落到下面的「改名自愈」。
    const QString repaired = tryRepairBackgroundPath(saved);
    if (!repaired.isEmpty()) {
        const_cast<AppSettings*>(this)->backgroundImagePath = repaired;
        const_cast<AppSettings*>(this)->save();
    }
    return repaired;
}

bool AppSettings::backgroundMissing() const
{
    if (backgroundImagePath.trimmed().isEmpty()) return false;
    return const_cast<AppSettings*>(this)->resolveBackgroundPath().isEmpty();
}

double AppSettings::similarity(const QString& a, const QString& b)
{
    const int n = a.size();
    const int m = b.size();
    if (n == 0 || m == 0) return 0;

    // 最长公共子序列 DP，滚动一维数组省内存。
    QVector<int> prev(m + 1, 0);
    QVector<int> cur(m + 1, 0);
    for (int i = 1; i <= n; ++i) {
        for (int j = 1; j <= m; ++j) {
            cur[j] = (a.at(i - 1) == b.at(j - 1)) ? prev[j - 1] + 1
                                                  : std::max(prev[j], cur[j - 1]);
        }
        std::swap(prev, cur);
        std::fill(cur.begin(), cur.end(), 0);
    }
    return 2.0 * prev[m] / (n + m);
}

// ── 刷机引擎（对齐 TMFTool 的 COM 速率 / 线程核心 / 兼容模式）────────────────

quint64 AppSettings::edlSliceBytes() const
{
    // 64 MB 是正常口径：一条 program 命令能装下就少一次 XML 往返 + ACK。
    // 兼容模式收到 8 MB —— 每次传输窗口更短，劣质线材 / 老 programmer 不容易断流。
    return engineCompatMode ? (8ULL * 1024ULL * 1024ULL) : (64ULL * 1024ULL * 1024ULL);
}

int AppSettings::effectiveThreads() const
{
    if (engineThreads > 0) return engineThreads;
    return qMax(2, QThread::idealThreadCount());
}

int AppSettings::mtkPacketDelayMs() const
{
    return engineCompatMode ? 3 : 0;
}

void AppSettings::applyRuntime() const
{
    // 🔴 只设上限，不设下限：QThreadPool 自己会按需起线程，把 maxThreadCount
    //    调到 1 会真的把并发压成串行（用户明确选了 1 核就该这样）。
    QThreadPool::globalInstance()->setMaxThreadCount(effectiveThreads());
}

} // namespace kc
