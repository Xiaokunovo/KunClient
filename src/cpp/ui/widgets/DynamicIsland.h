#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  DynamicIsland —— 灵动岛（2026-10-18 第四代：可玩性 + 个性化）
//
//  外壳顶栏正中悬浮的一枚黑色胶囊，语义对齐 iOS 的 Dynamic Island。
//
//  ── 折叠态（高 30，宽按文字自适应 118~220）──────────────────────────────────
//    · 状态点 + 一句话；有进度时底部一根 2.5px 进度线；
//    · 显示内容四档（IslandOptions::collapsed）：
//        auto   —— 进度 > 连接 > 设备 > 「就绪」（改造前的逻辑）
//        device —— 固定显示设备名
//        page   —— 固定显示当前页面名
//        clock  —— 显示 HH:mm（1s 定时器，只在用到时才走）
//    · **左右拖动切页**：横向拖过 8px 进入拖动态，胶囊跟手位移（阻尼 0.35），
//      两端浮出 ‹ / ›；松手位移过 30px 就发 pageStepRequested(±1)。
//
//  ── 展开态（宽度按列数，高度按行数）────────────────────────────────────────
//    四段式，从上到下：
//      ① 标题行（44）：左「灵动岛」、右状态点 + 一句话、最右收起 chevron；
//      ② 搜索行（34）：真 QLineEdit 子控件，输入即过滤页表（外壳做过滤）；
//      ③ 快捷动作条（34）：截图 / 主屏 / 控制中心 / 设置 四个胶囊；
//      ④ 页面卡片网格（58 高 × N 行）：点一张发 pageRequested(key)；
//      ⑤ 设备读数条（30）：芯片 / 模式 / 任务 / 进度，一条读完。
//
//  ── 个性化（IslandOptions，来自 AppSettings，外壳喂进来）───────────────────
//    · theme：ink（纯黑，iOS 原味）/ accent（品牌蓝加深）/ glass（深色玻璃）；
//    · radius：展开面板圆角（折叠态永远是胶囊，= 高度的一半）；
//    · columns：4（紧凑，行数变多）/ 6（标准）/ 8（宽，大屏）；
//    · visibility：always / hover（折叠时缩成 34×5 的小短横，移上去才展开）/ hidden；
//    · dim：展开时是否压暗背景（外壳读这个值去驱动 IslandScrim）；
//    · liteMotion：关掉卡片错峰淡入、展开时长 340 → 180ms。
//
//  🔴 岛体**永远深色**（连浅色主题下也是黑的）—— 这是灵动岛的识别特征，
//     所以这里的取色**不走 ThemeManager**，文字一律白色系。
//  🔴 搜索框是**真子控件**：paintEvent 里的 setClipPath 管不到子控件，所以收起时
//     必须 hide()，否则它会浮在折叠胶囊外面（这是自绘控件里嵌输入框最容易踩的坑）。
//
//  位置由外壳在 resizeEvent 里调 reposition() 维护（贴顶 + 水平居中）；
//  展开后会向下盖住页面内容，正是 iOS 的行为 —— 所以它是外壳 central 的
//  直接子控件并 raise() 到最上层。
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class QLineEdit;
class QPropertyAnimation;
class QTimer;

namespace kc::ui {

/// 灵动岛的状态快照（外壳从 ShellStatus 中枢喂进来）。
struct IslandState {
    QString device;      ///< 设备读数（"-" = 未知）
    QString mode;        ///< 当前模式（EDL / FASTBOOT …）
    QString task;        ///< 当前任务（空闲 / 刷写中 …）
    QString protocol;    ///< 协议通信状态（正常 / 异常）
    QString link;        ///< 连接状态（"● 已连接"）
    QString linkState;   ///< idle | ok | failed —— 决定状态点颜色
    int percent = -1;    ///< 进度百分比；< 0 表示无进度（不画进度线）
};

/// 展开态里的一张页面卡片（外壳 registerPages 之后整表喂进来）。
struct IslandPage {
    QString key;     ///< 页面 key（与 MainShell::navigateTo 同一套）
    QString title;   ///< 卡片下方显示的名字
};

/// 灵动岛的外观 / 行为选项。默认值 = 改造前的外观，老用户升级后观感不变。
struct IslandOptions {
    QString theme = QStringLiteral("ink");          ///< ink | accent | glass
    int radius = 26;                                ///< 展开面板圆角
    QString collapsed = QStringLiteral("auto");     ///< auto | device | page | clock
    QString visibility = QStringLiteral("always");  ///< always | hover | hidden
    bool dim = true;                                ///< 展开时压暗背景
    int columns = 6;                                ///< 4 | 6 | 8
    bool liteMotion = false;                        ///< 动效精简
};

class DynamicIsland : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal expansion READ expansion WRITE setExpansion)

public:
    explicit DynamicIsland(QWidget* parent = nullptr);

    void setState(const IslandState& state);

    /// 页面表（顺序即卡片顺序，行优先）。超出网格容量的部分不显示。
    void setPages(const QVector<IslandPage>& pages);
    /// 当前所在页的 key（空 = 主屏）。用来给那张卡片加一圈强调色描边。
    void setCurrentKey(const QString& key);
    /// 当前页标题（折叠态 collapsed == "page" 时显示）。
    void setCurrentTitle(const QString& title);

    /// 外观 / 行为选项。改了立即重排并重绘（展开中也会跟着变尺寸）。
    void setOptions(const IslandOptions& options);
    const IslandOptions& options() const { return m_opt; }

    /// 液态玻璃背板（theme == "glass" 时用）。由外壳从壁纸层合成后喂进来 ——
    /// 岛自己拿不到壁纸，而 grab 窗口会把岛自己抓进去（磨砂后是一块灰糊）。
    /// 传空图 = 退回「深色实心 + 一道高光」的旧观感。
    void setBackdrop(const QImage& backdrop);
    bool hasBackdrop() const { return !m_backdrop.isNull(); }

    void setExpanded(bool expanded);
    bool isExpanded() const { return m_expanded; }

    /// 按父控件宽度重新水平居中并贴顶（外壳 resizeEvent 调用）。
    void reposition();

    /// 搜索框里的文字（外壳据此过滤页表；空串 = 不过滤）。
    QString searchText() const;
    /// 清空搜索框（收起面板时调）。
    void clearSearch();

    /// 在折叠胶囊上闪一句话（「已复制截图」「已是最后一页」…）。
    /// 期间 collapsedText() 被它顶掉，到点自动回到正常状态。
    /// 为什么不用 NotificationBar：那是一条通栏，挂在 shellRoot 上没有布局会得到
    /// 0 尺寸；而岛的折叠态本来就在显示一句话，借它当提示区是零成本的。
    void flashMessage(const QString& text, int ms = 1800);

signals:
    /// 展开态里点了某张页面卡片。key 与 MainShell::navigateTo 一致。
    void pageRequested(const QString& key);
    /// 折叠态左右拖动切页：-1 = 上一页，+1 = 下一页。
    void pageStepRequested(int delta);
    /// 快捷动作条：screenshot | console | control | settings。
    void actionTriggered(const QString& id);
    /// 搜索框内容变化（外壳过滤页表后 setPages 回来）。
    void searchChanged(const QString& text);
    /// 展开进度（0 = 折叠，1 = 完全展开）。外壳拿它同步背景压暗层的浓度。
    void expansionChanged(qreal value);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    qreal expansion() const { return m_expansion; }
    void setExpansion(qreal value);
    void applyGeometry();
    QSize currentSize() const;
    QSize expandedSize() const;

    // ── 几何 ────────────────────────────────────────────────────────────────
    int columnCount() const;
    int rowCount() const;
    QRectF searchRect() const;
    QVector<QRectF> actionRects() const;
    QVector<QRectF> cardRects() const;
    QRectF readoutRect() const;
    /// 命中的卡片下标；-1 = 没命中。
    int cardAt(const QPoint& pos) const;
    /// 命中的快捷动作下标；-1 = 没命中。
    int actionAt(const QPoint& pos) const;
    /// 第 index 张卡片的「淡入权重」：展开到 55% 之后才开始逐张出现。
    qreal cardReveal(int index) const;
    /// 搜索框跟着展开进度摆位 / 显隐。
    void layoutSearch();
    /// 折叠态是否缩成小短横（visibility == "hover" 且没悬停、没展开）。
    bool collapsedAsDash() const;

    // ── 绘制 ────────────────────────────────────────────────────────────────
    QString collapsedText() const;
    QColor statusColor() const;
    QColor bodyColor() const;
    QColor bodyStrokeColor() const;
    void paintCollapsed(QPainter& p) const;
    void paintHeader(QPainter& p) const;
    void paintActions(QPainter& p) const;
    void paintCards(QPainter& p) const;
    void paintReadout(QPainter& p) const;

    void updateClockTimer();

    IslandState m_state;
    IslandOptions m_opt;
    /// 液态玻璃背板（theme == "glass" 且非空时才走玻璃绘制路径）。
    QImage m_backdrop;
    QVector<IslandPage> m_pages;
    QString m_currentKey;
    QString m_currentTitle;
    qreal m_expansion = 0.0;   ///< 0 = 折叠，1 = 展开
    bool m_expanded = false;
    bool m_hover = false;
    int m_hoverCard = -1;      ///< 光标停在哪张卡片上（-1 = 无）
    int m_hoverAction = -1;    ///< 光标停在哪颗快捷动作上（-1 = 无）

    // ── 折叠态横向拖动 ──────────────────────────────────────────────────────
    bool m_dragArmed = false;  ///< 已按下、还没越过死区
    bool m_dragActive = false; ///< 已越过死区，正在跟手
    QPoint m_pressPos;
    qreal m_dragDx = 0.0;      ///< 当前位移（像素，向右为正）

    /// 折叠态宽度缓存：文字变了要重算，否则胶囊宽度不会跟着内容走。
    mutable int m_cachedCollapsedW = 0;

    /// 搜索框（真子控件，见头注释的红线说明）。
    QLineEdit* m_search = nullptr;
    /// 折叠态显示时钟时的秒级刷新（只有 collapsed == "clock" 才启动）。
    QTimer* m_clock = nullptr;
    /// flashMessage 的临时文案与它的收尾定时器。
    QString m_flash;
    QTimer* m_flashTimer = nullptr;

    /// 🔴 QPointer，不能是裸指针：Motion::tween() 内部用 DeleteWhenStopped 启动动画，
    ///    跑完就自删；裸指针此刻已悬垂，下一次 setExpanded() 里的 `m_anim->stop()`
    ///    就是 use-after-free —— 表现为「展开一次正常、再展开就闪退」。
    QPointer<QPropertyAnimation> m_anim;
};

} // namespace kc::ui
