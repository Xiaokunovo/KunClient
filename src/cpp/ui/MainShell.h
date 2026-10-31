#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  MainShell — KunClient 主外壳窗口
//
//  导航结构 = **iOS 启动器式控制台**（2026-10-17 用户拍板「整个布局重写，模仿
//  iOS 启动工具的控制台」后的重构，取代此前的「左导航轨 + 顶栏页签条」两级模型）：
//
//    · **控制台主屏**（ConsolePage，页面栈索引 0）：全幅壁纸 + 应用图标网格
//      （5 列 × 3 行）+ 底部 Dock，就是 iOS 主屏那套；
//    · **灵动岛**（DynamicIsland）：顶栏正中悬浮的黑色胶囊，折叠态一句话状态，
//      点开成面板列全部读数（设备 / 模式 / 任务 / 协议 / 状态 + 进度）；
//    · **页面**：点图标进页；顶栏左侧出现「‹ 控制台」返回按钮，一键回主屏；
//      页内快捷入口（如 FASTBOOT 的「可视刷写 ›」）仍走 navigateTo 直达；
//    · **底部状态栏**：读数分段 + 双进度条 + 跑马灯（2026-10-16 从内嵌 EDL 窗
//      页脚并入），控制台与页面下都常显 —— 它是系统级 UI，不是页面附属。
//
//  被撤掉的东西（不要往回加）：左导航轨（含收起/展开、弹簧指示器、分节头）、
//  顶栏页签条、导航项 NavButton（矢量图标已抽到 ui/widgets/AppIcons，供主屏复用）。
//
//  页面清单不变：刷机 6 + 设备 5 + 高级 1 + 云端 1 + 设置 2（另有「可视刷写」
//  隐藏页，只走 navigateTo）。
//  🔴 EDL 刷写页内嵌 swt-edl 旧窗。2026-10-14 曾定红线「布局不许动」，只在外壳层
//     包壳；2026-10-16 用户放开放弃该红线，要求 EDL 界面一并改造，QSS 的
//     #edlLegacyPage 保护区段已删除。FDL 页（原「小米EDL」）同批撤掉独立页签，
//     改由 EDL 右下角重启列表触发完整流程。
// ─────────────────────────────────────────────────────────────────────────────

#include <QHash>
#include <QImage>
#include <QMainWindow>
#include <QVector>

class QFrame;
class QHBoxLayout;
class QLabel;
class QPaintEvent;
class QPainter;
class QPushButton;
class QResizeEvent;
class QShowEvent;
class QStackedWidget;
class QTimer;
class QVBoxLayout;

namespace kc::media {
class VideoBackground;
}

namespace kc::ui {

class BasePage;
class ConsolePage;
class ControlCenterPage;
class DynamicIsland;
class IslandScrim;
class MarqueeLabel;
class PageTransition;
class ToolProgressBar;
class AssistiveTouch;
class StandByOverlay;
class ShortcutsPage;
class BatchFlashPage;

class MainShell : public QMainWindow {
    Q_OBJECT

public:
    explicit MainShell(QWidget* parent = nullptr);
    ~MainShell() override;

    /// 跳转到指定页面（key 见 registerPages；探针 --page 用）。
    /// 特例：key == "console" 时回控制台主屏。
    void navigateTo(const QString& key);

    /// 兼容 shim（探针 --tab 用）：tab 为 "all" 时回控制台，否则落到该分组首个可见页。
    /// 页签条本身已在控制台重构中撤掉，这里只为不破坏既有命令行调用而保留。
    Q_INVOKABLE void setActiveTab(const QString& tab, bool navigate = true);

    /// 回控制台主屏（页面顶栏「‹ 控制台」按钮 / 探针 --console 用）。
    Q_INVOKABLE void showConsole();

    /// 重载动态背景（设置页保存后经 invokeMethod 调用；对齐 C# ApplyDynamicBackground）。
    Q_INVOKABLE void reloadBackground();

    /// 拉开控制中心（= 切到左滑那一页）。探针 --controlcenter 也走这里。
    Q_INVOKABLE void openControlCenter();

    /// 重新读 AppSettings 里的灵动岛配置并应用到岛（设置页保存后经 invokeMethod 调用）。
    Q_INVOKABLE void applyIslandSettings();

    /// 重新读 AppSettings 里的「iOS 玩法」六项并应用（设置页保存后经 invokeMethod
    /// 调用）：辅助触控悬浮球显隐 / 主屏小组件 / 快捷指令入口 /
    /// 专注模式 / 待机自动进入秒数。
    Q_INVOKABLE void applyIosFeatures();

    /// 待机模式（StandBy）：整屏大时钟 + 设备读数，点一下任意处回来。
    /// 探针 --standby 也走这里。
    Q_INVOKABLE void enterStandby();
    Q_INVOKABLE void leaveStandby();


    /// 专注模式「刷机模式」开关（只显示刷机相关页面）。
    Q_INVOKABLE void setFocusFlashMode(bool on);
    bool focusFlashMode() const { return m_focusFlash; }

    /// 探针（--swipe-preview <0~1>）：把「主屏 → 控制中心」的横滑**定格**在
    /// 指定进度，供截图取证。跟手转场最容易出的问题是「手指到 30% 画面却
    /// 已经 80%」，无头环境里没手指，只能靠定格帧看几何对不对。
    Q_INVOKABLE void previewSwipe(qreal progress);

    /// 全局单例（液态玻璃菜单 / 抽屉 / 弹窗要找主壳要背板）。
    static MainShell* instance() { return s_instance; }

    /// 取「某个**顶层**玻璃控件背后那块真实画面」（壁纸 + 环境色晕），
    /// 喂给 glass::paint 当背板。控件必须传自己的全局几何（top-level 的
    /// geometry() 就是屏幕坐标）。非顶层控件请用 composeBackdrop(QRect) 自己算。
    QImage backdropForWidget(QWidget* w) const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
#ifdef Q_OS_WIN
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
#endif

private:
    struct PageEntry {
        QString key;
        QString title;
        QString group;              // 分组 tag：flash / device / advanced / cloud /
                                    // settings（主屏分区与转场模式共用）
        BasePage* page = nullptr;
        bool hidden = false;        // 不进主屏图标网格（如「可视刷写」），仍可 navigateTo
    };

    void buildUi();
    QWidget* buildTopBar();
    QWidget* buildStatusBar();
    void registerPages();
    void addPage(const QString& key, const QString& title, const QString& group,
                 BasePage* page, bool hidden = false);

    /// 切到页面栈里的某个页面（pageIndex 是 m_pages 下标，不是栈下标）。
    void showPage(int pageIndex);

    /// 取控制台主屏快照（缓存；调用时主屏必须正是当前页）。
    QPixmap consoleSnapshot();
    /// 取**页面栈渲染出的某一页**快照（含页面态的玻璃底）。
    /// 🔴 不能直接 `page->grab()`：QStackedLayout 是 StackOne 模式，只给**当前页**
    ///    摆几何，隐藏页停在 640×480，抓出来是一张空白；而且页面自己是透明的，
    ///    玻璃底画在 QStackedWidget 上（QSS 的 [console] 分支）—— 两件事都要临时
    ///    摆正才能抓对。grab 是同步渲染，期间不进事件循环，所以不会闪。
    QPixmap grabPageInStack(int pageIndex);
    /// 让控制台快照失效（尺寸 / 主题 / 入口变化时调）。
    void invalidateConsoleShot() { m_consoleShotValid = false; }
    /// 主屏图标网格 / Dock 与页面表对齐（registerPages 之后调用一次）。
    void updateConsoleEntries();

    /// 渲染「页面栈那一块」的**壁纸**（不含任何子控件），供转场叠层当衬底。
    /// 🔴 转场叠层盖在页面栈上，衬底若只铺纯色，切页的 400ms 里壁纸 / 视频
    ///    背景会整块消失再跳回来（2026-10-17 用户反馈「背景也会消失」）。
    /// 成本 = 一次全屏 blit + 一次壁纸绘制，每次转场只调一次，可接受。
    QPixmap stackBackdrop();

    void applyTheme(bool dark);
    /// 窗底绘制：环境色晕（常驻）+ 壁纸（按设置）。对齐 C# DynamicBackground。
    void drawAmbientBloom(QPainter& painter, const QRect& rect) const;
    /// 按窗口尺寸与 backgroundFit 重建 m_bgFrame。
    void rebuildBgFrame();
    /// 把「底色 + 四团环境色晕」烘成一张缓存图（跟 KenBurns 相位无关，每帧重算
    /// 四层覆盖整窗的径向渐变是纯浪费）。窗口尺寸 / 主题变化时重建。
    void rebuildBgBase();
    void onPageStatus(const QString& device, const QString& mode, const QString& hint);
    /// 合成「窗口里这块矩形背后的真实画面」（底色晕 → 壁纸 / 视频帧）。
    /// 这是所有液态玻璃面板的背板来源：Dock、顶栏 NavPill、灵动岛。
    ///
    /// 🔴 **绝不能 grab 窗口**：快照里含玻璃自己和所有控件，磨砂之后是一块灰糊
    ///    —— 假玻璃最典型的翻车方式。只能按 paintEvent 第 1~3 步重新合成。
    /// 🔴 与 paintEvent 必须同口径：漏掉任何一层（底色晕 / 壁纸 / 视频帧），
    ///    玻璃就会比周围亮或暗一档，看起来像贴了张纸。
    QImage composeBackdrop(const QRect& logical) const;
    /// 把「Dock 背后那块壁纸」裁出来喂给主屏 Dock 的液态玻璃。
    /// 🔴 必须延迟到事件循环再算：布局刚摆完时 Dock 的几何还是旧值。
    void scheduleGlassBackdrops();
    void updateGlassBackdrops();    /// ShellStatus 中枢 → 底部双进度条 + 分段状态栏 + 灵动岛。
    void syncShellStatus();
    /// 刷新底部跑马灯带：有「个性签名」滚签名，没有就轮播人生哲理文案。
    void updateMarquee();

    /// 顶栏：返回按钮可见性 + 当前页标题（控制台态显示品牌名）。
    void updatePageHeader(int pageIndex);
    /// 页名交叉淡化：旧名（m_pageTitlePrev）淡出、新名淡入。text 为空 = 收掉页名。
    void crossFadePageTitle(const QString& text);
    /// 顶栏品牌 meta：署名 + 版本 +（有则）会员签名档。
    void updateBrandMeta();
    /// 灵动岛贴顶居中（窗口尺寸变化时调用）。
    void repositionIsland();

    // ── 彩蛋：全局 Konami 码（↑↑↓↓←→←→BA）───────────────────────────────
    /// 累积一个按键到 Konami 序列；命中返回 true（序列随之清零）。
    bool feedKonamiKey(int key);
    /// 重置 Konami 累积进度（焦点切到输入框 / 按了无关键时调）。
    void resetKonamiProgress();
    /// 命中后的演出（2026-10-31 起为空实现：Toast 已移除）。
    void triggerKonamiEgg();

    // ── 控制中心（左滑那一页）──────────────────────────────────────────────
    /// 页里点了某个动作（theme / glass / blur / anim / autoport / debuglog /
    /// update / reset / card:<v> / bg:<v> / nav:<key>）。
    void applyControlAction(const QString& action);
    /// 灵动岛展开态的页面卡片 → 跳页（key == "console" 回主屏）。
    void onIslandPageRequested(const QString& key);
    /// 灵动岛快捷动作条：screenshot / console / control / settings。
    void onIslandAction(const QString& id);
    /// 按岛内搜索框的当前文字重建卡片表（空 = 不过滤）。registerPages 之后也走这里。
    void rebuildIslandPages();
    /// 折叠态左右拖动：按可见页序列前后各走一步（到头绕回）。
    void stepIslandPage(int delta);

    // ── iOS 玩法（2026-10-29）──────────────────────────────────────────────
    void ensureAssistiveTouch();
    void ensureStandbyOverlay();
    /// 悬浮球菜单 / 控制中心 / 待机层的统一动作分派：
    /// console / control / shortcuts / history / standby / focus / screenshot。
    void handleQuickAction(const QString& id);
    /// 待机计时：用户有操作就重置（m_standbyTimer 单发）。
    void noteUserActivity();
    /// 专注模式：刷新主屏图标表的可见性（只留刷机 + 设备页）。
    void refreshFocusFilter();

    // ── 横滑手势（主屏 ⇄ 控制中心，跟手）──────────────────────────────────
    /// 按下时判定这一笔有没有可滑的目标；-1 = 不接（当前页没有左右邻居）。
    int swipeTargetForCurrentPage() const;
    /// 越过死区后真正开始跟手：抓两页快照 + 交给叠层。
    void beginSwipe(bool forward);
    /// 松手：按位移 / 甩速决定落定还是回弹。
    void endSwipe();

    // ── 账号轮询 / 启动更新检查已随服务器功能移除（停更离线版）────────────────

protected:
    /// 主窗口入场：淡入 + 上浮（与弹窗同一族曲线）。只播一次。
    void showEvent(QShowEvent* event) override;
    /// 最小化 / 恢复：停掉或重开视频壁纸解码（后台不该烧 CPU）。
    void changeEvent(QEvent* event) override;

    QStackedWidget* m_stack = nullptr;
    /// 页面栈索引 0 常驻控制台主屏；各功能页从索引 1 开始。
    static constexpr int kConsoleStackIndex = 0;

    // ── 顶栏 ─────────────────────────────────────────────────────────────
    QFrame* m_topBar = nullptr;           // 顶栏本体（eventFilter 拖动判定用；缓存避免 findChild）
    QLabel* m_pageTitleLabel = nullptr;   // 页面态 = 页标题（控制台态隐藏）
    /// 页名交叉淡化的「旧名层」：与 m_pageTitleLabel 叠在同一个网格格子里，
    /// 切换时旧名压下面淡出、新名淡入。平时隐藏（隐藏项不参与布局尺寸）。
    QLabel* m_pageTitlePrev = nullptr;
    QWidget* m_titleHost = nullptr;       // 两层页名的宿主（QGridLayout 叠放）
    QPushButton* m_backButton = nullptr;  // 圆形 chevron 返回钮，仅页面态可见
    QWidget* m_brandGroup = nullptr;      // 控制台态：logo + KunClient + 版本（NavPill）
    QWidget* m_rightPill = nullptr;       // 右侧整组：署名 + 主题钮 │ 窗口钮（NavPill）
    QLabel* m_brandMeta = nullptr;        // 署名 + 签名档（顶栏右侧留白处）
    QPushButton* m_themeToggle = nullptr; // 顶栏主题切换（矢量绘制）

    // ── 灵动岛 ───────────────────────────────────────────────────────────
    DynamicIsland* m_island = nullptr;
    /// 灵动岛展开时压在页面栈上的背景压暗层（见 IslandScrim 注释）。
    IslandScrim* m_islandScrim = nullptr;
    /// 展开时是否压暗背景（AppSettings::islandDim 的缓存：expansionChanged 每帧
    /// 都要读，不每帧去翻设置对象）。
    bool m_islandDim = true;

    // ── 控制中心 ─────────────────────────────────────────────────────────
    /// 控制中心页（页面栈里的隐藏页，不出现在主屏图标网格与切换器里）。
    ControlCenterPage* m_controlPage = nullptr;
    /// 控制中心在 m_pages 里的下标（-1 = 还没注册）。
    int m_controlPageIndex = -1;

    // ── 横滑手势（主屏 ⇄ 控制中心）────────────────────────────────────────
    /// 已按下、还没越过死区。越界即作废（竖向滚动 / 横向拖到一半放弃）。
    bool m_swipeArmed = false;
    /// 已在跟手拖动（叠层此刻正在被 drive）。
    bool m_swipeActive = false;
    QPoint m_swipeStart;
    /// 目标页在右边（forward=true，主屏 → 控制中心）还是左边（false，返回）。
    bool m_swipeForward = true;
    /// 目标页在 m_pages 里的下标；-1 表示目标是主屏。
    int m_swipeTargetPage = -1;
    /// 跟手期间的最近一次采样（算甩速用）。
    QPoint m_swipeLastPos;
    qint64 m_swipeLastMs = 0;
    qreal m_swipeVelocity = 0.0;   ///< 像素/秒，向右为正
    qreal m_swipeProgress = 0.0;   ///< 当前跟手进度（0 = 还在原页，1 = 已到目标页）
    /// 🔴 横滑落定时会先切页面栈再补完动画；这期间 showPage / showConsole
    ///    **不许**自己再起一段转场（会打断正在补完的那一段，画面闪一下）。
    bool m_swipeSwitching = false;

    // ── 左边缘右滑返回（iOS 手势）────────────────────────────────────────
    /// 正在跟一条从窗口左边缘起手的拖动。只有页面态才可能置 true。
    bool m_edgeSwipeTracking = false;
    QPoint m_edgeSwipeStart;   ///< 按下点（central 坐标）

    // ── 控制台主屏 ───────────────────────────────────────────────────────
    ConsolePage* m_consolePage = nullptr;

    QLabel* m_statusDevice = nullptr;
    QLabel* m_statusMode = nullptr;
    QLabel* m_statusTask = nullptr;
    QLabel* m_statusPartition = nullptr;
    QLabel* m_statusSpeed = nullptr;
    QLabel* m_statusTotal = nullptr;
    QLabel* m_statusPercent = nullptr;
    /// EDL 链路两段（2026-10-16 从内嵌 EDL 窗底部三格并入）：协议通信状态 / 连接状态。
    QLabel* m_statusProtocol = nullptr;
    QLabel* m_statusLink = nullptr;
    ToolProgressBar* m_barTotal = nullptr;    // 双进度条：总进度
    ToolProgressBar* m_barPart = nullptr;     //            当前分区进度
    /// 底部跑马灯带：有「个性签名」滚签名，没有就轮播人生哲理文案。
    MarqueeLabel* m_marquee = nullptr;
    QTimer* m_marqueeRotateTimer = nullptr;   ///< 哲理文案轮播（仅无签名时转）
    int m_marqueeQuoteIndex = 0;

    // ── 彩蛋：Konami 码进度（↑↑↓↓←→←→BA）────────────────────────────────
    //  在 qApp 级 eventFilter 里累积；只认 KeyPress，命中即清零免得连按重复触发。
    //  焦点在输入框（QLineEdit / QTextEdit / QPlainTextEdit）时整段跳过，
    //  否则用户正常打字会被这套序列吃掉（尤其 B / A 两个字母）。
    int m_konamiProgress = 0;

    PageTransition* m_transition = nullptr;   // 3D 转场叠层（null = 动画已关）
    QWidget* m_central = nullptr;             // shellRoot：灵动岛的宿主
    QWidget* m_mainColumn = nullptr;          // 顶栏 + 页面栈 + 状态栏 的竖列

    QVector<PageEntry> m_pages;
    bool m_darkTheme = true;
    bool m_dragging = false;
    bool m_entrancePlayed = false;   ///< 主窗口入场动画只播一次

    // ── 个性签名（服务器关停后恒为空串：跑马灯回落哲理轮播、顶栏只留署名）──
    QString m_signature;

    // ── 控制台快照缓存 ────────────────────────────────────────────────────
    //  主屏是**静态**的（图标网格 + Dock，两次访问之间不会变），而转场每次都要
    //  grab 新旧两页 —— 两次全窗渲染是切页卡顿的主因之一。控制台那一侧改成缓存，
    //  每次转场就只剩一次 grab。失效时机：窗口尺寸变、换主题、入口列表变。
    QPixmap m_consoleShot;
    bool m_consoleShotValid = false;
    /// Dock 玻璃背板的延迟更新是否已排队（防抖：一次 resize 会连发好几次）。
    bool m_dockBackdropQueued = false;
    /// 视频帧序号（每帧 +1）与上次更新 Dock 背板时的序号。
    /// 用帧序号而不是时钟：30fps 视频下 45 帧 ≈ 1.5s，换成 60fps 视频用时钟算
    /// 会变成每 0.75s 一次，白白多一倍开销。
    qint64 m_bgSourceSeq = 0;
    qint64 m_dockBackdropSeq = 0;
    /// 视频刚打开、还没收到第一帧。
    /// 🔴 不能靠 `m_bgSource.isNull()` 判断首帧：从静态图切到视频壁纸时
    ///    m_bgSource 还留着上一张图（非空），首帧判断永远为假 —— Dock 的
    ///    玻璃背板就一直停在「底色晕」上，浅色主题下表现为一块白板。
    bool m_bgVideoAwaitingFirstFrame = false;

    // ── 动态背景（对齐 C# DynamicBackground：色晕常驻 + 壁纸层 + KenBurns 漂移）──
    QImage m_bgSource;          // 预模糊后的原图（窗口无关）
    QPixmap m_bgFrame;          // 按窗口尺寸缩放后的当前帧（静态图用）
    QPixmap m_bgBase;           // 底色 + 环境色晕（缓存，随尺寸/主题重建）
    /// 视频帧在窗口里的落点（cover 裁切后的目标矩形）。视频**不**走 m_bgFrame：
    /// 每帧都 fromImage + scaled 会白搬两次全窗像素，直接在 paintEvent 里
    /// drawImage(rect, m_bgSource) 让光栅引擎一趟缩放完（2026-10-17 实测）。
    QRectF m_bgVideoRect;
    QTimer* m_bgAnimTimer = nullptr;
    double m_bgAnimT = 0;       // 漂移相位（秒）
    /// KenBurns 节流计数：每 N 次 100ms tick 才刷一次玻璃背板。
    /// 🔴 不能每 tick 都刷 —— 背板重算是 5~8ms，100ms 一刷就是 5~8% 的核，
    ///    而 0.1s 内画面只漂 0.1px，玻璃底下根本看不出变化。
    int m_bgAnimTick = 0;
    double m_bgOpacity = 0.35;
    double m_bgBlur = 0;
    QString m_bgFit;
    bool m_bgAnim = false;
    /// 视频壁纸（mp4 等）。见 kc/media/VideoBackground.h —— 走 Windows Media
    /// Foundation 而不是 Qt6::Multimedia，因为静态单文件包那套 Qt 没有 FFmpeg 后端。
    kc::media::VideoBackground* m_bgVideo = nullptr;
    bool m_bgIsVideo = false;
    /// 最小化时把视频解码线程停掉（见 changeEvent），恢复时再打开。
    bool m_bgVideoPaused = false;

    /// 全局单例指针（instance() 返回），在构造函数里置位、析构里清。
    static MainShell* s_instance;

    // ── iOS 玩法（2026-10-29）──────────────────────────────────────────────
    /// 辅助触控悬浮球（普通子控件，挂 central）。nullptr = 设置里关着 / 还没建。
    AssistiveTouch* m_assistiveTouch = nullptr;
    /// 待机模式覆盖层（挂 central，盖满整窗）。懒建。
    StandByOverlay* m_standby = nullptr;
    /// 通知中心下拉面板（顶层玻璃浮窗，复用）。懒建。
    /// 待机自动进入的计时器（单发）。idleSeconds = 0 时不停表。
    QTimer* m_standbyTimer = nullptr;
    /// 专注模式「刷机模式」当前是否开启。
    bool m_focusFlash = false;
    /// 通知中心面板当前是否展开（顶栏铃铛按钮的高亮态也读它）。
    /// 快捷指令页（2026-10-29）。页面本身在 m_pages 里，这里留一份指针是为了
    /// 悬浮球菜单「快捷指令」能直接切过去 / 跑某条指令（不靠 findChild 全树搜）。
    ShortcutsPage* m_shortcutsPage = nullptr;
    BatchFlashPage* m_batchFlashPage = nullptr;   // 批量刷机队列页（2026-10-31）
};

} // namespace kc::ui
