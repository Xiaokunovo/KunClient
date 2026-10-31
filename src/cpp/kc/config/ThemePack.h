#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ThemePack —— 「整套外观」的数据模型（主题包）
//
//  为什么需要它：现在的外观设置散在四张卡里 —— 界面设置（强调色 / 卡片不透明度 /
//  界面透明）、液态玻璃（五项）、灵动岛（七项）、背景。想调出一套顺眼的观感要
//  来回拖十几个滑条，而且**没法分享、没法备份、没法一键回到上次那套**。
//
//  主题包 = 把这一整套值打包成一个文件（.kcskin），可以：
//    · 用内置预设一键切换（5 套风格，不只是换个强调色）
//    · 导出给朋友 / 存起来
//    · 导入别人的包
//
//  🔴 设计上的两个硬约束：
//    ① **只包含「外观」**，不含设备、路径、刷机参数 —— 主题包是拿来分享的，
//       混进个人配置（比如上次选的固件目录）会有隐私和误用风险。
//    ② 带 **schema 版本号**：将来加字段时，旧文件仍能读（缺字段走默认值），
//       新文件在旧版本上会给出明确提示而不是崩掉或静默丢配置。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QVector>

namespace kc::config {

/// 主题包当前 schema 版本。加字段时 +1，并在 fromJson 里处理旧版本。
constexpr int kThemePackSchema = 1;

/// 一套外观。
struct ThemePack {
    // ── 元信息 ────────────────────────────────────────────────────────────
    QString name;          ///< 显示名（必填）
    QString description;   ///< 一句话说明（给预设列表用，可为空）
    QString author;        ///< 作者（可为空）
    int schema = kThemePackSchema;

    // ── 基础外观 ──────────────────────────────────────────────────────────
    bool dark = false;              ///< 深色 / 浅色
    QString accentColor;            ///< #rrggbb；空 = 用主题内置的 #0A84FF
    double cardOpacity = 0.86;      ///< 卡片不透明度 0~1
    double uiTransparency = 1.0;    ///< 界面透明度 0~1

    // ── 液态玻璃（五项）──────────────────────────────────────────────────
    double glassTint = 0.58;        ///< 染色浓度 0~1
    double glassFrost = 22.0;       ///< 磨砂半径 0~60
    double glassRefraction = 0.55;  ///< 边缘折射 0~1
    double glassDispersion = 0.35;  ///< 色散 0~1
    double glassHighlight = 1.0;    ///< 顶沿高光 0~1

    // ── 灵动岛（七项里影响观感的五项）────────────────────────────────────
    QString islandTheme = QStringLiteral("ink");   ///< ink / brand / glass
    int islandRadius = 26;                          ///< 10~40
    int islandColumns = 6;                          ///< 4~8
    bool islandDim = true;                          ///< 展开时压暗背景
    bool islandLiteMotion = false;                  ///< 动效精简

    // ── 主屏 ──────────────────────────────────────────────────────────────
    bool consoleWidgets = true;     ///< 主屏顶部三块玻璃卡
};

/// 校验。返回 false 时 error 写明**具体**哪个字段不对（不要只说「无效」）。
bool validateThemePack(const ThemePack& pack, QString* error);

/// 序列化 / 反序列化（JSON）。
/// fromJson 对缺字段走默认值（向前兼容），对**超范围**的值返回 false。
bool themePackToJson(const ThemePack& pack, QByteArray* out, QString* error);
bool themePackFromJson(const QByteArray& json, ThemePack* out, QString* error);

/// 从 AppSettings 抓当前外观 → ThemePack（name 留空，由调用方填）。
ThemePack captureCurrentThemePack();

/// 把 ThemePack 写进 AppSettings 的**内存字段**（不 save、不重放 QSS —— 由调用方决定）。
void applyThemePackToSettings(const ThemePack& pack);

/// 内置预设。至少 5 套，风格要真的不同（不是只换强调色）。
QVector<ThemePack> builtinThemePacks();

/// 按名字找内置预设；找不到返回 false。
bool findBuiltinThemePack(const QString& name, ThemePack* out);

/// 主题包文件扩展名（含点）。
constexpr const char* kThemePackExtension = ".kcskin";

/// 建议的文件名（不含路径）：<名称>.kcskin，名称里的非法字符会被清洗。
QString suggestedThemePackFileName(const QString& name);

} // namespace kc::config
