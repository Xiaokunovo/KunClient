#pragma once

#include <QString>

namespace kc {

// 应用设置模型（JSON 持久化）。
//
// 设置文件必须放在**不被退出清理删除**的状态目录里（见 AppStatePaths 注释）：
// %LOCALAPPDATA%\KunClient 整个目录会在关闭工具时被删掉，
// 而 settings.json 里的 legalAcceptedVersion 一旦丢失，每次启动都会重新弹合规条款。
class AppSettings
{
public:
    // 单例：首次访问时从 AppStatePaths::FindExisting("settings.json") 加载并迁移。
    static AppSettings& current();

    QString backgroundImagePath;
    QString backgroundAnimation = QStringLiteral("KenBurns");
    double backgroundOpacity = 0.35;

    // 背景「真实模糊」强度（像素半径，0 = 不模糊）。玻璃面板的磨砂感来自它：
    // 壁纸先被这支效果模糊，再透过半透明面板露出来。视频 / GIF 背景逐帧变，
    // 逐帧重模糊是纯浪费，一律自动降级为不模糊；软件渲染同样跳过。
    double backgroundBlur = 24;

    // 背景显示方式：Fill = 等比放大裁剪铺满窗口（默认，无留白）；
    // Uniform = 等比完整显示（可能留空）；Stretch = 拉伸填满（可能变形）。
    // 默认 cover：留边（contain）在桌面窗口里几乎必然露底，看着像没铺满。
    QString backgroundFit = QStringLiteral("UniformToFill");

    // 卡片界面不透明度（1.0 = 不透明，越小越透出动态背景）。
    double cardOpacity = 0.86;

    // 界面整体透明度（全局，0 = 完全不透明，1 = 全透明完全透出背景）。
    // 默认 1.0（页面不再盖白底）：之前默认 0.38 会在所有页面上糊一层 62% 不透明的白，
    // 把动态背景整个挡住——用户反馈的「页面上层那层白色」就是它。
    double uiTransparency = 1.0;

    // 配置结构版本。< 2 表示这份配置产生在「页面默认盖一层白底」的年代，升级时撤掉那层白底。
    // 默认值必须是 1 而不是 2：老配置文件里根本没有这个字段，反序列化时初始值会生效——
    // 写成 2 的话老配置会被误判成「已升级」，迁移直接跳过。
    int settingsVersion = 1;

    // 上次选引导程序用过的目录，下次打开选择器直接落在这里。
    QString lastLoaderDir;

    // 侧栏显示形态："expanded"（默认：图标 + 文字）或 "collapsed"（仅图标）。
    QString sidebarMode = QStringLiteral("expanded");

    // ── 灵动岛（2026-10-18 用户口径「可玩性和个性化太少」）──────────────────
    //  这七项全部由外壳读出来喂给 DynamicIsland::setOptions()，改动即时生效。
    //  默认值 = 改造前的外观，所以升级上来的老用户看到的还是原来那枚岛。

    // 岛体配色：ink（纯黑，iOS 原味）/ accent（跟随主题强调色）/ glass（深色玻璃）。
    QString islandTheme = QStringLiteral("ink");
    // 展开面板圆角（px）。折叠态永远是胶囊（= 高度的一半），这一项只管展开态。
    int islandRadius = 26;
    // 折叠态那一行显示什么：auto（进度 > 连接 > 设备 > 就绪）/ device / page / clock。
    QString islandCollapsed = QStringLiteral("auto");
    // 显示方式：always（常驻）/ hover（鼠标移上去才浮现）/ hidden（完全隐藏）。
    QString islandVisibility = QStringLiteral("always");
    // 展开时是否把底下页面压暗（默认开，iOS 同款）。
    bool islandDim = true;
    // 展开面板列数：4（紧凑，行数变多）/ 6（标准）/ 8（宽，适合大屏）。
    int islandColumns = 6;
    // 动效精简：关掉卡片错峰淡入、展开时长从 340ms 收到 180ms（低配机 / 嫌晃眼）。
    bool islandLiteMotion = false;

    // ── 强调色（2026-10-24）────────────────────────────────────────────
    //  #RRGGBB。空串 = 用主题内置的 #0A84FF。
    //  只影响「强调色系列」（按钮 / 进度条 / 选中态 / 链接 / 焦点框），
    //  不碰语义色（成功绿 / 警告黄 / 危险红）—— 那三个换了会误导用户。
    QString accentColor;

    // ── 液态玻璃（iOS 26 材质，2026-10-23）──────────────────────────────
    //  这五项直接喂给 glass::Style（见 ui/widgets/LiquidGlass.h），由
    //  ui/theme/GlassSettings 负责与 Style 互转。
    //
    //  为什么要让用户调：不同壁纸下「好看」的玻璃参数差很多 —— 深色照片墙要
    //  高浓度低磨砂（否则糊成一片灰），浅色纯色墙要低浓度高折射（否则看不出
    //  材质）。出厂只给一档「标准」，等于把一半用户的观感牺牲掉了。
    //
    //  glassPreset 只是「一键改这五项」的快捷方式：crystal（清透）/ standard
    //  （标准）/ frosted（磨砂）/ minimal（极简）。用户拖过任意滑条后变成 custom。
    QString glassPreset = QStringLiteral("standard");
    /// 染色浓度 0~1：越小越透。
    double glassTint = 0.58;
    /// 磨砂半径（px）：0 = 完全清晰（只靠折射），48 = 糊成色块。
    double glassFrost = 22.0;
    /// 边缘折射强度 0~1：iOS 液态玻璃的灵魂，0 = 关掉退回普通磨砂。
    double glassRefraction = 0.55;
    /// 色散强度 0~1：边缘的彩虹边。
    double glassDispersion = 0.35;
    /// 高光 / 收边强度倍率 0~1.6。
    double glassHighlight = 1.0;

    // ── FDL 通道（原小米 EDL；流程里的 fastboot 命令，改完记住） ──
    // 这些都是**用户可编辑**的命令，不是写死的机型方案：各机型 bootloader 的命令支持度
    // 不一样，默认值只是占位，用户改过之后以他的为准。
    QString xiaomiEdlReadCommand = QStringLiteral("getvar token");

    // 已废弃，不再被读取。原来表示「blob 与 sig 的拼接顺序」，但实机确认 stage 的文件里
    // 只有 sig、不含 blob，也就没有顺序可言。键保留只是为了让老 settings.json 少一个
    // 「找不到的字段」，改成什么都无效。
    int xiaomiEdlPackOrder = 0;

    QString xiaomiEdlWriteCommand = QStringLiteral("stage");
    QString xiaomiEdlEdlCommand = QStringLiteral("oem edl");

    // 用户已同意的法务文本版本。小于当前版本就在启动时强制重新阅读并同意。
    // 默认 0 = 从未同意过（老配置文件没有该字段，反序列化后就是这个初始值）。
    int legalAcceptedVersion = 0;

    // ── 环境安全检测（2026-10-24 重做）────────────────────────────────
    //  默认**开启**：这是账号风控的唯一来源，关掉等于裸奔。
    //  但必须给用户一个开关 —— 界面上写清楚「检测什么、不检测什么」，
    //  并让结果可见（设置页会显示最近一次检测命中的项），否则就是偷偷摸摸。
    //  关掉后 EnvGuard 不再检测、不再上报，也不起复检定时器。
    bool envCheckEnabled = true;

    // ── 刷机引擎（2026-10-18，对齐 TMFTool 的「COM 速率 / 线程核心 / 兼容模式」）──
    //  MTK BROM 走串口，波特率是可调的：默认 115200，部分机型在 921600 下握手更快。
    //  ⚠️ 高通 EDL 走 USB bulk，qcusbser 忽略波特率 —— 这一项只对 MTK 生效。
    int engineMtkBaud = 115200;
    // 工作线程上限：0 = 自动（QThread::idealThreadCount，最少 2）。
    // 影响所有走 QThreadPool 的任务（校验、下载、OEM 工具），不影响刷写本身。
    int engineThreads = 0;
    // 兼容模式：牺牲速度换容错 —— EDL 每次 program 的分片从 64 MB 收到 8 MB
    // （每片一次 XML 往返 + ACK，劣质线材/老固件下更不容易断流），
    // MTK 每个传输包之间插 3 ms 间隔（老 BROM 处理不过来会丢包）。
    bool engineCompatMode = false;

    /// EDL 每次 program 命令的最大分片字节数（兼容模式 8 MB，正常 64 MB）。
    [[nodiscard]] quint64 edlSliceBytes() const;
    /// 生效的工作线程数（0 配置 → 自动）。
    [[nodiscard]] int effectiveThreads() const;
    /// MTK 每个传输包之间的间隔（兼容模式 3 ms，正常 0）。
    [[nodiscard]] int mtkPacketDelayMs() const;
    /// 把「线程核心」应用到全局线程池。启动时与设置变更时各调一次。
    void applyRuntime() const;

    // 写 JSON 到 AppStatePaths::Combine("settings.json")（缩进格式）。
    void save() const;

    // 老配置升级（见实现内的迁移注释）。加载时自动调用。
    void migrate();

    // 返回背景文件的真实路径，并在路径失效时尝试自愈（按名称相似度在同目录找改名后的文件）。
    QString resolveBackgroundPath();

    // 背景路径是否配置了、但实际文件已找不到（用于在设置界面给出明确提示）。
    bool backgroundMissing() const;

    // 基于最长公共子序列的名称相似度（0~1）。
    static double similarity(const QString& a, const QString& b);

private:
    AppSettings() = default;
    static AppSettings load();
};

} // namespace kc
