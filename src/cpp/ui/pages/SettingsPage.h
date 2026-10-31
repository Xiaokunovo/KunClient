#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  SettingsPage — 设置页
//
//  移植自 tests/src/KunClient.App/Pages/SettingsPage.{xaml,xaml.cs}。
//  C# 原页还有用户信息 / Wallpaper Engine / 法务弹窗等（云端背景已于 2026-10-21 整体下线），
//  这里保留 AppSettings 里真正可持久化的字段：
//   · 背景设置 —— 背景文件（QLineEdit + 浏览）、动画效果、显示方式、
//     背景不透明度 / 背景模糊滑杆；
//   · 界面设置 —— 卡片不透明度 / 界面透明滑杆、侧栏模式；
//   · 灵动岛 —— 配色 / 展开圆角 / 折叠态内容 / 显示方式 / 面板列数 / 展开压暗 /
//     动效精简（2026-10-18 加，用户口径「个性化太少」）。这一组是**即时生效**的，
//     与四条滑杆同样走「写内存 → 通知外壳重读 → 静默落盘」；
//   · 法务 —— legalAcceptedVersion 读数。
//  （服务器地址 / FDL 命令 / 主题切换 2026-10-13 起不在设置页暴露：前两者走配置文件
//   与内置默认值，主题切换随「服务器设置」卡一并移除，对齐 C# 设置页。）
//  所有控件改动先落在界面上，点「保存设置」统一写回 AppSettings 并 save()。
//  2026-10-17：「卡片不透明度 / 界面透明 / 背景不透明度 / 背景模糊」四条滑杆改成
//   **拖动即预览**（applyLive，不落盘），松手时静默落盘。原来的行为是「拖了没反应，
//   必须点保存」—— 用户看到的就是「透明度不能调节」。
//  onActivated() 从 AppSettings 重新回填（外部改过配置后切回页面所见即所得）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/pages/BasePage.h"

#include <QVector>

class QCheckBox;
class QColor;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace kc::config {
struct ThemePack;
}
class QSpinBox;
class QTimer;
class QVBoxLayout;

namespace kc::ui {
class IosSwitch;
class SegmentedControl;
}

namespace kc::ui {

class NotificationBar;
class GlassSurface;

class SettingsPage : public BasePage {
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);

    void onActivated() override;

private:
    void buildUi();
    QFrame* makeCard(const QString& title);
    void loadFromSettings();   // AppSettings → 控件（onActivated / 构造共用）
    void applyLive();          // 控件 → AppSettings（内存）+ 刷样式：拖动即时预览，不落盘
    /// 灵动岛七项 → AppSettings + 让外壳重读（岛是外壳的子控件，设置页改不动它）。
    void applyIslandLive();
    /// 套用一个主题包（2026-10-31）：写内存 + 回填控件 + 立即预览。
    /// 落盘仍走底部「保存设置」（与其它外观项一致）。
    void applyThemePack(const kc::config::ThemePack& pack);
    /// iOS 玩法六项 → AppSettings + 让外壳重读（悬浮球 / 小组件 / 待机都是外壳的
    /// 子控件，设置页同样改不动它们 —— 走窗口上的 Q_INVOKABLE applyIosFeatures）。
    void applyIosLive();
    /// 液态玻璃五项 → AppSettings（内存）+ 刷新预览面板。**不重放样式表**：
    /// 玻璃参数只影响自绘面板，重放 QSS 是纯浪费（一次 repolish 整棵树）。
    void applyGlassLive();
    /// AppSettings → 玻璃滑条 + 预览（loadFromSettings / 应用预设后调用）。
    void syncGlassFromSettings();
    /// 预览面板的背板：优先用用户自己的壁纸（缩略图），没有就用内置环境光。
    void loadGlassPreviewBackdrop();
    /// 应用一个强调色（无效色 = 恢复内置蓝）。立即生效 + 静默落盘。
    void applyAccentColor(const QColor& c);
    /// 把色点的选中态同步到当前强调色。
    void syncAccentChips();
    void persist();            // 控件 → AppSettings + save()，不弹通知
    void applyAndSave();       // persist() + 通知条反馈

    // 账号与会员：把 MembershipService 里的等级 / 到期 / 模块状态刷到界面上。
    // 卡密兑换（POST /api/v2/auth/redeem）：走线程池，成功后重刷会员卡。

    NotificationBar* m_notify = nullptr;

    // 滑条拖动预览的防抖定时器（一次重放样式表要 repolish 整棵树，不能每个 tick 都跑）
    QTimer* m_liveTimer = nullptr;
    /// 玻璃参数专用防抖：磨砂 + 折射是 5~10ms 级开销，逐像素重算会明显掉帧。
    /// 与 m_liveTimer 分开是因为它**不能**触发 refreshStyleSheet（玻璃只影响自绘面板）。
    QTimer* m_glassLiveTimer = nullptr;
    bool m_loading = false;    // loadFromSettings 期间不触发预览（否则回填也会刷一遍样式表）

    // 背景设置
    QLineEdit* m_bgPath = nullptr;
    QComboBox* m_bgAnimation = nullptr;
    QComboBox* m_bgFit = nullptr;
    QSlider* m_bgOpacity = nullptr;
    QLabel* m_bgOpacityValue = nullptr;
    QSlider* m_bgBlur = nullptr;
    QLabel* m_bgBlurValue = nullptr;

    // 界面设置
    QSlider* m_cardOpacity = nullptr;
    QLabel* m_cardOpacityValue = nullptr;
    QSlider* m_uiTransparency = nullptr;
    QLabel* m_uiTransparencyValue = nullptr;
    /// 强调色色点（8 个预设）。点一下立即生效，选中态靠 QSS :checked 描边。
    QVector<QPushButton*> m_accentChips;

    // 灵动岛（配色 / 圆角 / 折叠态内容 / 显示方式 / 列数 / 压暗 / 动效精简）
    SegmentedControl* m_islandTheme = nullptr;
    QSlider* m_islandRadius = nullptr;
    QLabel* m_islandRadiusValue = nullptr;
    QComboBox* m_islandCollapsed = nullptr;
    QComboBox* m_islandVisibility = nullptr;
    QComboBox* m_islandColumns = nullptr;
    IosSwitch* m_islandDim = nullptr;
    IosSwitch* m_islandLite = nullptr;

    // 液态玻璃（预设 + 五项参数 + 实时预览面板）
    GlassSurface* m_glassPreview = nullptr;
    QLabel* m_glassHint = nullptr;
    QComboBox* m_glassPreset = nullptr;
    QSlider* m_glassTint = nullptr;
    QLabel* m_glassTintValue = nullptr;
    QSlider* m_glassFrost = nullptr;
    QLabel* m_glassFrostValue = nullptr;
    QSlider* m_glassRefraction = nullptr;
    QLabel* m_glassRefractionValue = nullptr;
    QSlider* m_glassDispersion = nullptr;
    QLabel* m_glassDispersionValue = nullptr;
    QSlider* m_glassHighlight = nullptr;
    QLabel* m_glassHighlightValue = nullptr;
    /// 预设下拉的「回填中」保护：syncGlassFromSettings 里 setCurrentIndex 会发
    /// currentIndexChanged，不挡住就会把刚回填的参数又按预设覆盖一遍。
    bool m_glassSyncing = false;
    /// 预览背板当前取自哪张壁纸（路径）。切回设置页时路径没变就不重新解码 ——
    /// 4K 壁纸解码一次是百毫秒级，每次 onActivated 都做会让切页明显发顿。
    QString m_glassBackdropPath;

    // 法务
    QLabel* m_legalState = nullptr;

    // 安全与隐私 / 账号与会员：已随服务器功能移除（停更离线版 2.0.6）。

    // 刷机引擎（COM 速率 / 线程核心 / 兼容模式）
    QComboBox* m_engineBaud = nullptr;
    QSpinBox* m_engineThreads = nullptr;
    QCheckBox* m_engineCompat = nullptr;
    QCheckBox* m_preFlashBackup = nullptr;   // 刷机前自动备份（2026-10-31）

    // iOS 玩法六项（2026-10-29）
    IosSwitch* m_assistiveTouch = nullptr;
    QSpinBox* m_standbyIdle = nullptr;
    IosSwitch* m_focusFlashMode = nullptr;
    IosSwitch* m_consoleWidgets = nullptr;
    IosSwitch* m_shortcutsPage = nullptr;

    /// 拉取已经结束（成功或失败）。区分「正在拉」与「拉过但没拿到」，
    /// 否则离线时那句「正在获取模块信息…」会永远挂在那儿。
    bool m_modulesDone = false;
};

} // namespace kc::ui
