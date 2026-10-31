#include "ui/MainShell.h"

#include "kc/config/AppSettings.h"
#include "kc/config/ClientConfig.h"
#include "kc/devices/UsbDeviceNotifier.h"
#include "kc/media/VideoBackground.h"   // 视频壁纸：Windows Media Foundation 解码
#include "kc/tools/WallpaperEngineService.h"   // 背景降级：视频壁纸 → 官方预览素材
#include "kc/version.h"
#include "ui/pages/AboutPage.h"
#include "ui/pages/AndroidToolsPage.h"
#include "ui/pages/AppManagerPage.h"
#include "ui/pages/BasePage.h"
#include "ui/pages/CommandConsolePage.h"
#include "ui/pages/ConsolePage.h"
#include "ui/widgets/ConsoleWidgets.h"
#include "ui/pages/ControlCenterPage.h"
#include "ui/pages/DeviceHomePage.h"
#include "ui/pages/EdlFlashPage.h"
#include "ui/MainWindow.h"   // 辅助触控「刷机记录」要调 EDL 窗的 showFlashHistory
#include "ui/pages/FastbootFlashPage.h"
#include "ui/pages/FileTransferPage.h"
#include "ui/pages/MtkFlashPage.h"
#include "ui/pages/OnePlusFlashPage.h"
#include "ui/pages/PayloadPage.h"
#include "ui/pages/ScreenMirrorPage.h"
#include "ui/pages/SettingsPage.h"
#include "ui/pages/ShortcutsPage.h"
#include "ui/pages/BatchFlashPage.h"
#include "ui/pages/VisualFlashPage.h"
#include "ui/pages/XiaomiFastbootPage.h"
#include "ui/ShellStatus.h"
#include "ui/theme/GlassSettings.h"   // 液态玻璃参数（顶栏胶囊 / 灵动岛共用）
#include "ui/theme/Motion.h"
#include "ui/theme/PageTransition.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/AppIcons.h"
#include "ui/widgets/AssistiveTouch.h"
#include "ui/widgets/DynamicIsland.h"
#include "ui/widgets/LiquidGlass.h"
#include "ui/widgets/LogView.h"
#include "ui/widgets/MarqueeLabel.h"
#include "ui/widgets/StandByOverlay.h"
#include "ui/widgets/ToolProgressBar.h"

#include <QAction>
#include <QAbstractSlider>   // 横滑手势要放过滑杆上的拖动
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>   // 灵动岛「截图」快捷动作：整窗截图 → 剪贴板
#include <QCursor>
#include <QDateTime>   // 横滑甩速采样（主屏 ⇄ 控制中心）
#include <QDebug>   // qWarning：背景素材解码失败不能再静默
#include <QFrame>
#include <QGraphicsBlurEffect>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>       // 彩蛋：Konami 码要读真实按键
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainterPath>
#include <QPlainTextEdit>  // 彩蛋：判断焦点是否在输入框（Konami 码不拦打字）
#include <QTextEdit>
#include "ui/dialogs/ThemedMessageBox.h"
#include <cmath>
#include <QtMath>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOption>
#include <QPointer>
#include <QPropertyAnimation>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>
#include <QWindow>

#include <thread>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif


namespace kc::ui {

namespace {
/// 窗口圆角半径。
/// 🔴 必须与 light.qss / dark.qss 里 `QFrame#shellRoot` 的 `border-radius` 逐像素一致：
///    壁纸裁切、顶栏自绘裁剪都用这个值，描边用 QSS 那个值 —— 两边不一致就会在四角
///    留出一圈空缝（屏上表现为白色月牙 / 双层圆角）。
constexpr int kShellCornerRadius = 30;

// 标题栏窗口钮：矢量绘制（最小化 / 最大化 / 还原 / 关闭）。
// 原来用文本字形 —/▢/× ，在不同字体/ DPI 下渲染粗糙、边缘发虚，用户判「不好看」。
// 这里改为 QPainter 画 crisp 图标：先走 QPushButton::paintEvent 画 QSS 的
// 背景 / 边框 / hover（chrome 不动），再用 palette 的 ButtonText 色叠画图形 ——
// 该色由 QSS 的 color: 驱动，普通态与 hover 态自动跟随，无需手动重渲染。
class WindowButton : public QPushButton {
public:
    enum class Kind { Minimize, Maximize, Restore, Close };
    explicit WindowButton(Kind kind, QWidget* parent = nullptr)
        : QPushButton(parent), m_kind(kind) {
        setObjectName(kind == Kind::Close ? QStringLiteral("closeButton")
                                          : QStringLiteral("windowButton"));
        setCursor(Qt::PointingHandCursor);
        setFixedSize(42, 30);
        setFocusPolicy(Qt::NoFocus);
    }
    void setKind(Kind k) { if (m_kind != k) { m_kind = k; update(); } }

protected:
    void paintEvent(QPaintEvent* e) override {
        QPushButton::paintEvent(e);   // QSS chrome（背景 / 边框 / hover）
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 🔴 取色不能只靠 palette().color(QPalette::ButtonText)：
        //    QSS 的 `color:` 只保证 style 绘制用它，**不保证回写调色板**。
        //    浅色主题实测：QSS 写 #5F6B7A（4.78:1），palette() 却给 #8D96A3
        //    （2.64:1，低于非文本 3:1 门槛）—— 图标因此偏淡。
        //    这里优先用 QStyleOptionButton 把 QSS 实际生效的调色板取出来，
        //    拿不到才退回 palette()。
        QColor c;
        {
            QStyleOptionButton opt;
            initStyleOption(&opt);
            c = opt.palette.color(QPalette::ButtonText);
        }
        if (c.alpha() == 0) c = palette().color(QPalette::ButtonText);
        const qreal cx = width() / 2.0;
        const qreal cy = height() / 2.0;
        const qreal s = 8.5;    // 图标基准尺寸
        // 🔴 最小化用 1.8px，其余 1.35px。
        //    原因：最小化只有**一条水平线**，1.35px 的线落在半像素上时，
        //    抗锯齿会把浓度摊到两行上，每行只剩 ~60% —— 实测最暗像素
        //    只有 #8D96A3（2.65:1），比方形/叉号（#5F6B7A / 4.81:1）淡得多。
        //    加粗到 1.8px 后线有完整一行实心像素，颜色才等于 QSS 给的值。
        const qreal t = (m_kind == Kind::Minimize) ? 1.8 : 1.35;
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, t, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        switch (m_kind) {
        case Kind::Minimize:
            p.drawLine(QPointF(cx - s / 2, cy), QPointF(cx + s / 2, cy));
            break;
        case Kind::Maximize:
            p.drawRect(QRectF(cx - s / 2, cy - s / 2, s, s));
            break;
        case Kind::Restore: {
            const qreal o = 2.4;
            p.drawRect(QRectF(cx - s / 2, cy - s / 2 + o, s - o, s - o));
            p.drawRect(QRectF(cx - s / 2 + o, cy - s / 2, s - o, s - o));
            break;
        }
        case Kind::Close: {
            const qreal d = s / 2;
            p.drawLine(QPointF(cx - d, cy - d), QPointF(cx + d, cy + d));
            p.drawLine(QPointF(cx - d, cy + d), QPointF(cx + d, cy - d));
            break;
        }
        }
    }

private:
    Kind m_kind;
};

// ── 顶栏底衬 ────────────────────────────────────────────────────────────────
/// 顶栏自绘底衬。
///
/// QSS 只能给「一块纯色 + 一条 border-bottom」，于是顶栏和下面的内容永远糊成
/// 一整片，看着像网页后台的 header。iOS 导航栏之所以「浮」在内容之上，靠的是
/// 三样东西：**半透明底**（内容透得上来）、**上沿一道内高光**（玻璃的反光）、
/// **下沿一道收边**（把两块面切开）。这里就补这三样，颜色仍走 ThemeManager，
/// 不写死深浅。
class TopBarChrome : public QFrame {
public:
    explicit TopBarChrome(QWidget* parent = nullptr) : QFrame(parent) {}

    /// 控制台态 / 页面态，两种完全不同的底：
    /// · 控制台：壁纸要连到窗口顶部，顶栏只在最上面压一层**渐隐蒙层**
    ///   （顶 70% 有底、往下化进壁纸），文字可读且没有硬边；
    /// · 页面：内容区在顶栏下面，顶栏给实色 + 上高光 + 下收边，把两块切开。
    void setConsoleMode(bool on) {
        if (m_consoleMode == on) return;
        m_consoleMode = on;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const bool dark = ThemeManager::instance().isDark();
        QPainter p(this);
        const QRectF r = QRectF(rect());

        if (m_consoleMode) {
            // 主屏：壁纸一路铺到窗口顶部，顶栏**不画任何底** —— 蒙层压上去会和
            // 壁纸打架（浅色主题 + 深色壁纸时尤其脏，白蒙蒙一片）。
            // 可读性交给两个悬浮胶囊（品牌组 / 右侧控件组）自己解决。
            return;
        }

        // 1) 实色底：浅色下比内容区**偏冷偏深一档**（#ECF1F9 vs #FBFCFE），
        //    否则两块浅色贴在一起，顶栏就"消失"了 —— 这正是上一版像没导航栏的原因。
        //    只留 3% 透气，不让壁纸花纹透上来（透了整条顶栏会花得很难看）。
        QColor base = dark ? QColor(0x10, 0x13, 0x19) : QColor(0xEC, 0xF1, 0xF9);
        base.setAlphaF(dark ? 0.95 : 0.97);

        // 🔴 顶栏铺到窗口边沿，必须自己跟着窗口收圆 —— 否则一个 `fillRect` 的方角
        //    会把窗口 30px 的圆角整块盖掉（2026-10-17 用户截图：「圆角问题」，
        //    屏上左上角是直角、而壁纸层是圆的）。
        //    做法：用「两倍高」的圆角矩形做裁剪 —— 下半截落到控件外面，
        //    于是只有**上面两个角**被切圆，下沿保持直角（下沿是内部接缝，不能圆）。
        QPainterPath clip;
        clip.addRoundedRect(QRectF(r.left(), r.top(), r.width(), r.height() * 2.0),
                            kShellCornerRadius, kShellCornerRadius);
        p.setClipPath(clip);
        p.fillRect(r, base);

        // 2) 上沿内高光 + 下沿收边（玻璃的两道边）。
        p.fillRect(QRectF(r.left(), r.top(), r.width(), 1),
                   dark ? QColor(255, 255, 255, 16) : QColor(255, 255, 255, 205));
        p.fillRect(QRectF(r.left(), r.bottom() - 1, r.width(), 1),
                   dark ? QColor(0, 0, 0, 115) : QColor(0xD3, 0xDD, 0xEC, 210));
    }

private:
    bool m_consoleMode = true;
};

// ── 主屏顶栏的玻璃胶囊 ──────────────────────────────────────────────────────
/// 主屏态顶栏元素压在壁纸上，必须自带一层底才托得住字。
///
/// 原本想用 QSS 的 `[console="true"]` 属性分支，实测不生效（属性 + re-polish
/// 在构造期和显隐切换时并不可靠，项目里已经踩过同类坑）—— 干脆自绘，所见即所得。
class NavPill : public QWidget {
public:
    explicit NavPill(QWidget* parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TranslucentBackground, true);
    }

    /// 只在主屏态染色：页面态的顶栏本身就是实色条，再叠胶囊就成双层了。
    void setTinted(bool on) {
        if (m_tinted == on) return;
        m_tinted = on;
        update();
    }
    bool tinted() const { return m_tinted; }

    /// 液态玻璃背板（与本体等尺寸的「它背后那块真实画面」）。
    /// 由 MainShell::updateGlassBackdrops 从壁纸层合成后喂进来。
    void setBackdrop(const QImage& img) {
        m_backdrop = img;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!m_tinted) return;
        const bool dark = ThemeManager::instance().isDark();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect());
        const qreal rad = r.height() / 2.0;   // 胶囊 = 高度一半（不是 999px）

        // 真液态玻璃：磨砂 + 边缘折射 + 顶沿高光。胶囊很矮（~34px），
        // 折射带按短边算只有 7px，所以参数里把 edgeBand 收窄一点，否则
        // 整个胶囊都会被折射带吃掉、变成一团糊。
        if (!m_backdrop.isNull()) {
            glass::Style st = currentGlassStyle();
            st.press = 0.0;         // 顶栏胶囊不响应按压果冻
            st.edgeBand = 0.30;     // 矮条上放大比例，折射才落在可见区域内
            st.lightPos = QPointF(0.5, 0.02);   // 光从正上方来（顶栏在窗口最上沿）
            st.tint = qMin(st.tint, 0.52);      // 太实会挡住壁纸，胶囊看着像贴纸
            // 🔴 dark 必须**跟随主题**，不能写死 true。
            //    胶囊里的文字颜色是 QSS 给的：深色下品牌名 #F2F6FC（白）、
            //    浅色下 #12161C（近黑）。早先这里写死 dark=true ⇒ 浅色主题下
            //    深字压深玻璃，品牌名和版本号几乎看不见（2026-10-30 截图实测）。
            //    跟随主题后：浅色 → 浅玻璃 + 深字，深色 → 深玻璃 + 白字，两侧都对。
            glass::paint(p, r, rad, m_backdrop, /*dark=*/dark, st);
            return;
        }

        // 背板还没就绪（视频壁纸首帧前 / 页面态切换瞬间）：退回半透明胶囊。
        // 🔴 不能什么都不画 —— 那会在启动时闪一下「文字浮在半空」。
        p.setPen(Qt::NoPen);
        p.setBrush(dark ? QColor(255, 255, 255, 26) : QColor(255, 255, 255, 140));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
        p.setBrush(Qt::NoBrush);
        // 描边跟主题走：浅色下用淡墨（白底压浅壁纸时白描边等于没有），
        // 深色下用白墨（黑底压深壁纸时黑描边同理）。
        p.setPen(QPen(dark ? QColor(255, 255, 255, 44) : QColor(15, 23, 42, 22), 1.0));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    }

private:
    bool m_tinted = false;
    QImage m_backdrop;
};

// ── 返回钮 ──────────────────────────────────────────────────────────────────
/// 返回控制台：一枚圆形玻璃片 + 矢量 chevron。
///
/// 原来是「‹ 控制台」文字胶囊，和右边的页名并排挤在左上角 —— 一行里一个中文词
/// 加一个标题，是后台管理系统面包屑的长相，不是导航栏。iOS 的返回钮从不带文字
/// （左箭头本身就是约定），点了才知道是返回。这里照办，把「控制台」三个字让给
/// tooltip。
class BackChevronButton : public QPushButton {
public:
    explicit BackChevronButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setFixedSize(32, 32);
        setToolTip(QStringLiteral("返回控制台"));
    }

protected:
    void paintEvent(QPaintEvent* e) override {
        QPushButton::paintEvent(e);   // QSS 负责圆底 / hover / 按下
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor c = palette().color(QPalette::ButtonText);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(c, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const qreal cx = width() / 2.0;
        const qreal cy = height() / 2.0;
        QPainterPath chevron;
        chevron.moveTo(cx + 2.4, cy - 5.0);
        chevron.lineTo(cx - 2.6, cy);
        chevron.lineTo(cx + 2.4, cy + 5.0);
        p.drawPath(chevron);
    }
};

// 高斯模糊一次成型（C# 用 XAML BlurEffect；Qt 用 QGraphicsBlurEffect 离屏渲染）。
// 长边压到 1600 再糊：模糊后的图本来就无高频，放大即平滑，省内存也省首次耗时。
QImage blurImage(const QImage& src, double radius)
{
    QImage scaled = src;
    if (scaled.width() > 1600)
        scaled = scaled.scaledToWidth(1600, Qt::SmoothTransformation);

    const int margin = static_cast<int>(radius) + 8;
    QGraphicsScene scene;
    scene.setSceneRect(-margin, -margin, scaled.width() + 2 * margin,
                       scaled.height() + 2 * margin);
    QGraphicsPixmapItem item(QPixmap::fromImage(scaled));
    item.setPos(0, 0);
    QGraphicsBlurEffect effect;
    effect.setBlurRadius(radius);
    effect.setBlurHints(QGraphicsBlurEffect::QualityHint);
    item.setGraphicsEffect(&effect);
    scene.addItem(&item);

    QImage out(scene.sceneRect().toRect().size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter p(&out);
    scene.render(&p);
    return out;
}

/// 视频帧专用的**廉价**模糊。
/// blurImage() 走的是 QGraphicsBlurEffect + QualityHint，单张静态壁纸无所谓，
/// 但逐帧跑它视频立刻掉成幻灯片（一帧几十毫秒）。这里用「缩到 1/N 再放大回来」
/// 的近似：效果对壁纸足够（本来就压着不透明度），耗时只要两三毫秒。
QImage cheapBlur(const QImage& src, double radius)
{
    if (src.isNull() || radius <= 0.5) return src;
    const int factor = qBound(2, static_cast<int>(radius / 4.0) + 2, 12);
    const QSize small = src.size() / factor;
    if (small.width() < 2 || small.height() < 2) return src;
    return src.scaled(small, Qt::IgnoreAspectRatio, Qt::FastTransformation)
        .scaled(src.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

/// 顶栏主题切换：深色主题画太阳（点了去浅色），浅色画月牙（点了去深色）。
/// 月牙用 EvenOdd 双圆抠图，不依赖背景色；太阳是圆 + 8 射线。
class ThemeToggleButton : public QPushButton {
public:
    explicit ThemeToggleButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setCursor(Qt::PointingHandCursor);
        setFixedSize(30, 30);
    }

protected:
    void paintEvent(QPaintEvent* e) override {
        QPushButton::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor c = palette().color(QPalette::ButtonText);
        const qreal cx = width() / 2.0;
        const qreal cy = height() / 2.0;
        if (ThemeManager::instance().isDark()) {
            p.setPen(QPen(c, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.drawEllipse(QPointF(cx, cy), 3.9, 3.9);
            for (int i = 0; i < 8; ++i) {
                const qreal a = i * M_PI / 4.0;
                p.drawLine(QPointF(cx + 5.5 * std::cos(a), cy + 5.5 * std::sin(a)),
                           QPointF(cx + 7.6 * std::cos(a), cy + 7.6 * std::sin(a)));
            }
        } else {
            QPainterPath moon;
            moon.addEllipse(QRectF(cx - 6.4, cy - 6.4, 12.8, 12.8));
            moon.addEllipse(QRectF(cx - 2.6, cy - 8.8, 12.0, 12.0));
            p.fillPath(moon, c);
        }
    }
};

/// 与 ThemeToggleButton 同族（自绘 + palette 取色），不用字体字形。
/// 顶栏品牌 meta（署名 + 签名档）的最大像素宽。顶栏左侧是品牌组 / 返回钮 + 页名、
/// 正中留给灵动岛，右侧还有主题钮 + 分隔线 + 三个窗口钮，这个预算是不挤到灵动岛的上限。
constexpr int kBrandMetaWidth = 200;

// ── 彩蛋：Konami 码（↑↑↓↓←→←→BA）───────────────────────────────────────────
//  维护者须知（这不是 bug）：在主窗口任意位置（**输入框除外**）依次按
//  ↑ ↑ ↓ ↓ ← → ← → B A 会触发一次彩蛋演出。序列判定见 feedKonamiKey，
//  事件入口在 MainShell::eventFilter 的「Konami」段。
//
//  为什么把按键表放这里而不是在 eventFilter 里写一串 if：这个表既是「序列」也是
//  「白名单」—— 按到表外的任意键就清零，避免用户按了一万次 A 再敲 B 也算命中。
constexpr int kKonamiSequence[] = {
    Qt::Key_Up, Qt::Key_Up, Qt::Key_Down, Qt::Key_Down,
    Qt::Key_Left, Qt::Key_Left, Qt::Key_Right, Qt::Key_Right,
    Qt::Key_B, Qt::Key_A,
};
constexpr int kKonamiLength = int(sizeof(kKonamiSequence) / sizeof(kKonamiSequence[0]));

/// 焦点控件是否是可输入文本的控件 —— Konami 彩蛋必须整段放过它们。
/// 用 qobject_cast 而不是 inherits()：QLineEdit 在项目里有一串子类
/// （IosSearchField 等），qobject_cast 走元对象，子类一样能认出来。
bool isTextInputWidget(const QWidget* w)
{
    if (!w) return false;
    return qobject_cast<const QLineEdit*>(w) != nullptr
           || qobject_cast<const QTextEdit*>(w) != nullptr
           || qobject_cast<const QPlainTextEdit*>(w) != nullptr;
}

// ── 环境光场（默认壁纸下给液态玻璃的「可折射物」）──────────────────────────
//
// 🔴 这层东西不是装饰，是液态玻璃能不能成立的前提。
//    `rebuildBgBase()` 原本只有「纯色 + 四团半径 0.62×长边 的径向渐变」，在
//    500×86 的 Dock 那片区域里几乎是常数（实测 20×20 窗口内极差只有 5/255）。
//    玻璃管线每一道工序都靠**局部差异**吃饭：磨砂磨一个常数还是常数，折射位移
//    一个常数还是常数 —— 于是整块玻璃退化成一块白板。Dock 与顶栏胶囊 100%
//    纯白、参数和图层顺序却全都正确，原因就在这里（2026-10-24 实测）。
//    真实壁纸自带大量高频内容（同类实测局部极差 110/255），所以设了壁纸就看
//    不出这个缺口；只有默认壁纸才稳定复现 —— 这也是它一直没被发现的原因。
//
// ⚠️ 尺度是这件事的关键，而且是**双向**约束：
//   · 下限：`glass::frost()` 先降采样再放大，factor = clamp(frost/2, 4, 24)；
//     默认磨砂 22 → factor 11，500×86 的 Dock 只剩 45×7 个采样点。细于 ~11px
//     的结构被整片抹平 —— 实测 ±6/255 的高频噪点过完管线后，折射可见度 RMS
//     只有 0.05 级（满量程 255），等于没做。
//   · 上限：折射位移只有 9~17px（见 LiquidGlass::process 的 strength）。结构
//     远大于位移时，整块内容平移还是平移，看不出「被吸进去」的形变。
//   工作尺度因此落在 **40~130px**：既活得过磨砂，又小到能被十几像素的位移扭曲。
//   实测这一档把 Dock 的边缘折射可见度从 0.08 提到 0.4 级（约 5 倍），
//   是肉眼能读出来的差别。
//
// 为什么用噪声而不是圆斑：等大圆斑排布读成「波点壁纸」，大小抖开的圆斑又容易
// 在疏处露出「一把圆规画了一堆圈」。两个八度的值噪声（value noise）叠出来是
// 连片的软起伏，读作「光与影」而不是「图案」—— 这是守住原视觉风格的关键。

/// 格点哈希 → [-1, 1]。
/// 固定常数异或 + xorshift-multiply：只需要「同一 (ix, iy, seed) 永远得到同一个
/// 值」，不需要密码学强度。
/// 🔴 种子是编译期常量而不是 time()：`rebuildBgBase()` 会随窗口拉伸 / 换主题 /
///    切页反复重跑，时间种子会让同一尺寸每次生成不同图案，屏上就是整块背景
///    在「抖」（用户会当成显卡花屏）。
qreal hashToUnit(int ix, int iy, quint32 seed)
{
    quint32 h = static_cast<quint32>(ix) * 0x9E3779B1u
                ^ static_cast<quint32>(iy) * 0x85EBCA77u
                ^ seed;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return static_cast<qreal>(h & 0xFFFFFFu) / 8388607.5 - 1.0;   // 24 位定点 → [-1,1]
}

/// 平滑值噪声：格点哈希 + smoothstep 插值。
/// 用 smoothstep 而不是线性插值，是因为线性插值在格点处一阶导不连续，放大后
/// 就是一圈肉眼可辨的菱形折线；smoothstep 一阶导连续，得到一整片软起伏。
qreal valueNoise(qreal x, qreal y, qreal cell, quint32 seed)
{
    const int cx = static_cast<int>(std::floor(x / cell));
    const int cy = static_cast<int>(std::floor(y / cell));
    const qreal fx = x / cell - cx;
    const qreal fy = y / cell - cy;
    const qreal sx = fx * fx * (3.0 - 2.0 * fx);
    const qreal sy = fy * fy * (3.0 - 2.0 * fy);
    const qreal n00 = hashToUnit(cx, cy, seed);
    const qreal n10 = hashToUnit(cx + 1, cy, seed);
    const qreal n01 = hashToUnit(cx, cy + 1, seed);
    const qreal n11 = hashToUnit(cx + 1, cy + 1, seed);
    const qreal top = n00 + (n10 - n00) * sx;
    const qreal bottom = n01 + (n11 - n01) * sx;
    return top + (bottom - top) * sy;
}

/// 往底色上叠一层两八度的环境光场（原地改 canvas）。
///
/// 两个八度分工：
///   · 粗（cell 130px）—— 决定整窗的明暗走向，负责「光从某一侧来」。
///   · 细（cell 52px）—— 负责 Dock / 顶栏胶囊那么窄的一条内部也有起伏。
/// 两者相加后按幅度铺开：粗 15 + 细 6，实测 Dock 区域局部极差约 13/255、
/// 全窗极差约 30/255 —— 肉眼读作「淡淡的明暗层次」，不会读成图案。
///
/// 只做**明暗**（三通道同量加减），不引入任何新色相：整体仍是原来那套冷白
/// 渐变，只是不再平坦。这是「不改变视觉风格」那条硬约束的落点。
///
/// 成本：噪声在 1/4 分辨率上求值（1280×820 → 320×205，6.5 万点），再平滑放大
/// 回全尺寸做一次纯加法。实测 rebuildBgBase 整体 <12ms，且只在窗口尺寸 / 主题
/// 变化时跑，不进任何动画路径。
void addAmbientLightField(QImage& canvas, bool dark)
{
    const int w = canvas.width();
    const int h = canvas.height();
    if (w < 16 || h < 16) return;

    // 1/4 分辨率求值：噪声是低频的，全分辨率求值只是把同样几个数重复四遍。
    const QSize qs(std::max(4, w / 4), std::max(4, h / 4));
    QImage field(qs, QImage::Format_Grayscale8);
    if (field.isNull()) return;

    constexpr quint32 kSeed = 0x6B756E31u;   // "Kun1"：固定值即可，见 hashToUnit
    for (int y = 0; y < qs.height(); ++y) {
        uchar* row = field.scanLine(y);
        for (int x = 0; x < qs.width(); ++x) {
            // 1/4 分辨率 → 坐标乘 4 才是全尺寸上的像素位置，格点频率才对得上。
            const qreal fx = x * 4.0;
            const qreal fy = y * 4.0;
            const qreal coarse = valueNoise(fx, fy, 190.0, kSeed) * 13.0;
            const qreal fine = valueNoise(fx, fy, 78.0, kSeed ^ 0x51EDC1u) * 5.0;
            row[x] = static_cast<uchar>(std::clamp(128.0 + coarse + fine, 0.0, 255.0));
        }
    }
    // 平滑放大回全尺寸：这一步本身就是低通，正好把 1/4 求值的方块边抹平。
    const QImage full = field.scaled(canvas.size(), Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation);
    if (full.isNull()) return;

    // 幅度：浅色底（0xFAFBFF）比深色底更「经不起」脏，同样的数值在浅底上更扎眼，
    // 所以浅色一档略收。这两个数不是拍的：它们对应 Dock 区域局部极差 13/255，
    // 是「肉眼能看出有层次」而「不像噪点 / 云纹」的交点（实测 sweep 过 6~22）。
    const qreal amp = dark ? 1.15 : 1.0;

    canvas = canvas.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        const uchar* nrow = full.constScanLine(y);
        auto* line = reinterpret_cast<QRgb*>(canvas.scanLine(y));
        for (int x = 0; x < w; ++x) {
            // 对称加减（不是叠加半透明白/黑）：半透明黑会把浅底整体压暗几十级，
            // 而半透明白在接近全白的底上又抬不动 —— 一正一负不对称，整窗会「变脏
            // 还变暗」（实测均值 233 → 204）。直接做对称的亮度偏移才能既加层次
            // 又保持原来的平均亮度。
            const int d = qRound((static_cast<int>(nrow[x]) - 128) * amp);
            const QRgb c = line[x];
            line[x] = qRgba(std::clamp(qRed(c) + d, 0, 255),
                            std::clamp(qGreen(c) + d, 0, 255),
                            std::clamp(qBlue(c) + d, 0, 255),
                            qAlpha(c));
        }
    }
}

/// 页面构造耗时探针（KC_TIMING=1）：打「距上一个标记的毫秒数」。
void pageTimingMark(const char* tag)
{
    if (qEnvironmentVariableIsEmpty("KC_TIMING")) return;
    static qint64 last = 0;
    static qint64 first = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!first) first = now;
    if (last) {
        std::fprintf(stderr, "[page] %5lld ms (+%5lld ms)  %s\n",
                     static_cast<long long>(now - first),
                     static_cast<long long>(now - last), tag);
    } else {
        std::fprintf(stderr, "[page]     0 ms  %s\n", tag);
    }
    std::fflush(stderr);
    last = now;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  IslandScrim —— 灵动岛展开时压在页面栈上的背景压暗层
//
//  为什么需要它：展开态面板（540×262，贴顶）的下沿会正好落在主屏图标网格的
//  **第一排图标与它的文字标签之间** —— 图标被面板挡住、标签却露在面板下面，
//  一串「Qualcomm / FASTBOOT / Payload …」孤零零浮在空白里，看着像渲染错位
//  （2026-10-18 截图实测）。面板高度是固定的，各页内容排布又各不相同，靠调高度
//  永远对不齐，所以干脆把「面板底下的东西」整体压暗：读者的注意力落在面板上，
//  露出来的半截内容变成理所当然的背景。
//
//  🔴 它接鼠标（**不**设 WA_TransparentForMouseEvents）：面板展开时点空白处只该
//     收起面板，不该顺手把底下那个图标/按钮也点掉。收起本身由 MainShell 的
//     qApp 级 eventFilter 负责（它在事件到达控件之前就跑了）。
//  🔴 浓度跟灵动岛的 expansion 走同一个补间（expansionChanged），不自己再起一条
//     动画 —— 两条动画时长稍有差异就会看到「面板还在长、暗层已经满了」。
// ─────────────────────────────────────────────────────────────────────────────
class IslandScrim : public QWidget {
public:
    explicit IslandScrim(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_NoSystemBackground, true);
        setCursor(Qt::ArrowCursor);
        hide();
    }

    /// level ∈ [0,1]：0 = 完全透明且隐藏，1 = 最浓。
    void setLevel(qreal level)
    {
        const qreal v = qBound(0.0, level, 1.0);
        if (qFuzzyCompare(m_level + 1.0, v + 1.0)) return;
        m_level = v;
        if (m_level <= 0.002) {
            if (isVisible()) hide();
            return;
        }
        if (!isVisible()) show();
        // 🔴 这里**不能** raise()：暗层和灵动岛都是 central 的直接子控件，暗层先建、
        //    岛后建并 raise()，层级才对。运行时再 raise() 会把暗层顶到岛上面 ——
        //    表现是「点开切换器，面板整个被蒙上一层灰」。
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        // 🔴 浓度必须分主题：浅色底下 41% 才把内容压成「背景」，而深色底本来就黑，
        //    41% 会把整窗压成一团泥 —— 连灵动岛面板的边界都糊没了（2026-10-18 深色
        //    截图实测）。深色只要 26%，够把内容推远、又留得住面板的轮廓。
        const int maxAlpha = ThemeManager::instance().isDark() ? 66 : 104;
        p.fillRect(rect(), QColor(6, 8, 12, int(maxAlpha * m_level)));
    }

private:
    qreal m_level = 0.0;
};

MainShell* MainShell::s_instance = nullptr;

MainShell::MainShell(QWidget* parent)
    : QMainWindow(parent)
{
    s_instance = this;
    setWindowTitle(QStringLiteral("KunClient"));
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    resize(1280, 820);
    setMinimumSize(1000, 640);

    buildUi();
    pageTimingMark("buildUi 完成");
    registerPages();
    pageTimingMark("registerPages 完成");
    updateConsoleEntries();   // 控制台主屏按页面表建图标网格 + Dock
    pageTimingMark("主屏图标网格完成");

    // 灵动岛展开后点别处即收起 —— 要收到「界面上任意位置」的按下事件，只能挂
    // 全局过滤器（见 eventFilter 里的开关判断，不展开时几乎零成本）。
    qApp->installEventFilter(this);

    // ── iOS 玩法：待机模式的空闲计时器（2026-10-29）──────────────────────
    // 单发；任何「用户在场」的证据（按键 / 点击 / 悬浮球拖动 / 待机层退出）
    // 都调 noteUserActivity() 重起。0 秒 = 从不自动进入（设置里的默认值）。
    m_standbyTimer = new QTimer(this);
    m_standbyTimer->setSingleShot(true);
    connect(m_standbyTimer, &QTimer::timeout, this, [this] {
        // 有待机层已经在显示 / 正在转场 / 窗口不在前台，都不该突然盖一层黑。
        if (m_standby && m_standby->isVisible()) return;
        if (!isVisible() || isMinimized()) return;
        enterStandby();
    });

    // 主题：优先命令行 --light/--dark 落下的 QSettings，其次 AppSettings。
    // 🔴 默认值必须与 main.cpp 的口径一致 = 浅色（2026-10-11 用户拍板：
    //    「没改过设置的新用户一律浅色」）。这里曾默认深色，新用户会被
    //    MainShell 自己翻成深色，与 main.cpp 的浅色 QSS 相冲。
    QSettings settings;
    applyTheme(settings.value(QStringLiteral("appearance/darkTheme"), false).toBool());

    // 顶栏品牌 meta：署名 + 版本（签名档由 profile 轮询回填）。
    updateBrandMeta();
    // 顶栏初始态 = 控制台（品牌组可见、返回钮隐藏、署名不显示）。
    // 别等第一次切页才对齐 —— 启动直接落在主屏，那一帧就是用户看到的第一帧。
    updatePageHeader(-1);

    // 动态背景：色晕常驻 + 壁纸层（对齐 C# MainWindow ctor 的 ApplyDynamicBackground）。
    reloadBackground();

    // 主题切换时同步内置控件（自绘 / 取令牌的部件需要主动重绘）。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this](bool dark) {
        applyTheme(dark);
        update();   // 窗底色晕 / 底色跟随主题
    });

    // USB 插拔钩子：设备切模式时各页可据此重探（幂等）。
    if (auto* notifier = kc::UsbDeviceNotifier::instance()) notifier->initialize();

    // ── 停更离线版（2.0.6）：以下联网初始化全部移除 ─────────────────────────
    //  云端强制下线（ForceLogout）、环境检测封禁回调、30s profile 轮询、
    //  启动更新检查 —— 都依赖服务器，服务器关停后无意义。

    // 启动落点：**控制台主屏**（2026-10-17 控制台重构）。
    // 此前是直接落到「主页」功能页；改成启动器模型后，主屏本身就是第一落点
    // ——「打开就能用」变成了「打开就能选」，与 iOS 主屏一致。
    showConsole();

    // iOS 玩法六项（2026-10-29）：最后应用一次，此时页面表 / 主屏 / 顶栏都已就位
    // （悬浮球要算父控件尺寸、专注模式要改页面 hidden 标记、待机计时器要读设置）。
    applyIosFeatures();
}

MainShell::~MainShell()
{
    // 全局事件过滤器必须摘掉：qApp 比本窗口活得久，留着就是悬空指针。
    if (qApp) qApp->removeEventFilter(this);

    // 视频解码线程必须显式收掉：它是独立线程 + COM 公寓，靠基类析构「顺带」
    // 清理会让线程在对象已经半死的时候回调进来。
    if (m_bgVideo) {
        m_bgVideo->close();
        m_bgVideo = nullptr;
    }

    // 转场叠层显式先删：它内部挂着自删的 QPropertyAnimation，等 ~QObject 按
    // 子对象顺序去收尾时容易踩到「动画回调打到已析构控件」的时序问题
    // （2026-10-17：截图探针偶发 exit=139 就是这条路径）。
    if (m_transition) {
        m_transition->hide();
        m_transition->setParent(nullptr);
        delete m_transition;
        m_transition = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  动态背景（对齐 C# DynamicBackground + ApplyDynamicBackground）
//  · 环境色晕常驻 —— 玻璃的账算在背后这层，没有壁纸时半透明面板也要有衬底；
//  · 壁纸：AppSettings.backgroundImagePath，resolveBackgroundPath 自愈解析，
//    backgroundOpacity / backgroundBlur / backgroundFit / backgroundAnimation 全消费；
//  · KenBurns：轻量正弦漂移（40ms 定时器整体平移，画面无跳变）。
// ─────────────────────────────────────────────────────────────────────────────

void MainShell::paintEvent(QPaintEvent* event)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // 🔴 壁纸必须走平滑缩放：视频帧是「按设备像素解、再按逻辑矩形贴」，
    //    只要窗口尺寸和帧尺寸不是 1:1 就要重采样；KenBurns 漂移还会产生
    //    小数偏移。不开这个 hint，Qt 光栅引擎一律最近邻 —— 屏上就是块状/发虚。
    //    （2026-10-17 用户报「自定义背景的画面清晰度不对」的两条原因之一。）
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // 🔴 按脏区裁剪：子控件（状态栏跑马灯等）重绘时只会把**自己那一小块**
    //    标脏，整窗重绘整张壁纸是纯浪费 —— 1280×820 的底色 + 壁纸 + 圆角
    //    clip 一趟下来是毫秒级，每秒几十次就是肉眼可见的卡顿
    //    （2026-10-30「动画会有卡顿」的成因之一）。
    //    裁到 event->rect() 之后，子控件那点重绘就只补它自己底下那一块。
    if (!event->rect().isEmpty() && event->rect() != rect()) {
        p.setClipRect(event->rect());
    }

    // 圆角必须和 `QFrame#shellRoot` 的 border-radius **完全一致**（两份 QSS 都是 30px）。
    // 🔴 这里曾是 16px，而 QSS 已经改成 30px —— 壁纸在 r=16 处就被裁掉，
    //    描边却在 r=30 处，两者之间留出一圈「空缝」，屏上看就是四角有一道
    //    白色月牙、像双层圆角（2026-10-17 用户截图指的就是这个）。
    //    改任何一边都必须同步改另一边。
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect()), kShellCornerRadius, kShellCornerRadius);
    p.setClipPath(clip);

    // 1+2) 底色 + 环境色晕：一次 drawPixmap 代替「整窗 fillRect + 四团覆盖整窗的
    //      径向渐变」。这两层只跟尺寸和主题有关，跟 KenBurns 相位无关 —— 缓起来。
    if (m_bgBase.size() != size()) rebuildBgBase();
    if (!m_bgBase.isNull()) {
        p.drawPixmap(0, 0, m_bgBase);
    } else {
        p.fillRect(rect(), ThemeManager::instance().color(ColorToken::Bg));
    }

    // 3) 壁纸层。
    if (m_bgIsVideo && !m_bgSource.isNull() && !m_bgVideoRect.isEmpty()) {
        // 视频：直接 drawImage 到 cover 目标矩形。光栅引擎一趟就把 1888×1078 的
        // 解码帧缩到窗口，**不**经过 m_bgFrame —— 原来每帧 fromImage + scaled
        // 要白搬两次全窗像素，30fps 下就是 200%+ CPU 的主要来源（2026-10-17 实测）。
        p.setOpacity(qBound(0.0, m_bgOpacity, 1.0));
        p.drawImage(m_bgVideoRect, m_bgSource);
        p.setOpacity(1.0);
    } else if (!m_bgFrame.isNull()) {
        p.setOpacity(qBound(0.0, m_bgOpacity, 1.0));
        QPointF offset;
        if (m_bgAnim) {
            // KenBurns 慢漂移：幅度 ~26px，两轴不同频率避免「画圈感」。
            offset = QPointF(26.0 * std::sin(m_bgAnimT * 0.50),
                             18.0 * std::cos(m_bgAnimT * 0.37));
        }
        p.drawPixmap(offset, m_bgFrame);
        p.setOpacity(1.0);
    }
}

void MainShell::rebuildBgBase()
{
    if (size().isEmpty()) return;
    // 先在 QImage 上画完再转 QPixmap：环境光场那一步要逐像素抬亮度，
    // QPixmap 的像素读写每趟都可能触发一次「上传 / 回读」，纯 CPU 光栅下比
    // QImage 慢一个数量级；最后一次性转成 QPixmap 缓存。
    QImage canvas(size(), QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull()) return;
    canvas.fill(ThemeManager::instance().color(ColorToken::Bg));
    {
        QPainter pb(&canvas);
        pb.setRenderHint(QPainter::Antialiasing, true);
        drawAmbientBloom(pb, rect());
    }
    // 🔴 光场必须**最后**叠：上面那四团覆盖整窗、半径 0.62×长边，先叠的话会被
    //    它们再抹平一遍，等于白做。这一层是液态玻璃背板的「可折射物」，
    //    见 addAmbientLightField 开头的说明。
    addAmbientLightField(canvas, m_darkTheme);
    m_bgBase = QPixmap::fromImage(canvas);
}

void MainShell::drawAmbientBloom(QPainter& painter, const QRect& area) const
{
    const bool dark = m_darkTheme;
    struct Blob {
        QPointF rel;        // 相对位置（0-1）
        QColor color;
    };
    const QColor blue  = dark ? QColor(0x2f, 0x6f, 0xd0) : QColor(0x86, 0xbb, 0xf2);
    const QColor teal  = dark ? QColor(0x1f, 0x9e, 0x8e) : QColor(0x93, 0xdd, 0xcf);
    const QColor violet= dark ? QColor(0x7a, 0x5f, 0xc8) : QColor(0xc3, 0xb6, 0xee);
    const QColor amber = dark ? QColor(0xb0, 0x7a, 0x30) : QColor(0xee, 0xd2, 0xa2);
    const int alpha = dark ? 42 : 70;
    const Blob blobs[] = {
        { QPointF(0.16, 0.20), blue },
        { QPointF(0.86, 0.14), teal },
        { QPointF(0.22, 0.84), violet },
        { QPointF(0.82, 0.82), amber },
    };
    const double radius = 0.62 * std::max(area.width(), area.height());
    for (const Blob& b : blobs) {
        QRadialGradient g(b.rel.x() * area.width(), b.rel.y() * area.height(), radius);
        QColor c = b.color;
        c.setAlpha(alpha);
        g.setColorAt(0.0, c);
        c.setAlpha(0);
        g.setColorAt(1.0, c);
        painter.fillRect(area, g);
    }
}

void MainShell::scheduleGlassBackdrops()
{
    if (m_dockBackdropQueued) return;
    m_dockBackdropQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_dockBackdropQueued = false;
        updateGlassBackdrops();
    });
}

QImage MainShell::composeBackdrop(const QRect& logical) const
{
    if (logical.isEmpty()) return QImage();
    // 按**逻辑尺寸**合成（不乘 dpr）：这块图马上要被磨砂 + 折射，dpr 精度
    // 毫无意义，而乘 dpr 会让 GlassSurface 再缩一次，白搬一趟像素。
    QImage canvas(logical.size(), QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull()) return canvas;
    canvas.fill(Qt::transparent);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.translate(-logical.topLeft());   // 窗口坐标 -> canvas 坐标

    if (!m_bgBase.isNull()) p.drawPixmap(0, 0, m_bgBase);

    // 视频壁纸必须单独处理：这条路上 m_bgFrame 是**空的**（视频帧从不进
    // m_bgFrame，见 rebuildBgFrame 的注释）。只画底色晕的话，浅色主题下
    // 玻璃底下的背板是「浅灰底 + 色晕」，而实际壁纸是深色视频 ——
    // 玻璃立刻变成一块白板，和周围完全脱节（2026-10-24 实测）。
    if (m_bgIsVideo && !m_bgSource.isNull() && !m_bgVideoRect.isEmpty()) {
        p.setOpacity(qBound(0.0, m_bgOpacity, 1.0));
        p.drawImage(m_bgVideoRect, m_bgSource);
        p.setOpacity(1.0);
    } else if (!m_bgFrame.isNull()) {
        p.setOpacity(qBound(0.0, m_bgOpacity, 1.0));
        QPointF offset;
        if (m_bgAnim) {
            offset = QPointF(26.0 * std::sin(m_bgAnimT * 0.50),
                             18.0 * std::cos(m_bgAnimT * 0.37));
        }
        p.drawPixmap(offset, m_bgFrame);
        p.setOpacity(1.0);
    }
    return canvas;
}

QImage MainShell::backdropForWidget(QWidget* w) const
{
    if (!w) return QImage();
    // 🔴 顶层控件的 geometry() 是**屏幕坐标**，必须先 map 回主壳本地坐标，
    //    再喂给 composeBackdrop（那一层按窗口坐标合成）。
    const QRect g = w->geometry();
    if (g.isEmpty()) return QImage();
    const QRect local(mapFromGlobal(g.topLeft()), g.size());
    return composeBackdrop(local);
}

// 主屏上所有液态玻璃面板的背板刷新（Dock + 顶栏两个 NavPill）。
//
// 🔴 三件事必须同时成立，否则这块玻璃会「假」：
//   ① **不能 grab 窗口**：快照里含玻璃自己和所有控件，磨砂之后是一块灰糊
//      —— 这是假玻璃最典型的翻车方式。只能从壁纸层重新合成（composeBackdrop）。
//   ② **只在主屏可见时算**：QStackedWidget 是 StackOne 模式，非当前页的控件
//      几何全是过期的（停在 640x480），裁出来的背板会整体错位。
//   ③ **与 paintEvent 同口径**：底色晕 -> 壁纸（backgroundOpacity + KenBurns
//      偏移），漏一层就会比周围亮一档，玻璃看起来像贴了张白纸。
//
// 成本：Dock 500x86 + 两个胶囊各 ~200x36，合成 + 磨砂 + 折射合计约 5~8ms。
// 只在「壁纸变 / 尺寸变 / 回主屏 / 改玻璃参数」时跑一次 —— 绝不在 KenBurns
// 每一帧里跑。
void MainShell::updateGlassBackdrops()
{
    // 底色晕可能还没烘（首次 show 之前）：补一次，否则背板缺一层、整体偏亮。
    if (m_bgBase.size() != size()) rebuildBgBase();

    // ① 灵动岛：任何页面都可能展开，**不能**被下面的主屏守卫挡住。
    //    只在 glass 档才需要背板（另外两档是纯色实心）。
    if (m_island && m_island->isVisible()
        && m_island->options().theme == QLatin1String("glass")
        && m_island->width() >= 8 && m_island->height() >= 8) {
        const QRect logical(m_island->mapTo(this, QPoint(0, 0)), m_island->size());
        m_island->setBackdrop(composeBackdrop(logical));
    }

    // ② 主屏专属（Dock + 顶栏胶囊）：非当前页的控件几何是过期的，必须挡住。
    if (!m_stack || m_stack->currentIndex() != kConsoleStackIndex) return;

    // ① 主屏 Dock
    if (m_consolePage) {
        QWidget* dock = m_consolePage->dockWidget();
        if (dock && dock->width() >= 8 && dock->height() >= 8) {
            m_consolePage->refreshDockGlass();   // 设置页改了参数不用另走通路
            const QRect logical(dock->mapTo(this, QPoint(0, 0)), dock->size());
            m_consolePage->setDockBackdrop(composeBackdrop(logical));
        }
        // 主屏小组件条（2026-10-29）：与 Dock 同一套液态玻璃，背板同样要从
        // 壁纸层裁 —— 它自己 grab 窗口会把底下的图标一起抓进去，磨砂出来一块灰糊。
        if (auto* widgets = m_consolePage->widgetsBar()) {
            if (widgets->isVisible() && widgets->width() >= 8 && widgets->height() >= 8) {
                const QRect logical(widgets->mapTo(this, QPoint(0, 0)), widgets->size());
                widgets->setBackdrop(composeBackdrop(logical));
            }
        }
    }

    // ② 顶栏两个 NavPill（只在主屏态染色，页面态是实色条、不需要背板）
    for (QWidget* w : {m_brandGroup, m_rightPill}) {
        auto* pill = static_cast<NavPill*>(w);   // 实际类型就是 NavPill
        if (!pill || !pill->isVisible() || !pill->tinted()) continue;
        const QRect logical(pill->mapTo(this, QPoint(0, 0)), pill->size());
        pill->setBackdrop(composeBackdrop(logical));
    }
}

void MainShell::rebuildBgFrame()
{
    if (m_bgSource.isNull()) {
        m_bgFrame = QPixmap();
        m_bgVideoRect = QRectF();
        return;
    }
    const QSize win = size();
    if (win.isEmpty()) return;

    // 🔴 一切缩放都按**设备像素**算，最后再把 dpr 挂回 QPixmap。
    //    只按逻辑尺寸做的话，125% 缩放的屏上整张壁纸会被再放大 1.25 倍
    //    （最近邻）—— 屏上看就是「自定义背景不清晰」。本机实测：窗口逻辑
    //    1024×656、devicePixelRatio 1.25、抓图 1280×820。
    const qreal dpr = qMax(1.0, devicePixelRatioF());
    const QSize winPx(qRound(win.width() * dpr), qRound(win.height() * dpr));

    // ── 视频壁纸：只算落点，不做任何像素搬运 ─────────────────────────────
    //  ① 视频一律按 cover 铺满，**无视** backgroundFit：contain 那条路每帧要
    //     放大裁切 + 26px 模糊衬底 + 4MB 画布 + 两次全窗 drawImage，30fps 下
    //     单核直接跑满（2026-10-17 实测 261% CPU 的元凶）。视频壁纸留黑边也没
    //     意义 —— 它不是「一张照片」，是「一整块会动的背景」。
    //  ② 落点算一次就够，帧本身留在 m_bgSource 里由 paintEvent 直接画。
    if (m_bgIsVideo) {
        m_bgFrame = QPixmap();
        const QSize src = m_bgSource.size();
        if (src.isEmpty()) {
            m_bgVideoRect = QRectF();
            return;
        }
        // cover：取较大的缩放比，多出来的那一维居中裁掉。
        // 🔴 帧是按设备像素解的，先折回**逻辑**尺寸再算 —— 直接拿像素宽算
        //    cover 会让整幅画面缩到 1/dpr（125% 下就是「画面变小、四周露底」）。
        const double srcW = src.width() / dpr;
        const double srcH = src.height() / dpr;
        const double s = qMax(win.width() / srcW, win.height() / srcH);
        const double w = srcW * s;
        const double h = srcH * s;
        m_bgVideoRect = QRectF((win.width() - w) / 2.0, (win.height() - h) / 2.0, w, h);
        return;
    }
    m_bgVideoRect = QRectF();

    const QString fit = m_bgFit;
    // 口径对齐 WPF Stretch：Uniform=contain / UniformToFill=cover / Fill=拉伸。
    // 设置页里存的值就是这套（实测 settings.json BackgroundFit="Uniform"）。
    Qt::AspectRatioMode mode = Qt::KeepAspectRatioByExpanding;   // 默认 cover
    double scaleUp = 1.0;
    if (fit == QStringLiteral("Uniform")) {
        mode = Qt::KeepAspectRatio;
    } else if (fit == QStringLiteral("Fill")) {
        mode = Qt::IgnoreAspectRatio;
    } else if (fit == QStringLiteral("UniformToFill")) {
        mode = Qt::KeepAspectRatioByExpanding;
        scaleUp = m_bgAnim ? 1.08 : 1.0;   // KenBurns 漂移时多留余量防止露边
    } else {
        scaleUp = m_bgAnim ? 1.08 : 1.0;
    }
    // 静态图只缩一次，Smooth 的成本可以忽略（视频那条路根本不走这里）。
    const QSize target(qRound(winPx.width() * scaleUp),
                       qRound(winPx.height() * scaleUp));
    QImage scaled = m_bgSource.scaled(target, mode, Qt::SmoothTransformation);
    if (mode == Qt::KeepAspectRatio && scaled.size() != target) {
        // contain（设置里的 Uniform）：四周必然空出来，透明画布会直接露出底下的
        // 环境色晕，看起来就是「背景没铺满 / 适配不好」。
        // 改相册 App 的标准处理：同一张图放大裁切 + 重度模糊当底，压暗一层，
        // 再把原比例的图居中贴上去 —— 留白变成有意为之的衬底，而不是空洞。
        QImage canvas(winPx, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        QPainter pa(&canvas);
        pa.setRenderHint(QPainter::SmoothPixmapTransform, true);
        // 画布是设备像素的，这里把画笔缩回逻辑坐标 —— 下面的尺寸全用 win 算，
        // 和改动前逐行对应，只多了一次 scale。
        pa.scale(dpr, dpr);

        QImage fill = m_bgSource.scaled(winPx, Qt::KeepAspectRatioByExpanding,
                                        Qt::FastTransformation);
        fill = cheapBlur(fill, 26.0 * dpr);   // 缩到 1/N 再放大，2ms 级
        pa.drawImage(QPointF((win.width() - fill.width() / dpr) / 2.0,
                             (win.height() - fill.height() / dpr) / 2.0), fill);
        pa.fillRect(QRectF(0, 0, win.width(), win.height()), QColor(0, 0, 0, 92));
        pa.drawImage(QPointF((win.width() - scaled.width() / dpr) / 2.0,
                             (win.height() - scaled.height() / dpr) / 2.0), scaled);
        QPixmap pm = QPixmap::fromImage(canvas);
        pm.setDevicePixelRatio(dpr);
        m_bgFrame = pm;
        return;
    }
    QPixmap pm = QPixmap::fromImage(scaled);
    pm.setDevicePixelRatio(dpr);   // 设备像素建的图，按 dpr 折回逻辑尺寸贴
    m_bgFrame = pm;
}

void MainShell::reloadBackground()
{
    kc::AppSettings& s = kc::AppSettings::current();
    const QString path = s.resolveBackgroundPath();   // 自愈解析（改名/移动后仍可用）
    m_bgOpacity = qBound(0.0, s.backgroundOpacity, 1.0);
    m_bgFit = s.backgroundFit;
    m_bgBlur = s.backgroundBlur;
    m_bgAnim = !path.isEmpty() && s.backgroundAnimation != QStringLiteral("None");

    // ⓪ 视频壁纸：走 Media Foundation 解码（Qt6::Multimedia 在静态单文件包里
    //    没有 FFmpeg 后端，见 kc/media/VideoBackground.h 的说明）。
    //    打开成功就直接返回 —— 视频有自己的逐帧推送，不走下面的静态图链路。
    if (!path.isEmpty() && kc::media::VideoBackground::looksLikeVideo(path)) {
        if (!m_bgVideo) {
            m_bgVideo = new kc::media::VideoBackground(this);
            connect(m_bgVideo, &kc::media::VideoBackground::frameReady, this,
                    [this](const QImage& frame) {
                        // 解码器交出来的已经是 ARGB32_Premultiplied（见
                        // VideoBackground.cpp 里关于 alpha 字节的说明），直接可用。
                        // 首帧到达时补一次 Dock 玻璃背板：reloadBackground 末尾那次
                        // schedule 跑在解码之前，那时 m_bgSource 还是空的。
                        const bool firstFrame = m_bgVideoAwaitingFirstFrame;
                        m_bgVideoAwaitingFirstFrame = false;
                        m_bgSource = (m_bgBlur > 0.5) ? cheapBlur(frame, m_bgBlur) : frame;
                        ++m_bgSourceSeq;
                        rebuildBgFrame();
                        // 🔴 不能每帧都重算：一次合成 + 磨砂 + 折射是 5ms 级，
                        //    24fps 下就是 12% 的核。视频画面明暗变化通常是几秒级，
                        //    但玻璃**背板**不止有明暗：镜头横摇时整块画面在平移，
                        //    1.9s（原 45 帧）一刷，Dock 边缘的折射带明显落后于
                        //    画面（2026-10-24 用户口径「玻璃没跟背景一起动」）。
                        //    24 帧 ≈ 1s：约 6ms/s 的开销（<1% 单核），跟得上又便宜。
                        if (firstFrame || m_bgSourceSeq - m_dockBackdropSeq >= 24) {
                            m_dockBackdropSeq = m_bgSourceSeq;
                            scheduleGlassBackdrops();
                        }
                        // 转场动画期间**不**整窗重绘：叠层是窗口的子控件，父控件
                        // update() 会连带把它一起重绘，30fps 的视频帧叠上去就是肉眼
                        // 可见的掉帧。转场只有 360ms，冻住壁纸反而更顺。
                        if (m_transition && m_transition->isVisible()) return;
                        update();
                    });
        }
        // 解码尺寸贴着窗口的**设备像素**走：原来是窗口逻辑边长打七五折，
        // 125% 缩放的屏上等于只解到 0.6× 再放大 —— 画面必然发虚（2026-10-17
        // 用户报「自定义背景的画面清晰度不对」）。现在按 1:1 解，paintEvent
        // 里就是一次不缩放的 blit，反而省掉了放大那一趟。
        // 上限 1920 给 4K 屏兜底，下限 720 保证小窗口也不糊。
        const qreal dpr = qMax(1.0, devicePixelRatioF());
        const int longEdgePx = qRound(qMax(width(), height()) * dpr);
        m_bgVideo->setMaxDimension(qBound(720, longEdgePx, 1920));
        QString why;
        if (m_bgVideo->open(path, &why)) {
            m_bgIsVideo = true;
            m_bgVideoAwaitingFirstFrame = true;   // 首帧到了要重裁 Dock 玻璃背板
            // 视频自己就在动，KenBurns 漂移叠上去只会互相打架。
            if (m_bgAnimTimer) m_bgAnimTimer->stop();
            m_bgAnim = false;
            update();
            return;
        }
        qWarning("背景：视频 %s 打不开（%s），回落到静态图",
                 qPrintable(path), qPrintable(why));
    }
    if (m_bgVideo) m_bgVideo->close();
    m_bgIsVideo = false;
    m_bgVideoAwaitingFirstFrame = false;

    // ① 直接按图片解（png/bmp 天然可用；jpg/gif 要 imageformats 插件，见 CMakeLists
    //    的 qt_import_plugins —— 静态包里漏了它就会「选了图但没背景」）。
    QImage img;
    if (!path.isEmpty()) img.load(path);

    // ② 解不出来再退一步：Wallpaper Engine 的视频壁纸（mp4/webm）本客户端放不了，
    //    直接 load 必然失败、而且**静默** —— 用户看到的就是「自定义背景失效」。
    //    同一张壁纸目录里一定有官方预览素材（preview.gif/jpg/png），拿它当静态背景，
    //    至少让背景出得来；否则明确写日志，不再让失败无声无息。
    if (img.isNull() && !path.isEmpty()) {
        const QString preview = kc::WallpaperEngineService::previewFor(path);
        if (!preview.isEmpty() && img.load(preview)) {
            qWarning("背景：%s 无法直接解码（视频壁纸？），已降级为预览素材 %s",
                     qPrintable(path), qPrintable(preview));
        } else {
            qWarning("背景：%s 无法解码，且找不到可用的预览素材 —— 背景将被跳过",
                     qPrintable(path));
        }
    }

    if (!img.isNull() && s.backgroundBlur > 0.5)
        img = blurImage(img, s.backgroundBlur);
    m_bgSource = img;
    rebuildBgFrame();

    if (m_bgAnim && !m_bgSource.isNull()) {
        if (!m_bgAnimTimer) {
            m_bgAnimTimer = new QTimer(this);
            connect(m_bgAnimTimer, &QTimer::timeout, this, [this] {
                // 🔴 慢漂移不需要 25fps：幅度只有 26px、周期十几秒，8fps 下每帧
                //    位移还不到 1px，肉眼分不出来。而每帧要整窗重绘（底色 + 4 团
                //    径向色晕 + 壁纸 + 所有半透明子控件），40ms 一帧直接把一个核
                //    跑满（实测空闲 130%+）。降到 100ms 后同一路径的 CPU 降到个位数。
                m_bgAnimT += 0.1;
                // 🔴 转场期间跳过：叠层铺的是**壁纸快照**，KenBurns 在这 400ms 里
                //    只漂 0.26px，肉眼完全看不出来；但这一次 update 是整窗重绘，
                //    会和转场动画抢同一个核 —— 4 次抢帧就是 4 次肉眼可见的顿挫
                //    （2026-10-17「切换界面会卡顿」的一半原因在这里）。
                if (!isVisible() || isMinimized()
                    || (m_transition && m_transition->isVisible()))
                    return;
                update();

                // 玻璃背板跟着漂移**节流**刷新（约 900ms 一次）。
                // 🔴 这条以前是缺的：壁纸在漂，玻璃底下却是定格的一帧 —— Dock
                //    上沿那条顶光相对壁纸内容一直在滑，近距离看就是「玻璃是张
                //    贴纸，没跟着背景走」。但绝不能每 tick 都刷：一次合成 +
                //    磨砂 + 折射是 5~8ms，10fps 下就是 5~8% 的核，而 0.1s 只漂
                //    0.1px。9 tick ≈ 900ms ≈ 1.2px 漂移，是「跟得上又几乎白送」
                //    的折中（节流计数在 m_bgAnimTick 里）。
                if (++m_bgAnimTick >= 9) {
                    m_bgAnimTick = 0;
                    scheduleGlassBackdrops();
                }
            });
        }
        m_bgAnimTimer->start(100);
    } else if (m_bgAnimTimer) {
        m_bgAnimTimer->stop();
    }
    // 壁纸换了 → Dock 玻璃底下的画面也换了。
    scheduleGlassBackdrops();
    update();
}

void MainShell::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);
    // 转场进行中窗口被拉伸：叠层跟着页面栈走（快照内容不变，只换覆盖区）。
    if (m_transition && m_transition->isVisible())
        m_transition->setGeometry(m_stack->geometry());
    // 压暗层跟着整个 central 走（连顶栏、状态栏一起压，避免亮度台阶）。
    if (m_islandScrim && m_central) m_islandScrim->setGeometry(m_central->rect());
    // 灵动岛贴顶居中（它不进布局，位置全靠这里维护）。
    repositionIsland();
    // iOS 玩法（2026-10-29）：这三样同样不进布局，尺寸一变要自己跟。
    if (m_assistiveTouch) m_assistiveTouch->repositionToParent();
    if (m_standby && m_central) m_standby->setGeometry(m_central->rect());
    rebuildBgFrame();
    rebuildBgBase();
    invalidateConsoleShot();   // 尺寸变了，旧快照的像素尺寸已不对
    scheduleGlassBackdrops();    // Dock 位置变了，玻璃背板要重新裁
    update();
}

void MainShell::repositionIsland()
{
    if (!m_island || !m_central) return;
    m_island->reposition();
}

void MainShell::buildUi()
{
    pageTimingMark("buildUi 开始");
    auto* central = new QFrame(this);
    central->setObjectName(QStringLiteral("shellRoot"));
    m_central = central;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(1, 1, 1, 1);
    root->setSpacing(0);

    // 主列：顶栏（返回 / 品牌 + 主题钮 + 窗口钮）→ 页面栈 → 状态栏。
    // 顶栏正中**刻意留空** —— 那一格是悬浮灵动岛的地盘（见本函数末尾）。
    auto* mainColumn = new QWidget(central);
    m_mainColumn = mainColumn;
    auto* col = new QVBoxLayout(mainColumn);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);

    col->addWidget(buildTopBar());

    m_stack = new QStackedWidget(mainColumn);
    m_stack->setObjectName(QStringLiteral("pageStack"));
    col->addWidget(m_stack, 1);

    col->addWidget(buildStatusBar());

    root->addWidget(mainColumn, 1);

    // 3D 转场叠层：盖在页面栈上（叠层本身不接鼠标，动画期间新页照常可点）。
    // 动画总开关关掉时干脆不创建 —— 转场期间每帧两张快照的切片投影白烧性能。
    if (Motion::animationsEnabled()) {
        m_transition = new PageTransition(mainColumn);
        m_transition->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_transition->hide();
    }

    // 灵动岛展开时的背景压暗层：盖满整个 central（连顶栏、状态栏一起）。
    // 🔴 只盖页面栈会在顶栏下沿留一道**横贯整窗的亮度台阶** —— 上半截还是原色、
    //    下半截已压暗，比不压暗更难看（2026-10-18 截图实测）。整窗压暗才读得出
    //    「临时浮层」的意思；代价是面板展开时顶栏的窗口钮要先点一下收起面板，
    //    这对一个瞬间就会关掉的切换器来说可以接受。
    m_islandScrim = new IslandScrim(central);
    m_islandScrim->setGeometry(central->rect());

    // 灵动岛：挂在 shellRoot 上而不是 mainColumn —— 它展开后要盖住整块内容区
    // （含顶栏），是「系统级」浮层。不进任何布局，位置由 repositionIsland 维护；
    // 后创建 + raise() 保证它压在顶栏与页面栈之上。
    m_island = new DynamicIsland(central);
    m_island->raise();
    // 展开态 = 页面切换器：点卡片直接跳页（2026-10-18 用户口径「灵动岛下来
    // 改为切换页面的控制台」）。收起由这里决定 —— 岛自己不收，因为外壳才知道
    // 「点完之后该不该把岛留下」。
    connect(m_island, &DynamicIsland::pageRequested, this,
            &MainShell::onIslandPageRequested);
    // 快捷动作条（截图 / 主屏 / 控制中心 / 设置）。收起面板由外壳决定 ——
    // 截图要等面板收回去才抓，否则截出来的是一张开着切换器的画面。
    connect(m_island, &DynamicIsland::actionTriggered, this,
            &MainShell::onIslandAction);
    // 搜索框：输入即过滤卡片表（外壳重建后 setPages 回来）。
    connect(m_island, &DynamicIsland::searchChanged, this,
            [this](const QString&) { rebuildIslandPages(); });
    // 折叠态左右拖动切页。
    connect(m_island, &DynamicIsland::pageStepRequested, this,
            &MainShell::stepIslandPage);
    // 暗层浓度跟岛同一条补间。压暗稍微滞后一点（0.15 之后才开始起）——
    // 胶囊刚一动就整屏变暗会显得「窗口闪了一下」。
    connect(m_island, &DynamicIsland::expansionChanged, this, [this](qreal t) {
        if (!m_islandScrim) return;
        if (m_central) m_islandScrim->setGeometry(m_central->rect());  // 布局刚变过也能对上
        // 「展开时压暗」关掉时暗层恒为 0（setLevel(0) 会自己 hide()，零开销）。
        m_islandScrim->setLevel(m_islandDim ? qBound(0.0, (t - 0.15) / 0.85, 1.0) : 0.0);

        // 岛的液态玻璃背板：只在**动画落定**时重裁一次（0 或 1）。
        // 逐帧重裁要 8ms/帧，340ms 的展开动画直接掉成幻灯片；而玻璃本来就是
        // 模糊的，中途用「目标尺寸」的背板拉伸一下肉眼看不出差别。
        if (t <= 0.001 || t >= 0.999) scheduleGlassBackdrops();
    });

    setCentralWidget(central);

    // 外观 / 行为（配色、圆角、折叠态内容、显示方式、列数、动效精简）从设置里读。
    // 必须放在 setCentralWidget 之后：applyIslandSettings() 里会调 reposition()，
    // 那一步要读父控件宽度 —— 还没挂上 central 的话居中会算成 0。
    applyIslandSettings();
}

void MainShell::onIslandPageRequested(const QString& key)
{
    if (m_island) m_island->setExpanded(false);
    navigateTo(key);
}

// ─────────────────────────────────────────────────────────────────────────────
//  灵动岛：外观 / 行为 / 可玩性（2026-10-18 用户口径「可玩性和个性化都太少」）
// ─────────────────────────────────────────────────────────────────────────────

void MainShell::applyIslandSettings()
{
    if (!m_island) return;

    const kc::AppSettings& s = kc::AppSettings::current();
    IslandOptions o;
    o.theme = s.islandTheme.trimmed().isEmpty() ? QStringLiteral("ink") : s.islandTheme;
    o.radius = qBound(10, s.islandRadius, 40);
    o.collapsed = s.islandCollapsed.trimmed().isEmpty() ? QStringLiteral("auto")
                                                        : s.islandCollapsed;
    o.visibility = s.islandVisibility.trimmed().isEmpty() ? QStringLiteral("always")
                                                          : s.islandVisibility;
    o.dim = s.islandDim;
    o.columns = (s.islandColumns == 4 || s.islandColumns == 8) ? s.islandColumns : 6;
    o.liteMotion = s.islandLiteMotion;

    m_islandDim = o.dim;
    m_island->setOptions(o);

    // 显示方式：hidden 时整枚岛不出现（收起面板 + 撤掉暗层，避免留一块点不掉的
    // 压暗层盖在界面上）。always / hover 都可见 —— 两者只在折叠态尺寸上不同。
    const bool hidden = (o.visibility == QLatin1String("hidden"));
    if (hidden) {
        if (m_island->isExpanded()) m_island->setExpanded(false);
        if (m_islandScrim) m_islandScrim->setLevel(0.0);
    }
    m_island->setVisible(!hidden);

    // 列数变了 → 行数变 → 一屏装得下的卡片数也变，页表要按容量重算一次。
    rebuildIslandPages();
    repositionIsland();
    // 配色从 ink / accent 切到 glass（或反过来）时，背板要么得补要么可以丢掉 ——
    // 一律重算一次，代价是 8ms，只在设置变更时发生。
    scheduleGlassBackdrops();
}

// ─────────────────────────────────────────────────────────────────────────────
//  iOS 玩法六项（2026-10-29）
//
//  用户口径「最近苹果新的系统加了好多好玩的，你加进工具里」。这一整块是**纯 UI**
//  能力，与刷机逻辑零耦合，所以集中在这里，不散进各个页面：
//    · 辅助触控悬浮球（AssistiveTouch）—— 任何页面都够得着的入口；
//    · 待机模式（StandByOverlay）—— 整屏大时钟 + 设备读数；
//    · 专注模式「刷机模式」—— 只留刷机页；
//    · 主屏小组件（ConsoleWidgets）—— 见 ConsolePage；
//    · 快捷指令页（ShortcutsPage）—— 见 registerPages。
// ─────────────────────────────────────────────────────────────────────────────

void MainShell::applyIosFeatures()
{
    const kc::AppSettings& s = kc::AppSettings::current();

    // ① 辅助触控悬浮球：设置里打开才创建（懒建，关掉只是 hide，不销毁 ——
    //    反复开关是常见操作，重建会丢位置与动画状态）。
    if (s.assistiveTouch) {
        ensureAssistiveTouch();
        if (m_assistiveTouch) {
            m_assistiveTouch->repositionToParent();
            m_assistiveTouch->setVisible(!m_focusFlash && !(m_standby && m_standby->isVisible()));
        }
    } else if (m_assistiveTouch) {
        m_assistiveTouch->hide();
    }

    // ③ 主屏小组件：构造时按设置决定建不建，这里补上「当场生效」那条路。
    if (m_consolePage) m_consolePage->setWidgetsVisible(s.consoleWidgets);

    // ④ 快捷指令页：同样当场生效（不注册的话只能重启才看得见）。
    refreshFocusFilter();
    if (!s.shortcutsPageVisible && m_shortcutsPage && m_stack
        && m_stack->currentWidget() == m_shortcutsPage) {
        // 正停在被关掉的那一页上：把人送回主屏，否则用户会盯着一个「已关闭」的页面。
        showConsole();
    }
    if (m_island) rebuildIslandPages();
    updateConsoleEntries();

    // ⑤ 待机模式自动进入：0 = 从不。秒数变了要重起计时器。
    if (m_standbyTimer) {
        m_standbyTimer->stop();
        if (s.standbyIdleSeconds > 0) {
            m_standbyTimer->start(qMax(10, s.standbyIdleSeconds) * 1000);
        }
    }

    // ⑤ 专注模式：设置里改了要立刻反映到主屏与页面表。
    setFocusFlashMode(s.focusFlashMode);
}

void MainShell::ensureAssistiveTouch()
{
    if (m_assistiveTouch || !m_central) return;
    m_assistiveTouch = new AssistiveTouch(m_central);
    // 悬浮球要压在页面栈之上（任何页面都看得见），但在灵动岛 / 待机层之下。
    m_assistiveTouch->raise();
    if (m_island) m_island->raise();
    if (m_standby && m_standby->isVisible()) m_standby->raise();
    connect(m_assistiveTouch, &AssistiveTouch::actionTriggered, this,
            &MainShell::handleQuickAction);
    // 拖动过 = 用户在场，待机计时重新起算。
    connect(m_assistiveTouch, &AssistiveTouch::dragged, this, &MainShell::noteUserActivity);
    m_assistiveTouch->repositionToParent();
}

void MainShell::ensureStandbyOverlay()
{
    if (m_standby || !m_central) return;
    m_standby = new StandByOverlay(m_central);
    m_standby->setGeometry(m_central->rect());
    connect(m_standby, &StandByOverlay::dismissed, this, [this] {
        noteUserActivity();
        if (m_assistiveTouch) m_assistiveTouch->setVisible(!m_focusFlash);
    });
}

void MainShell::enterStandby()
{
    ensureStandbyOverlay();
    if (!m_standby) return;
    const ShellStatus::Snapshot snap = ShellStatus::instance().snapshot();
    m_standby->setReadings(snap.device, snap.mode, snap.link, snap.task,
                           snap.busy ? qRound(snap.percent) : -1);
    m_standby->setGeometry(m_central->rect());
    m_standby->raise();
    // 悬浮球在待机层下应该收掉 —— 待机是「熄灯」，上面还飘个点很怪。
    if (m_assistiveTouch) m_assistiveTouch->hide();
    m_standby->showOverlay();
    m_standby->setFocus();
}

void MainShell::leaveStandby()
{
    if (!m_standby) return;
    m_standby->hideOverlay();
    if (m_assistiveTouch) m_assistiveTouch->setVisible(!m_focusFlash);
    noteUserActivity();
}

void MainShell::noteUserActivity()
{
    if (!m_standbyTimer) return;
    const int seconds = kc::AppSettings::current().standbyIdleSeconds;
    if (seconds <= 0) {
        m_standbyTimer->stop();
        return;
    }
    m_standbyTimer->start(qMax(10, seconds) * 1000);
}

void MainShell::handleQuickAction(const QString& id)
{
    if (id == QLatin1String("console")) {
        showConsole();
    } else if (id == QLatin1String("control")) {
        openControlCenter();
    } else if (id == QLatin1String("shortcuts")) {
        if (m_shortcutsPage) navigateTo(QStringLiteral("Shortcuts"));

    } else if (id == QLatin1String("history")) {
        // 刷机记录窗归 EDL 页（数据在 FlashHistory，但入口在「高级功能 ▾」）。
        // 先跳 EDL 页再直接调它的公开方法 —— 走 QMetaObject 反射在 Qt6 下
        // 对「无参无返回」的字符串调用没有合适重载（编译期就报错）。
        navigateTo(QStringLiteral("EdlFlash"));
        if (auto* edl = findChild<EdlFlashPage*>()) {
            if (auto* mw = edl->edlWindow()) mw->showFlashHistory();
        }
    } else if (id == QLatin1String("standby")) {
        enterStandby();
    } else if (id == QLatin1String("focus")) {
        setFocusFlashMode(!m_focusFlash);
    } else if (id == QLatin1String("screenshot")) {
        onIslandAction(QStringLiteral("screenshot"));
    }
}

void MainShell::setFocusFlashMode(bool on)
{
    if (m_focusFlash == on) return;
    m_focusFlash = on;
    kc::AppSettings& s = kc::AppSettings::current();
    s.focusFlashMode = on;
    // 🔴 从悬浮球 / 控制中心切进来时也要落盘：设置页那条路会 save()，
    //    这条路以前只改内存 —— 结果「关掉程序再打开，专注模式自己没了」，
    //    而设置页里的开关还停在开着的位置（读的是同一份内存）。
    s.save();

    refreshFocusFilter();
    // 专注模式下把浮动入口收掉（悬浮球会挡刷机进度），退出时按设置恢复。
    if (m_assistiveTouch) {
        m_assistiveTouch->setVisible(!on && kc::AppSettings::current().assistiveTouch
                                     && !(m_standby && m_standby->isVisible()));
    }
    if (on) {
        // 进专注模式：停在控制台会让人不知道发生了什么，直接落到 EDL 页。
        showConsole();

    }
    updateConsoleEntries();
    if (m_island) rebuildIslandPages();
}

void MainShell::refreshFocusFilter()
{
    // 专注模式的过滤口径：只留「刷机 + 设备」两组（这两组才是刷机时真正会用到的），
    // 设置 / 关于 / 命令行 / 快捷指令这些先收起 —— 它们的入口随时能退出来再进。
    // 实现方式是把不相关页面标成 hidden（与「可视刷写」同一个机制），
    // 于是主屏网格、灵动岛切换器、控制台快照缓存全部自动跟着变。
    for (PageEntry& entry : m_pages) {
        if (entry.key == QLatin1String("ControlCenter")) continue;   // 隐藏页本来就不上主屏
        if (entry.key == QLatin1String("VisualFlash")) continue;     // 同理
        const bool flashGroup = entry.group == QLatin1String("flash")
                                || entry.group == QLatin1String("device");
        entry.hidden = m_focusFlash ? !flashGroup : false;
        // 快捷指令页 / 控制中心这类本来就有 hidden 语义的页面不能在这里被解开，
        // 所以只对「非系统页」恢复；系统页（控制中心）始终保持隐藏。
        if (!m_focusFlash && entry.key == QLatin1String("ControlCenter")) entry.hidden = true;
        // 设置里关掉的快捷指令页：退出专注模式也**不许**被恢复出来，
        // 否则「关掉开关 → 进一次专注模式 → 退出」它自己又冒回主屏了。
        if (entry.key == QLatin1String("Shortcuts")
            && !kc::AppSettings::current().shortcutsPageVisible) {
            entry.hidden = true;
        }
    }
    invalidateConsoleShot();
}

void MainShell::rebuildIslandPages()
{
    if (!m_island) return;

    // 主屏排第一（它是默认页），其余按页面表顺序；隐藏页不上切换器 ——
    // 但控制中心是个例外中的例外：它是「左滑那一页」，用户切到那儿之后
    // 岛里也该看得见它，否则折叠态的 page 档会显示一个岛上找不到的名字。
    QVector<IslandPage> all;
    all.reserve(m_pages.size() + 2);
    all.append({ QStringLiteral("console"), QStringLiteral("主屏") });
    for (const PageEntry& e : m_pages) {
        if (e.hidden) continue;
        all.append({ e.key, e.title });
    }
    if (m_controlPageIndex >= 0) {
        const PageEntry& c = m_pages.at(m_controlPageIndex);
        all.append({ c.key, c.title });
    }

    const QString query = m_island->searchText().trimmed();
    if (query.isEmpty()) {
        m_island->setPages(all);
        return;
    }

    QVector<IslandPage> hits;
    hits.reserve(all.size());
    for (const IslandPage& p : all) {
        // 名字和 key 都匹配：用户可能记的是「EDL」而不是「Qualcomm EDL」。
        if (p.title.contains(query, Qt::CaseInsensitive)
            || p.key.contains(query, Qt::CaseInsensitive)) {
            hits.append(p);
        }
    }
    m_island->setPages(hits);
    // 一条都没命中时给一句人话，否则展开的面板里只剩一个空白网格。
    if (hits.isEmpty()) {
        m_island->flashMessage(QStringLiteral("没有匹配的页面"), 1400);
    }
}

void MainShell::stepIslandPage(int delta)
{
    if (!m_stack || delta == 0) return;

    // 序列与岛里的卡片表同源：主屏 → 各可见页 → 控制中心。到头绕回，
    // 拖动切页本来就是个环，撞墙停下反而像卡住了。
    QVector<QString> keys;
    keys.reserve(m_pages.size() + 2);
    keys.append(QStringLiteral("console"));
    for (const PageEntry& e : m_pages) {
        if (e.hidden && e.key != QLatin1String("ControlCenter")) continue;
        keys.append(e.key);
    }
    if (keys.size() < 2) return;

    const QString currentKey = (m_stack->currentIndex() == kConsoleStackIndex)
                                   ? QStringLiteral("console")
                                   : m_pages.at(m_stack->currentIndex() - 1).key;
    int index = keys.indexOf(currentKey);
    if (index < 0) index = 0;

    const int n = keys.size();
    const int next = ((index + delta) % n + n) % n;
    navigateTo(keys.at(next));
}

void MainShell::onIslandAction(const QString& id)
{
    if (!m_island) return;

    // 先收面板：四个动作里有三个是「跳到别处」，留着面板会盖住刚落地的页面。
    const bool wasExpanded = m_island->isExpanded();
    if (wasExpanded) m_island->setExpanded(false);

    if (id == QLatin1String("screenshot")) {
        // 整窗截图进剪贴板。抓图要等面板收回去 —— 展开中的画面不是用户想要的。
        // 340ms 是收起补间的时长（见 DynamicIsland::setExpanded），多留 60ms 余量。
        QPointer<MainShell> guard(this);
        QTimer::singleShot(wasExpanded ? 400 : 0, this, [guard] {
            if (!guard) return;
            const QPixmap shot = guard->grab();
            if (shot.isNull()) {
                if (guard->m_island) {
                    guard->m_island->flashMessage(QStringLiteral("截图失败"), 1600);
                }
                return;
            }
            QApplication::clipboard()->setPixmap(shot);
            if (guard->m_island) {
                guard->m_island->flashMessage(QStringLiteral("截图已复制到剪贴板"), 2000);
            }
        });
        return;
    }

    if (id == QLatin1String("console")) {
        showConsole();
        return;
    }
    if (id == QLatin1String("control")) {
        openControlCenter();
        return;
    }
    if (id == QLatin1String("settings")) {
        navigateTo(QStringLiteral("Settings"));
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  控制中心
// ─────────────────────────────────────────────────────────────────────────────

void MainShell::openControlCenter()
{
    // 控制中心现在是**页面栈里的一页**（2026-10-18），不再是浮在内容上的下拉
    // 抽屉：走正常的 navigateTo，于是转场、返回钮、状态栏、灵动岛高亮全部自动生效。
    if (m_controlPageIndex < 0) return;
    showPage(m_controlPageIndex);
}

// ─────────────────────────────────────────────────────────────────────────────
//  横滑手势：主屏 ⇄ 控制中心（跟手）
//
//  为什么是「跟手」而不是「滑一下再播放转场」：用户要的是「向左滑是控制中心」，
//  这跟 iOS 的翻页一样，手指到哪页面就到哪 —— 松手前就能看见落点，才敢放心划。
//  实现上复用 PageTransition 的 Fluid 几何，只是进度由这里逐帧 drive()，
//  松手再 settle() 到 0 或 1。
// ─────────────────────────────────────────────────────────────────────────────

int MainShell::swipeTargetForCurrentPage() const
{
    if (!m_stack || !m_central) return -2;
    if (m_transition && m_transition->running()) return -2;   // 转场中不接手势
    const int current = m_stack->currentIndex();
    if (current == kConsoleStackIndex) {
        // 主屏只有右边一个邻居：控制中心。
        return m_controlPageIndex >= 0 ? m_controlPageIndex : -2;
    }
    if (current == m_controlPageIndex + 1) {
        // 控制中心只有左边一个邻居：主屏（用 -1 表示「目标是主屏」）。
        return -1;
    }
    return -2;
}

void MainShell::beginSwipe(bool forward)
{
    if (!m_stack) {
        m_swipeArmed = false;
        return;
    }
    if (!m_transition) {
        // 动画总开关关掉时没有叠层可跟手 —— 直接标记「已激活」，松手按位移判定跳页。
        m_swipeActive = true;
        m_swipeProgress = 0.0;
        return;
    }

    const int current = m_stack->currentIndex();
    const QPixmap fromPix = (current == kConsoleStackIndex) ? consoleSnapshot()
                                                            : m_stack->grab();
    QPixmap toPix;
    if (m_swipeTargetPage >= 0) {
        toPix = grabPageInStack(m_swipeTargetPage);
    } else {
        toPix = consoleSnapshot();
    }
    if (fromPix.isNull() || toPix.isNull()) {
        m_swipeArmed = false;
        return;
    }

    m_transition->setGeometry(m_stack->geometry());
    m_transition->begin(fromPix, toPix, PageTransition::Mode::Fluid, forward,
                        ThemeManager::instance().color(ColorToken::Bg), QRect(),
                        stackBackdrop());
    m_swipeActive = true;
    m_swipeProgress = 0.0;
}

void MainShell::previewSwipe(qreal progress)
{
    if (!m_stack || !m_transition || m_controlPageIndex < 0) return;
    if (m_stack->currentIndex() != kConsoleStackIndex) return;
    m_swipeTargetPage = m_controlPageIndex;
    m_swipeForward = true;
    beginSwipe(true);
    if (m_swipeActive) m_transition->drive(qBound(0.0, progress, 1.0));
}

void MainShell::endSwipe(){
    m_swipeArmed = false;
    if (!m_swipeActive) return;
    m_swipeActive = false;

    // 落定判据：拖过 26% 就算「想过去」；或者往目标方向甩得够快（一次短促的
    // 快划），只要越过 6% 也认 —— 手指快的人不会慢慢拖到四分之一屏。
    const qreal toward = m_swipeForward ? -m_swipeVelocity : m_swipeVelocity;
    const bool commit = (m_swipeProgress > 0.26)
                        || (toward > 620.0 && m_swipeProgress > 0.06);

    if (!commit) {
        if (m_transition) m_transition->settle(0.0, [] {});
        return;
    }

    // 🔴 先切栈、再补完：叠层是不透明的，提前切在视觉上完全看不出来；
    //    反过来（先补完再切）收尾那一帧会露出还没切走的旧页，闪一下。
    m_swipeSwitching = true;
    if (m_swipeTargetPage >= 0) showPage(m_swipeTargetPage);
    else showConsole();
    m_swipeSwitching = false;

    if (m_transition) m_transition->settle(1.0, [] {});
}

void MainShell::applyControlAction(const QString& action)
{
    if (action == QLatin1String("theme")) {
        applyTheme(!m_darkTheme);
        if (m_controlPage) m_controlPage->syncTiles();
        return;
    }
    if (action == QLatin1String("glass")) {
        // 「液态玻璃」开 = 卡片更透（磨砂感更强）；关 = 实心卡片。
        kc::AppSettings& s = kc::AppSettings::current();
        s.cardOpacity = (s.cardOpacity < 0.70) ? 0.92 : 0.52;
        s.save();
        ThemeManager::instance().refreshStyleSheet();
        if (m_controlPage) m_controlPage->syncTiles();
        return;
    }
    if (action == QLatin1String("blur")) {
        kc::AppSettings& s = kc::AppSettings::current();
        s.backgroundBlur = (s.backgroundBlur > 0.5) ? 0.0 : 24.0;
        s.save();
        reloadBackground();
        if (m_controlPage) m_controlPage->syncTiles();
        return;
    }
    if (action == QLatin1String("anim")) {
        kc::AppSettings& s = kc::AppSettings::current();
        s.backgroundAnimation = (s.backgroundAnimation == QStringLiteral("None"))
                                    ? QStringLiteral("KenBurns")
                                    : QStringLiteral("None");
        s.save();
        reloadBackground();
        if (m_controlPage) m_controlPage->syncTiles();
        return;
    }
    if (action == QLatin1String("autoport")) {
        QSettings qs;
        const bool on = qs.value(QStringLiteral("connection/autoDetect9008"), true).toBool();
        qs.setValue(QStringLiteral("connection/autoDetect9008"), !on);
        if (m_controlPage) m_controlPage->syncTiles();
        return;
    }
    if (action == QLatin1String("debuglog")) {
        LogView::setGlobalDebugVisible(!LogView::globalDebugVisible());
        if (m_controlPage) m_controlPage->syncTiles();
        return;
    }
    // 「update」动作（检查更新）已随更新功能移除（停更离线版）。
    if (action == QLatin1String("reset")) {
        // 「恢复外观」：把四个滑条 / 开关拉回出厂值，背景重载一次。
        kc::AppSettings& s = kc::AppSettings::current();
        s.cardOpacity = 0.86;
        s.backgroundOpacity = 0.35;
        s.backgroundBlur = 24.0;
        s.backgroundAnimation = QStringLiteral("KenBurns");
        s.backgroundFit = QStringLiteral("Fill");
        s.uiTransparency = 1.0;
        s.save();
        ThemeManager::instance().refreshStyleSheet();
        reloadBackground();
        if (m_controlPage) m_controlPage->syncTiles();
        return;
    }
    // 滑杆：拖动过程中只应用不落盘，松手（*-commit）才写 settings.json。
    auto splitSlider = [](const QString& a, const char* prefix, int* out) {
        const QString p = QString::fromLatin1(prefix);
        if (!a.startsWith(p)) return false;
        bool ok = false;
        const int v = a.mid(p.size()).toInt(&ok);
        if (!ok) return false;
        *out = v;
        return true;
    };
    int value = 0;
    if (splitSlider(action, "card:", &value) || splitSlider(action, "card-commit:", &value)) {
        kc::AppSettings& s = kc::AppSettings::current();
        s.cardOpacity = qBound(0, value, 100) / 100.0;
        if (action.startsWith(QLatin1String("card-commit:"))) s.save();
        ThemeManager::instance().refreshStyleSheet();
        return;
    }
    if (splitSlider(action, "bg:", &value) || splitSlider(action, "bg-commit:", &value)) {
        kc::AppSettings& s = kc::AppSettings::current();
        s.backgroundOpacity = qBound(0, value, 100) / 100.0;
        if (action.startsWith(QLatin1String("bg-commit:"))) s.save();
        m_bgOpacity = s.backgroundOpacity;
        update();
        return;
    }
    if (action.startsWith(QLatin1String("nav:"))) {
        navigateTo(action.mid(4));
        return;
    }
}

QWidget* MainShell::buildTopBar()
{
    auto* titleBar = new TopBarChrome(this);
    // 🔴 对象名不能叫 titleBar —— 内嵌 EDL 旧窗自己的标题栏就叫那个名字
    //    （ui/MainWindow.cpp），eventFilter 的 findChild 会抓错人。
    titleBar->setObjectName(QStringLiteral("shellTopBar"));
    titleBar->setFixedHeight(48);
    m_topBar = titleBar;
    auto* layout = new QHBoxLayout(titleBar);
    layout->setContentsMargins(12, 0, 8, 0);
    layout->setSpacing(10);

    // ── 左：返回钮（页面态）/ 品牌组（控制台态）+ 当前页名 ────────────────
    // 2026-10-17 控制台重构：原来的「页标题 + 6 页签条」整条撤掉。页签即分组的
    // 两级模型被「主屏图标 → 页面」替代，顶栏只留一个返回入口与当前页名。
    m_backButton = new BackChevronButton(titleBar);
    m_backButton->setObjectName(QStringLiteral("shellBack"));
    m_backButton->hide();   // 控制台态不显示
    connect(m_backButton, &QPushButton::clicked, this, &MainShell::showConsole);
    layout->addWidget(m_backButton);

    // 控制台态的品牌组：logo + 名称 + 版本，三件并成一组。
    // 原来左上「KunClient」、右上「by 小坤ovo · v2.0.1」—— 一行里自报两次家门，
    // 而且分居两端，视线得来回跳。现在品牌只在左端出现一次，版本紧贴名字。
    m_brandGroup = new NavPill(titleBar);
    m_brandGroup->setObjectName(QStringLiteral("shellBrandGroup"));
    m_brandGroup->setFixedHeight(34);
    auto* brandRow = new QHBoxLayout(m_brandGroup);
    brandRow->setContentsMargins(13, 0, 15, 0);
    brandRow->setSpacing(8);
    auto* brandLogo = new QLabel(m_brandGroup);
    brandLogo->setObjectName(QStringLiteral("shellBrandLogo"));
    brandLogo->setPixmap(QPixmap(QStringLiteral(":/icons/logo.png"))
                             .scaled(22, 22, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    brandLogo->setFixedSize(22, 22);
    brandRow->addWidget(brandLogo);
    auto* brandName = new QLabel(QStringLiteral("KunClient"), m_brandGroup);
    brandName->setObjectName(QStringLiteral("shellBrandName"));
    brandRow->addWidget(brandName);
    auto* brandVer = new QLabel(
        QStringLiteral("v%1").arg(QString::fromLatin1(kc::kVersionString)), m_brandGroup);
    brandVer->setObjectName(QStringLiteral("shellBrandVersion"));
    brandRow->addWidget(brandVer);
    layout->addWidget(m_brandGroup);

    // 页名：两层 QLabel 叠在同一个网格格子里（QGridLayout 的 (0,0) 会互相覆盖），
    // 切换页面时旧名在下面淡出、新名在上面淡入 —— 这是整条转场里唯一一处
    // 「顶栏内容硬跳」的地方，改成交叉淡化后页名和页面是同时到位的。
    // 🔴 不用 reveal/revealX：它们内部 move()，而这里的位置由布局管，
    //    布局下一次重排就会把位置推回去（位移被无声吃掉，还闪一下）。
    m_titleHost = new QWidget(titleBar);
    m_titleHost->setObjectName(QStringLiteral("pageTitleHost"));
    auto* titleStack = new QGridLayout(m_titleHost);
    titleStack->setContentsMargins(0, 0, 0, 0);
    titleStack->setSpacing(0);

    m_pageTitlePrev = new QLabel(m_titleHost);
    // 与当前页名共用同一个 objectName：QSS 里只有 #pageTitle 一条规则，
    // 两层必须同字号同字重，否则交叉淡化会看到字「胖瘦」跳一下。
    m_pageTitlePrev->setObjectName(QStringLiteral("pageTitle"));
    m_pageTitlePrev->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_pageTitlePrev->hide();
    titleStack->addWidget(m_pageTitlePrev, 0, 0);

    m_pageTitleLabel = new QLabel(m_titleHost);
    m_pageTitleLabel->setObjectName(QStringLiteral("pageTitle"));
    m_pageTitleLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_pageTitleLabel->hide();   // 控制台态没有页名
    titleStack->addWidget(m_pageTitleLabel, 0, 0);   // 后加的在上层

    layout->addWidget(m_titleHost);

    layout->addStretch();

    // ── 右：品牌 meta + 主题钮 │ 窗口钮 ──────────────────────────────────
    // 顶栏正中留给灵动岛，所以右侧这一串只能靠右。分隔线把「应用内动作」
    // （主题）和「系统动作」（最小化/最大化/关闭）切开 —— iOS 工具栏也这么分组。
    // 右侧整组收进一枚胶囊：主屏压在壁纸上时它自带底，页面态不染色（顶栏
    // 本身已经是实色条）。主题钮与窗口钮之间那条竖线是「应用动作 │ 系统动作」。
    m_rightPill = new NavPill(titleBar);
    m_rightPill->setObjectName(QStringLiteral("shellRightGroup"));
    m_rightPill->setFixedHeight(34);
    auto* rightRow = new QHBoxLayout(m_rightPill);
    rightRow->setContentsMargins(8, 0, 5, 0);
    rightRow->setSpacing(6);

    m_brandMeta = new QLabel(m_rightPill);
    m_brandMeta->setObjectName(QStringLiteral("shellBrandMeta"));
    m_brandMeta->setMaximumWidth(kBrandMetaWidth);
    rightRow->addWidget(m_brandMeta);

    // ── 主题切换（设置页之外的第二入口；矢量太阳 / 月牙）──────────────────
    auto* themeToggle = new ThemeToggleButton(m_rightPill);
    themeToggle->setObjectName(QStringLiteral("shellThemeToggle"));
    themeToggle->setToolTip(QStringLiteral("切换主题"));
    connect(themeToggle, &QPushButton::clicked, this, [this] {
        ThemeManager::instance().setDark(!ThemeManager::instance().isDark());
    });
    m_themeToggle = themeToggle;
    rightRow->addWidget(themeToggle);

    auto* sep = new QFrame(m_rightPill);
    sep->setObjectName(QStringLiteral("shellSep"));
    sep->setFixedSize(1, 18);
    rightRow->addWidget(sep);

    // ── 窗口钮 ───────────────────────────────────────────────────────────
    // 2026-10-13：矢量绘制（WindowButton），去掉 —/▢/× 文本字形（用户判「不好看」）。
    auto* minimize = new WindowButton(WindowButton::Kind::Minimize, m_rightPill);
    auto* maximize = new WindowButton(
        isMaximized() ? WindowButton::Kind::Restore : WindowButton::Kind::Maximize, m_rightPill);
    auto* close = new WindowButton(WindowButton::Kind::Close, m_rightPill);
    connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
    connect(maximize, &QPushButton::clicked, this, [this, maximize] {
        if (isMaximized()) {
            showNormal();
            maximize->setKind(WindowButton::Kind::Maximize);
        } else {
            showMaximized();
            maximize->setKind(WindowButton::Kind::Restore);
        }
    });
    connect(close, &QPushButton::clicked, this, &QWidget::close);
    rightRow->addWidget(minimize);
    rightRow->addWidget(maximize);
    rightRow->addWidget(close);
    layout->addWidget(m_rightPill);

    // 顶栏拖动：直接用原生 startSystemMove（与既有 MainWindow 同款）。
    // 事件过滤器要装在顶栏**及其子控件**上，否则点标签 / 空白处收不到按下事件。
    titleBar->installEventFilter(this);
    for (QObject* child : titleBar->children()) child->installEventFilter(this);
    return titleBar;
}

QWidget* MainShell::buildStatusBar()
{
    // 对齐 C# MainWindow.xaml 的 ③+④：进度区（双进度条）+ 分段读数行。
    // 2026-10-16 增补：内嵌 EDL 窗底部三格（协议 / 连接状态 / 提示文案）并入本栏
    // —— 协议与连接状态各占一段，提示文案那行改成底部跑马灯带（见 updateMarquee）。
    auto* bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("shellStatusBar"));
    auto* outer = new QVBoxLayout(bar);
    // 🔴 左右 12 / 下 8：窗口四角是 30px 圆角，状态栏贴在最下沿 —— 边距给 6/4 时，
    //    跑马灯胶囊的左下角正好落进圆角弧里被切掉一块（2026-10-17 实测）。
    //    圆角弧在 y=814 处才收到 x=12，胶囊底边留 8px 就安全了。
    outer->setContentsMargins(12, 5, 12, 8);
    outer->setSpacing(4);

    // ③ 双进度条：总进度 + 当前分区进度（第二条默认显示，report(-1) 收起）。
    m_barTotal = new ToolProgressBar(bar);
    m_barTotal->setFixedHeight(12);
    m_barPart = new ToolProgressBar(bar);
    m_barPart->setFixedHeight(12);
    outer->addWidget(m_barTotal);
    outer->addWidget(m_barPart);

    // ④ 分段读数行。
    auto* row = new QWidget(bar);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(6, 0, 6, 0);
    layout->setSpacing(16);

    auto makeSegment = [&](const QString& label, QLabel** value) {
        auto* segment = new QWidget(row);
        auto* seg = new QHBoxLayout(segment);
        seg->setContentsMargins(0, 0, 0, 0);
        seg->setSpacing(4);
        auto* key = new QLabel(label, segment);
        key->setObjectName(QStringLiteral("statusLabel"));
        *value = new QLabel(QStringLiteral("-"), segment);
        (*value)->setObjectName(QStringLiteral("statusValue"));
        seg->addWidget(key);
        seg->addWidget(*value);
        return segment;
    };

    layout->addWidget(makeSegment(QStringLiteral("设备:"), &m_statusDevice));
    layout->addWidget(makeSegment(QStringLiteral("模式:"), &m_statusMode));
    layout->addWidget(makeSegment(QStringLiteral("任务:"), &m_statusTask));
    layout->addWidget(makeSegment(QStringLiteral("分区:"), &m_statusPartition));
    // EDL 链路两段（原内嵌 EDL 窗页脚）：协议通信状态 + 连接状态。
    layout->addWidget(makeSegment(QStringLiteral("协议:"), &m_statusProtocol));
    layout->addWidget(makeSegment(QStringLiteral("状态:"), &m_statusLink));
    layout->addStretch();
    layout->addWidget(makeSegment(QStringLiteral("速度:"), &m_statusSpeed));
    layout->addWidget(makeSegment(QStringLiteral("总量:"), &m_statusTotal));
    layout->addWidget(makeSegment(QStringLiteral("进度:"), &m_statusPercent));
    outer->addWidget(row);

    // ⑤ 跑马灯带：原 EDL 页脚那行「遇到长时间无响应…」的位置，改成滚动展示
    //    个性签名（有则）或人生哲理文案（无则轮播）。空闲/忙碌都常显 —— 它是
    //    状态栏的装饰层，不占读数槽位。
    auto* marqueeBox = new QFrame(bar);
    marqueeBox->setObjectName(QStringLiteral("marqueeBox"));
    marqueeBox->setFixedHeight(22);
    auto* marqueeLayout = new QHBoxLayout(marqueeBox);
    marqueeLayout->setContentsMargins(0, 0, 0, 0);
    marqueeLayout->setSpacing(0);
    m_marquee = new MarqueeLabel(marqueeBox);
    marqueeLayout->addWidget(m_marquee, 1);
    outer->addWidget(marqueeBox);

    // 哲理文案轮播：无签名时每 12 秒换一条（有签名时该定时器停着）。
    m_marqueeRotateTimer = new QTimer(this);
    m_marqueeRotateTimer->setInterval(12 * 1000);
    connect(m_marqueeRotateTimer, &QTimer::timeout, this, [this] {
        if (!m_marquee) return;
        if (!m_signature.trimmed().isEmpty()) return;   // 有签名就不轮播哲理
        ++m_marqueeQuoteIndex;
        updateMarquee();
    });
    updateMarquee();

    // ShellStatus 中枢 → 状态栏（跨线程自动排队）。
    connect(&ShellStatus::instance(), &ShellStatus::changed, this,
            &MainShell::syncShellStatus, Qt::QueuedConnection);
    syncShellStatus();   // 初始占位（"-"）
    return bar;
}

void MainShell::syncShellStatus()
{
    // 对齐 C# SyncShellStatus：读数 → 双进度条 + 分段文本。
    const ShellStatus::Snapshot snap = ShellStatus::instance().snapshot();

    // 进度条只在「真的有任务在跑」时占位 —— 2026-10-12 用户口径：空闲时底部那条
    // 0.00% 的空进度条太碍眼（他的原话「这里下面的进度条删除」）。空闲 = 整条收起，
    // 分段读数行（设备/模式/任务/进度…）照旧常显；任务开始（beginTask）时再出现。
    const bool showBars = snap.busy;
    if (m_barTotal) {
        m_barTotal->setVisible(showBars);
        m_barTotal->setValue(snap.percent);
    }
    if (m_barPart) {
        // 第二条只在「分区进度参与显示」时可见：单分区操作两条恒等，照搬会画成
        // 两条同步的进度条（看起来像 bug）。
        m_barPart->setVisible(showBars && snap.partitionVisible);
        m_barPart->setValue(snap.partitionPercent);
        m_barPart->setShowText(qAbs(snap.partitionPercent - snap.percent) > 0.5);
    }

    // 耗时不单独占槽位，并进「任务」段显示。
    const QString taskText = snap.elapsed.isEmpty()
                                 ? snap.task
                                 : QStringLiteral("%1  %2").arg(snap.task, snap.elapsed);
    if (m_statusTask) m_statusTask->setText(taskText);
    if (m_statusPartition) m_statusPartition->setText(snap.partition);
    if (m_statusSpeed) m_statusSpeed->setText(snap.speed);
    if (m_statusTotal) m_statusTotal->setText(snap.totalSize);
    if (m_statusPercent)
        m_statusPercent->setText(QStringLiteral("%1%").arg(snap.percent, 0, 'f', 0));
    if (m_statusDevice) m_statusDevice->setText(snap.device);
    if (m_statusMode) m_statusMode->setText(snap.mode);

    // 灵动岛：同一份快照喂过去（折叠态一句话、展开态五行 + 进度）。
    if (m_island) {
        IslandState is;
        is.device = snap.device;
        is.mode = snap.mode;
        is.task = snap.busy ? snap.task : QString();
        is.protocol = snap.protocol;
        is.link = snap.link;
        is.linkState = snap.linkState;
        is.percent = snap.busy ? qRound(snap.percent) : -1;
        m_island->setState(is);
    }

    // 控制中心的状态芯片：同一份快照。
    if (m_controlPage) {
        m_controlPage->setStatus(snap.device, snap.mode, snap.protocol, snap.link,
                                 snap.linkState);
    }

    // EDL 链路两段：文本 + linkState 属性（QSS 属性选择器给「状态:」着色，改属性后
    // 必须重新 polish 才生效 —— 与页签 active 同款坑）。
    if (m_statusProtocol) m_statusProtocol->setText(snap.protocol);
    if (m_statusLink) {
        m_statusLink->setText(snap.link);
        if (m_statusLink->property("linkState").toString() != snap.linkState) {
            m_statusLink->setProperty("linkState", snap.linkState);
            m_statusLink->style()->unpolish(m_statusLink);
            m_statusLink->style()->polish(m_statusLink);
        }
    }
}

// ── 底部跑马灯带（2026-10-16）──────────────────────────────────────────────
//
//  内容优先级：① 用户「个性签名」（/api/v2/profile 的 signature）→ 滚动展示；
//  ② 无签名 → 轮播一组人生哲理文案（每 12s 换一条，见 buildStatusBar 的定时器）。
//  原先这里放的是 EDL 页脚那行「遇到长时间无响应时，请耐心等待，避免重复操作。」，
//  用户要求改成跑马灯并补文案 —— 那句提示改挂到本带的 tooltip，不丢。
void MainShell::updateMarquee()
{
    if (!m_marquee) return;

    // 2026-10-17 用户要求：别写刷机相关的「哲理」，要真正的人生道理。
    // 口径：一句话、不训人、不喊口号、不说教，读完能让人松一口气而不是被教育。
    static const QStringList kQuotes = {
        QStringLiteral("慢一点没关系，方向对了就不算绕路。"),
        QStringLiteral("你不需要赢过谁，只需要比昨天清楚一点。"),
        QStringLiteral("情绪上头时做的决定，往往要花更久去收拾。"),
        QStringLiteral("看不清全貌的时候，先把眼前这一步走稳。"),
        QStringLiteral("把注意力放在能改变的事上，其余的交给时间。"),
        QStringLiteral("允许自己慢，但别允许自己糊弄。"),
        QStringLiteral("所有捷径，最后都要补票。"),
        QStringLiteral("不必事事争先，先把手上的事做完。"),
        QStringLiteral("真正的耐心不是等待，而是等待的时候不慌。"),
        QStringLiteral("留三分余地给别人，也留三分余地给自己。"),
    };

    const QString signature = m_signature.trimmed();
    if (!signature.isEmpty()) {
        m_marquee->setText(signature);
        m_marquee->setToolTip(QStringLiteral("个性签名：%1").arg(signature));
        if (m_marqueeRotateTimer) m_marqueeRotateTimer->stop();
        return;
    }

    if (kQuotes.isEmpty()) {
        m_marquee->setText(QString());
        return;
    }
    m_marqueeQuoteIndex = ((m_marqueeQuoteIndex % kQuotes.size()) + kQuotes.size()) % kQuotes.size();
    m_marquee->setText(kQuotes.at(m_marqueeQuoteIndex));
    m_marquee->setToolTip(QStringLiteral(
        "遇到长时间无响应时，请耐心等待，避免重复操作。\n"
        "在「设置 · 个性签名」里填一句自己的话，这里会改滚你的签名。"));
    if (m_marqueeRotateTimer && !m_marqueeRotateTimer->isActive())
        m_marqueeRotateTimer->start();
}

void MainShell::registerPages()
{
    pageTimingMark("registerPages 开始");
    // 导航表**完全照抄** tests/src/KunClient.App/MainWindow.xaml.cs 的 Groups
    // （2026-10-03 重分类：按「是不是刷机」分成 6 / 7 / 2）。
    // 显示名也用 .NET 口径（EdlFlash 的页签名是 Qualcomm，key 不变）。
    // 2026-10-17 控制台重构：分组 tag（flash/device/advanced/cloud/settings）
    // 继续保留 —— 它现在只服务两件事：主屏 Dock 的取用，以及转场模式判定。

    // ── 控制台主屏（页面栈索引 0，不是功能页）────────────────────────────
    // 它不进 m_pages：那张表描述的是「可导航到的功能页」，主屏由 showConsole()
    // 直接切栈索引，也不需要标题 / 分组。
    m_consolePage = new ConsolePage(m_stack);
    connect(m_consolePage, &ConsolePage::pageRequested, this, &MainShell::navigateTo);
    m_stack->addWidget(m_consolePage);

    // ── 刷机（flash）────────────────────────────────────────────────────
    auto* edlFlash = new EdlFlashPage(this);
    addPage(QStringLiteral("EdlFlash"), QStringLiteral("Qualcomm"),
            QStringLiteral("flash"), edlFlash);
    // 注：EdlFlashPage::navigationRequested 曾用于「FDL ›」快捷入口跳转；2026-10-16
    // 该入口撤掉（FDL 改由 EDL 右下角重启列表触发完整流程），信号保留但暂无发送方。
    auto* fastbootFlash = new FastbootFlashPage(this);
    addPage(QStringLiteral("FastbootFlash"), QStringLiteral("FASTBOOT"),
            QStringLiteral("flash"), fastbootFlash);
    // FASTBOOT 页「可视刷写 ›」→ 跳转 VisualFlash 隐藏页。
    connect(fastbootFlash, &FastbootFlashPage::navigationRequested, this,
            [this](const QString& key) { navigateTo(key); });    addPage(QStringLiteral("Payload"), QStringLiteral("Payload"),
            QStringLiteral("flash"), new PayloadPage(this));
    addPage(QStringLiteral("OnePlusFlash"), QStringLiteral("欧加线刷"),
            QStringLiteral("flash"), new OnePlusFlashPage(this));
    addPage(QStringLiteral("XiaomiFastboot"), QStringLiteral("小米线刷"),
            QStringLiteral("flash"), new XiaomiFastbootPage(this));
    // MTK（联发科）刷写页：设备发现 / BROM 握手 / scatter 解析 / BROM 直写直读。
    // 挂「刷机」页签与 Qualcomm/FASTBOOT 并列；DA 高速通道为二期（见 MtkFlashPage 头注）。
    addPage(QStringLiteral("MtkFlash"), QStringLiteral("MTK"),
            QStringLiteral("flash"), new MtkFlashPage(this));
    // FDL（原「小米EDL」）：2026-10-16 用户要求撤掉独立页面 —— 流程本体
    // （kc::ui::XiaomiEdlPage）改由 EDL 窗右下角「重启设备 ▾」菜单的 FDL 项承载，
    // 打开一个弹窗并自动开跑完整流程（见 swt::edl::MainWindow::openFdlFlow）。
    // 因此这里不再 addPage("XiaomiEdl")；类本身仍编译进工程，供弹窗实例化。
    // 可视刷写：C# 最终形态并入 FASTBOOT 页；C++ 沿用隐藏页先例注册，
    // 由 FASTBOOT 页的「可视刷写 ›」按钮跳转。
    addPage(QStringLiteral("VisualFlash"), QStringLiteral("可视刷写"),
            QStringLiteral("flash"), new VisualFlashPage(this), /*hidden=*/true);

    // ── 设备（device）────────────────────────────────────────────────────
    // 主页「命令行 ›」快捷入口 → 跳转命令行页（对齐 C# BtnOpenConsole_Click）。
    auto* deviceHome = new DeviceHomePage(this);
    connect(deviceHome, &DeviceHomePage::navigationRequested, this,
            [this](const QString& key) { navigateTo(key); });
    addPage(QStringLiteral("DeviceHome"), QStringLiteral("主页"),
            QStringLiteral("device"), deviceHome);
    addPage(QStringLiteral("AndroidTools"), QStringLiteral("安卓工具"),
            QStringLiteral("device"), new AndroidToolsPage(this));
    addPage(QStringLiteral("ScreenMirror"), QStringLiteral("投屏"),
            QStringLiteral("device"), new ScreenMirrorPage(this));
    addPage(QStringLiteral("AppManager"), QStringLiteral("应用管理"),
            QStringLiteral("device"), new AppManagerPage(this));
    addPage(QStringLiteral("FileTransfer"), QStringLiteral("文件传输"),
            QStringLiteral("device"), new FileTransferPage(this));
    // 「小米字库牌子」2026-10-21 已从主屏移除，搬进 EDL 页「高级功能 ▾」
    // （用 EDL 页选中的串口与引导程序，日志与结果都进 EDL 日志）。
    // 独立页的 UI 与入口一并删除，逻辑留在 kc/devices/StorageBrandRunner。

    // ── 高级（advanced）──────────────────────────────────────────────────
    addPage(QStringLiteral("CommandConsole"), QStringLiteral("命令行"),
            QStringLiteral("advanced"), new CommandConsolePage(this));

    // ── 快捷指令（2026-10-29，iOS「快捷指令」的极简版）────────────────────
    // 挂在「高级」组：它编排的是刷机流程本身，不是某一台设备的操作。
    // 🔴 页面**总是**注册，只是按设置标 hidden：设置页的开关写着「改完立刻生效」，
    //    而「不注册」这条路只能重启才生效（快捷指令页还会连带从灵动岛切换器里消失，
    //    用户会以为页面被删了）。hidden 与「可视刷写」同一个机制，主屏网格与
    //    灵动岛切换器都自动跟着变（见 refreshFocusFilter）。
    {
        auto* shortcuts = new ShortcutsPage(this);
        // 页面自己碰不到设备（拿不到内嵌 EDL 窗），需要设备的动作统一转交外壳。
        // 这里先落一条通知 + 记一条通知历史：真正的设备动作要用户到对应页面确认
        // （自动发「连接设备」这种命令等于绕过所有确认框，不可接受）。
        connect(shortcuts, &ShortcutsPage::requestDeviceAction, this,
                [this](const QString& action, const QString& parameter) {
                    const QString detail = parameter.isEmpty()
                        ? QStringLiteral("已转交设备页执行")
                        : QStringLiteral("参数：%1").arg(parameter);
                    // 通知中心与 Toast 已移除：这条转交记录只进日志。
                    Q_UNUSED(detail);
                });
        addPage(QStringLiteral("Shortcuts"), QStringLiteral("快捷指令"),
                QStringLiteral("advanced"), shortcuts,
                /*hidden=*/!kc::AppSettings::current().shortcutsPageVisible);
        m_shortcutsPage = shortcuts;

    // ── 批量刷机队列（2026-10-31）──────────────────────────────────────────
    // 挂在「刷机」组：它编排的就是刷机流程本身。
    // 🔴 页面不碰设备，需要设备的动作一律 emit requestDeviceAction() 转交外壳
    //    （与快捷指令页同一条路子）。外壳这边只记日志 —— 真正的刷机要用户
    //    到 EDL 页确认，自动发「开始刷机」等于绕过所有确认框。
    {
        auto* batch = new BatchFlashPage(this);
        connect(batch, &BatchFlashPage::requestDeviceAction, this,
                [](const QString& action, const QString& parameter) {
                    // 🔴 外壳这里**不做任何事**：批量页的转交只是「告知」，
                    //    真正的刷机必须由用户到 EDL 页确认（自动发「开始刷机」
                    //    等于绕过所有确认框，不可接受）。
                    //    参数原样丢掉 —— 批量页自己会在日志里记「已转交」。
                    Q_UNUSED(action);
                    Q_UNUSED(parameter);
                });
        addPage(QStringLiteral("BatchFlash"), QStringLiteral("批量刷机"),
                QStringLiteral("flash"), batch);
        m_batchFlashPage = batch;
    }
    }

    // ── 云端（cloud）分组：云驱动页已随服务器功能移除（停更离线版）────────

    // ── 设置（settings，导航轨里沉底）────────────────────────────────────
    addPage(QStringLiteral("Settings"), QStringLiteral("设置"),
            QStringLiteral("settings"), new SettingsPage(this));
    addPage(QStringLiteral("About"), QStringLiteral("关于"),
            QStringLiteral("settings"), new AboutPage(this));

    // ── 控制中心（隐藏页）────────────────────────────────────────────────
    // 2026-10-18：从「顶部下拉的玻璃面板」改成**主屏左边的一页** —— 用户口径
    // 「这个页面向左滑是控制中心，默认显示为主页，然后把现在这个删掉」。
    // hidden = true：它不上主屏图标网格（否则「控制中心」会变成主屏上的一个 app，
    // 语义上它更像系统面板），但**要**进灵动岛的切换器，所以下面单独拼表。
    auto* controlPage = new ControlCenterPage(this);
    connect(controlPage, &ControlCenterPage::actionTriggered, this,
            &MainShell::applyControlAction);
    addPage(QStringLiteral("ControlCenter"), QStringLiteral("控制中心"),
            QStringLiteral("system"), controlPage, /*hidden=*/true);
    m_controlPage = controlPage;
    m_controlPageIndex = m_pages.size() - 1;

    // ── 灵动岛切换器的页面表 ─────────────────────────────────────────────
    // 主屏排第一（它是默认页），其余按页面表顺序。表由 rebuildIslandPages() 统一
    // 拼装 —— 搜索过滤、控制中心入列、容量按列数变化都只有那一处口径。
    rebuildIslandPages();
}

void MainShell::addPage(const QString& key, const QString& title, const QString& group,
                        BasePage* page, bool hidden)
{
    if (!page) return;

    // 页面构造耗时探针（KC_TIMING=1）。页面是 `addPage(..., new XxxPage(this))` 这样
    // 就地构造的，所以「距上一个标记的增量」≈ 这一页的构造开销。
    // 冷启动到主界面 3s 里到底是谁在吃时间，只有这个数字说得清。
    pageTimingMark(qPrintable(key));

    page->setTitle(title);
    m_stack->addWidget(page);

    PageEntry entry;
    entry.key = key;
    entry.title = title;
    entry.group = group;
    entry.page = page;
    entry.hidden = hidden;

    m_pages.append(entry);

    // 页面向上汇报设备 / 模式 → 外壳状态栏。
    connect(page, &BasePage::shellStatusChanged, this, &MainShell::onPageStatus);
}

// ─────────────────────────────────────────────────────────────────────────────
//  控制台主屏：图标网格 / Dock 与页面表对齐
// ─────────────────────────────────────────────────────────────────────────────

void MainShell::updateConsoleEntries()
{
    if (!m_consolePage) return;

    QVector<ConsolePage::Entry> entries;
    entries.reserve(m_pages.size());
    for (const PageEntry& e : m_pages) {
        if (e.hidden) continue;   // 「可视刷写」只走页内快捷入口，不上主屏
        entries.append({ e.key, e.title, e.group });
    }

    // Dock：钉 5 个最高频入口（顺序即显示顺序）。其余全部进主屏网格。
    static const QStringList kDockKeys = {
        QStringLiteral("DeviceHome"),      // 设备主页：看状态的第一站
        QStringLiteral("EdlFlash"),        // Qualcomm EDL
        QStringLiteral("FastbootFlash"),   // FASTBOOT
        QStringLiteral("MtkFlash"),        // MTK
        QStringLiteral("Settings"),        // 设置
    };
    m_consolePage->setEntries(entries, kDockKeys);
    invalidateConsoleShot();   // 入口列表变了，主屏内容跟着变
}

// ─────────────────────────────────────────────────────────────────────────────
//  控制台 ↔ 页面 导航
// ─────────────────────────────────────────────────────────────────────────────

void MainShell::showConsole()
{
    if (!m_stack || !m_consolePage) return;
    const int previous = m_stack->currentIndex();
    if (m_island) m_island->setCurrentKey(QStringLiteral("console"));
    if (previous == kConsoleStackIndex) {
        updatePageHeader(-1);   // 幂等：保证顶栏处于控制台态（隐藏返回钮、显示品牌名）
        return;
    }

    // 离开的若是功能页，走它的 onDeactivated（页面缓存常驻，切回来不会再走
    // showEvent 首次路径，生命周期只能靠外壳显式调）。
    if (previous >= 1 && previous - 1 < m_pages.size())
        m_pages.at(previous - 1).page->onDeactivated();

    // 横滑落定时不重播转场：叠层正在补完那一段，再起一段会把它打断。
    const bool animate = m_transition && Motion::animationsEnabled() && isVisible()
                         && !m_swipeSwitching;
    QPixmap fromPix;
    if (animate) fromPix = m_stack->grab();   // 离开的是功能页，内容随时会变，只能现抓

    m_stack->setCurrentIndex(kConsoleStackIndex);
    updatePageHeader(-1);

    if (animate) {
        // 回到主屏：主屏自上次离开后没变过，直接复用缓存 —— 一次转场里的
        // 两次全窗 grab 就这样省掉一次。缓存没命中才现抓（抓完顺手填上）。
        const QPixmap toPix = (m_consoleShotValid && !m_consoleShot.isNull())
                                  ? m_consoleShot
                                  : m_stack->grab();
        // 主屏此刻若有瓦片处于悬停放大态就不入缓存 —— 存下放大画面，
        // 下次回来转场会闪一下放大的图标。
        if (!m_consolePage || !m_consolePage->isMagnifying()) {
            m_consoleShot = toPix;
            m_consoleShotValid = true;
        }
        m_transition->setGeometry(m_stack->geometry());
        m_transition->start(fromPix, toPix, PageTransition::Mode::Fluid,
                            /*forward=*/false, ThemeManager::instance().color(ColorToken::Bg),
                            []() {}, QRect(), stackBackdrop());
    }

    // 回主屏：Dock 的玻璃背板要按当前壁纸重裁一次（离开期间壁纸可能换过）。
    scheduleGlassBackdrops();
}

QPixmap MainShell::consoleSnapshot()
{
    // 只有在主屏正是当前页时调用才准 —— 调用点都保证了这一点。
    if (!m_consoleShotValid || m_consoleShot.isNull()) {
        const QPixmap shot = m_stack->grab();
        // 光标正悬停在某个瓦片上时画面带着放大效果：这一张给本次转场用
        // （它本来就该和屏幕一致），但不入缓存。
        if (!m_consolePage || !m_consolePage->isMagnifying()) {
            m_consoleShot = shot;
            m_consoleShotValid = true;
        }
        return shot;
    }
    return m_consoleShot;
}

QPixmap MainShell::grabPageInStack(int pageIndex)
{
    if (!m_stack || pageIndex < 0 || pageIndex >= m_pages.size()) return {};
    const int target = pageIndex + 1;          // 栈索引 0 是控制台主屏
    const int keep = m_stack->currentIndex();
    if (keep == target) return m_stack->grab();

    // 页面栈的玻璃底由 QSS 的 [console="true"] 分支决定：抓图期间必须按
    // 「页面态」上色，否则抓出来的是一张透明底 —— 贴到转场衬底（壁纸）上，
    // 看起来就是「新页没铺满、只飘着几个磁贴」。
    auto setConsoleProp = [this](bool on) {
        if (m_stack->property("console").toBool() == on) return;
        m_stack->setProperty("console", on);
        m_stack->style()->unpolish(m_stack);
        m_stack->style()->polish(m_stack);
    };
    const bool keepConsole = m_stack->property("console").toBool();

    setConsoleProp(false);
    m_stack->setCurrentIndex(target);
    // 🔴 不能靠 `layout()->activate()`：布局此时是「干净的」（刚为栈本身算过），
    //    activate() 直接空转，隐藏页仍停在 640×480 —— 抓出来就是一张内容错位、
    //    右边被截掉的图（2026-10-18 定格帧实测）。直接按栈的矩形摆一次几何，
    //    顺带触发该页自己的布局（resize → QResizeEvent → QLayout 激活）。
    if (QWidget* page = m_stack->widget(target)) page->setGeometry(m_stack->rect());
    const QPixmap shot = m_stack->grab();
    m_stack->setCurrentIndex(keep);
    setConsoleProp(keepConsole);
    return shot;
}

QPixmap MainShell::stackBackdrop()
{
    // 只画「壁纸」这一层，不碰任何子控件 —— 给转场叠层当衬底用。
    // 与 paintEvent 的第 1~3 步逐行对应，改那边记得同步这边。
    if (!m_stack) return {};
    const QSize sz = m_stack->size();
    if (sz.isEmpty()) return {};

    const QPoint origin = m_stack->mapTo(this, QPoint(0, 0));
    if (m_bgBase.size() != size()) rebuildBgBase();

    QPixmap pm(sz);
    pm.fill(ThemeManager::instance().color(ColorToken::Bg));
    QPainter p(&pm);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // ① 底色 + 环境色晕：从缓存图上裁出页面栈那一块。
    if (!m_bgBase.isNull()) p.drawPixmap(QPoint(0, 0), m_bgBase, QRect(origin, sz));

    // ② 壁纸层（坐标要从窗口系平移到叠层系）。
    if (m_bgIsVideo && !m_bgSource.isNull() && !m_bgVideoRect.isEmpty()) {
        p.setOpacity(qBound(0.0, m_bgOpacity, 1.0));
        p.drawImage(m_bgVideoRect.translated(-origin), m_bgSource);
        p.setOpacity(1.0);
    } else if (!m_bgFrame.isNull()) {
        p.setOpacity(qBound(0.0, m_bgOpacity, 1.0));
        QPointF offset;
        if (m_bgAnim) {
            offset = QPointF(26.0 * std::sin(m_bgAnimT * 0.50),
                             18.0 * std::cos(m_bgAnimT * 0.37));
        }
        p.drawPixmap(offset - QPointF(origin), m_bgFrame);
        p.setOpacity(1.0);
    }
    return pm;
}

void MainShell::setActiveTab(const QString& tab, bool navigate)
{    // 页签条已随控制台重构撤掉。这里保留成兼容 shim，只为不破坏既有的
    // `--tab <key>` 命令行 / 探针调用：
    //   · "all" → 回控制台主屏；
    //   · 其余  → 落到该分组第一个可见页（分组 tag 仍是 flash/device/…）。
    if (!navigate) return;
    if (tab.isEmpty() || tab == QStringLiteral("all")) {
        showConsole();
        return;
    }
    for (int i = 0; i < m_pages.size(); ++i) {
        if (!m_pages.at(i).hidden && m_pages.at(i).group == tab) {
            showPage(i);
            return;
        }
    }
}

void MainShell::updatePageHeader(int pageIndex)
{
    const bool onConsole = (pageIndex < 0 || pageIndex >= m_pages.size());
    // 控制台态 = 品牌组（logo + 名称 + 版本）；页面态 = 返回钮 + 页名。
    // 两组互斥，同一时刻只出现一组 —— 顶栏左端永远只有一个视觉焦点。
    if (m_backButton) m_backButton->setVisible(!onConsole);
    if (m_brandGroup) {
        m_brandGroup->setVisible(onConsole);
        // 品牌组只在主屏出现，胶囊底也只在那儿要 —— 实际类型就是 NavPill。
        static_cast<NavPill*>(m_brandGroup)->setTinted(onConsole);
    }
    // 署名只在页面态出现：主屏左端已经是品牌组，右端再挂一条「by 小坤ovo」
    // 就是同一行报两次家门。页面态左端是返回钮 + 页名，右端才需要它。
    if (m_brandMeta) m_brandMeta->setVisible(!onConsole);
    if (m_rightPill) static_cast<NavPill*>(m_rightPill)->setTinted(onConsole);
    // 染色态变了 → 胶囊的液态玻璃背板要重裁（页面态不画胶囊，跳过）。
    // 走 schedule 而不是直接调：此刻 setVisible 刚改完，几何还没落定。
    if (onConsole) scheduleGlassBackdrops();
    if (m_pageTitleLabel) {
        const QString want = onConsole ? QString() : m_pages.at(pageIndex).title;
        // 页名不在转场快照里（它是顶栏的一部分），硬 setText 会让它在页面还在
        // 推入时「啪」地跳变 —— 整条转场里最扎眼的一处硬切就在这儿。
        // 新旧文字不同就走交叉淡化，页名和页面同时到位。
        const bool animate = Motion::animationsEnabled() && isVisible()
                             && m_pageTitleLabel->text() != want;
        if (animate) {
            crossFadePageTitle(want);
        } else {
            m_pageTitleLabel->setText(want);
            m_pageTitleLabel->setVisible(!onConsole);
            if (m_pageTitlePrev) m_pageTitlePrev->hide();
        }
    }

    // 顶栏底色分两态：主屏是「壁纸 + 渐隐蒙层」，页面是「实色玻璃条」。
    // m_topBar 的实际类型就是 TopBarChrome（buildTopBar 里 new 的），直接转。
    if (m_topBar) static_cast<TopBarChrome*>(m_topBar)->setConsoleMode(onConsole);

    // 内容区顶部的 20px 圆角只属于「页面」——主屏要露出壁纸，底色必须撤掉，
    // 否则壁纸上会压一层浅色圆角纸。属性 + re-polish 是 QSS 换分支的唯一办法。
    if (m_stack && m_stack->property("console").toBool() != onConsole) {
        m_stack->setProperty("console", onConsole);
        m_stack->style()->unpolish(m_stack);
        m_stack->style()->polish(m_stack);
    }

    // 灵动岛折叠态 collapsed == "page" 时显示的就是这行字。放在最后统一喂：
    // 顶栏页名与岛内页名必须同一时刻切换，否则转场里会看到两个不同的名字。
    if (m_island) {
        m_island->setCurrentTitle(onConsole ? QStringLiteral("主屏")
                                            : m_pages.at(pageIndex).title);
    }
}

void MainShell::crossFadePageTitle(const QString& text)
{
    if (!m_pageTitleLabel || !m_pageTitlePrev) return;
    const QString previous = m_pageTitleLabel->text();
    if (previous == text) return;

    // 旧名接手当前文字，作为压在下面的那一层；新名立刻换字，从 0 淡入。
    // 两层都可见的那 200ms 里，网格格子取两者宽度的较大值 —— 布局只会
    // 横向多出一点点（被后面的 addStretch 吃掉），不会把右半组推走。
    m_pageTitlePrev->setText(previous);
    m_pageTitlePrev->setVisible(!previous.isEmpty());

    if (text.isEmpty()) {
        // 回主屏：主屏没有页名 —— 新层直接收掉，让旧名独自淡出。
        m_pageTitleLabel->hide();
        m_pageTitleLabel->setText(QString());
        if (!previous.isEmpty()) Motion::fadeOut(m_pageTitlePrev, 150, /*hideAfter=*/true);
        return;
    }

    m_pageTitleLabel->setText(text);
    // 从主屏进页时旧名是空的：新名不必等，直接淡入（否则开头 50ms 顶栏是空的）。
    const int delay = previous.isEmpty() ? 0 : 45;
    if (!previous.isEmpty()) Motion::fadeOut(m_pageTitlePrev, 150, /*hideAfter=*/true);
    Motion::fadeIn(m_pageTitleLabel, 230, delay);
}

void MainShell::updateBrandMeta()
{
    if (!m_brandMeta) return;
    // 版本号已经并进左端品牌组，这里只留署名（+ 有则会员签名档）——
    // 一条顶栏里不重复报版本，右端这行越短越好。
    QString meta = QStringLiteral("by 小坤ovo");
    if (!m_signature.isEmpty()) meta += QStringLiteral(" · ") + m_signature;
    // 🔴 按**固定预算**省略，不能用 m_brandMeta->width()：QLabel 宽度会随上一轮
    //    省略后的文本缩水，用它当预算会让文案每刷新一次就短一截。
    m_brandMeta->setToolTip(meta);
    m_brandMeta->setText(
        QFontMetrics(m_brandMeta->font()).elidedText(meta, Qt::ElideRight, kBrandMetaWidth));
}

void MainShell::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !m_bgIsVideo) return;

    // 最小化后没人看得见壁纸 —— 别在后台白烧一个核解码。
    // 视频壁纸的解码是纯软件解码（MF 基础视频处理不缩放、也拿不到硬件解码器），
    // 1080p 30fps 常驻就是一整个核，最小化时停掉是白捡的收益。
    if (isMinimized()) {
        if (!m_bgVideoPaused) {
            m_bgVideoPaused = true;
            if (m_bgVideo) m_bgVideo->close();
        }
    } else if (m_bgVideoPaused) {
        m_bgVideoPaused = false;
        reloadBackground();   // 重新 open 一次（内部会重新探测尺寸）
    }
}

void MainShell::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (m_entrancePlayed) return;
    m_entrancePlayed = true;

    // 首次显示时布局才定型 —— 灵动岛此刻才能算准居中位置。
    repositionIsland();
    // Dock 的几何也是这一刻才定型的，玻璃背板要等布局摆完才能裁。
    scheduleGlassBackdrops();

    if (!Motion::animationsEnabled()) return;

    // 主窗口入场：淡入 + 上浮 16px（与弹窗同一族曲线，位移量刻意做小）。
    // 🔴 用 windowOpacity 而不是 QGraphicsOpacityEffect —— 本窗口开着
    //    WA_TranslucentBackground 且靠 paintEvent 画底色晕，挂 graphics effect
    //    会和自己的绘制打架（TopLevel + Translucent 下表现为整窗发黑）。
    const QPoint endPos = pos();
    const QPoint startPos(endPos.x(), endPos.y() + 16);
    setWindowOpacity(0.0);
    move(startPos);

    QPointer<MainShell> guard(this);
    auto finish = [guard, endPos]() {
        if (!guard) return;
        guard->setWindowOpacity(1.0);
        guard->move(endPos);
    };

    auto* fade = Motion::tween(this, "windowOpacity", 0.0, 1.0, 240, Motion::silkEase());
    auto* rise = new QPropertyAnimation(this, "pos", this);
    rise->setDuration(240);
    rise->setStartValue(startPos);
    rise->setEndValue(endPos);
    rise->setEasingCurve(Motion::silkEase());

    QObject::connect(fade, &QAbstractAnimation::finished, this, finish);
    QTimer::singleShot(240 + Motion::TimeoutFudgeMs, this, [guard, finish]() {
        if (!guard) return;
        finish();
    });
    rise->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainShell::showPage(int pageIndex)
{
    if (pageIndex < 0 || pageIndex >= m_pages.size()) return;
    const PageEntry& entry = m_pages.at(pageIndex);
    const int stackIndex = pageIndex + 1;   // 栈索引 0 是控制台主屏

    if (m_stack->currentIndex() == stackIndex) {
        // 同一页再点一次：重放 onActivated（等价 C# 的「读取设备信息」入口）。
        entry.page->onActivated();
        return;
    }

    const int previous = m_stack->currentIndex();
    if (previous >= 1 && previous - 1 < m_pages.size())
        m_pages.at(previous - 1).page->onDeactivated();

    // 转场方向：从控制台进页一律算「前进」；页间跳转按栈序号比大小。
    // 🔴 早先写成 `(previous != kConsoleStackIndex) && ...`，把「从主屏进页」
    //    判成了后退 —— 新页会从左边推入，方向整个反掉。
    const bool forward = (previous == kConsoleStackIndex) || (stackIndex > previous);

    // 3D 转场：切换前抓旧页快照，切换后抓新页快照，叠层动画期间真实控件
    // 已就位（叠层只是快照投影，不拦鼠标）。动画关 / 首帧 / 未显示 → 直改。
    // 2026-10-17：统一走 Fluid —— 控制台模型里「分组」不再是导航层级，
    // 原来的跨组 Cube 大动作失去语义，留着只会显得比内容还抢戏。
    const bool animate = m_transition && Motion::animationsEnabled() && isVisible()
                         && previous != stackIndex && !m_swipeSwitching;
    QPixmap fromPix;
    // 图标矩形必须在 setCurrentIndex **之前**取：切走之后主屏的子控件全部
    // 变成不可见，ConsolePage::tileRect 里的 isVisible 守卫会返回空矩形，
    // 转场就悄悄退化成推入了（2026-10-17 实测踩过一次）。
    QRect zoomOrigin;
    if (animate) {
        // 从主屏出发走缓存（此刻主屏正是当前页，抓出来的一定是对的）。
        fromPix = (previous == kConsoleStackIndex) ? consoleSnapshot() : m_stack->grab();
        if (previous == kConsoleStackIndex && m_consolePage)
            zoomOrigin = m_consolePage->tileRect(entry.key);
    }

    m_stack->setCurrentIndex(stackIndex);
    entry.page->onActivated();
    updatePageHeader(pageIndex);
    // 灵动岛切换器里给当前页那张卡片加一圈强调色描边。
    if (m_island) m_island->setCurrentKey(entry.key);

    if (animate) {
        const QPixmap toPix = m_stack->grab();
        m_transition->setGeometry(m_stack->geometry());

        // 从主屏点图标进页 → 让页面从**那个图标**的位置长出来（Mode::Zoom）。
        // 页间跳转 / 从侧边或命令行走进来（拿不到图标矩形）→ 退回推入转场。
        PageTransition::Mode mode = zoomOrigin.isValid() ? PageTransition::Mode::Zoom
                                                         : PageTransition::Mode::Fluid;
        m_transition->start(fromPix, toPix, mode, forward,
                            ThemeManager::instance().color(ColorToken::Bg), []() {},
                            zoomOrigin, stackBackdrop());
    }
}

void MainShell::navigateTo(const QString& key)
{
    // "console" 是主屏的保留 key（探针 --page console 也走这里）。
    if (key.isEmpty() || key == QLatin1String("console")) {
        showConsole();
        return;
    }
    for (int i = 0; i < m_pages.size(); ++i) {
        if (m_pages.at(i).key == key) {
            showPage(i);
            return;
        }
    }
}

void MainShell::applyTheme(bool dark)
{
    m_darkTheme = dark;
    ThemeManager::instance().setDark(dark);
    ThemeManager::instance().apply(*qApp);
    // 主题钮是自绘的（太阳 / 月牙按 isDark 切换），QSS 重放不会触发重绘。
    if (m_themeToggle) m_themeToggle->update();
    invalidateConsoleShot();   // 换主题后主屏配色全变，旧快照作废
    if (m_controlPage) m_controlPage->update();
    // 色晕的四个色值随主题变（深色一档 / 浅色一档），缓存必须重烘。
    rebuildBgBase();
    scheduleGlassBackdrops();   // 底色晕换了一套 → Dock 的玻璃背板也得重裁
}

void MainShell::onPageStatus(const QString& device, const QString& mode, const QString& hint)
{
    // 统一走 ShellStatus 中枢（旧 setShellStatus 通道与新的 report() 汇进同一份状态）。
    ShellStatus::setDevice(device, mode, hint);
}

// ── 账号轮询 / 启动更新检查已随服务器功能移除（停更离线版）────────────────

// ── 彩蛋：Konami 码（↑↑↓↓←→←→BA）───────────────────────────────────────────
//  维护者须知（这不是 bug）：这个函数就是「彩蛋：输入 Konami 码」的判定核心。
//  返回 true = 十连击刚好走完，调用方负责演出（triggerKonamiEgg）。

bool MainShell::feedKonamiKey(int key)
{
    if (key == kKonamiSequence[m_konamiProgress]) {
        ++m_konamiProgress;
    } else {
        // 错位时**不回退到 0**而是重判这一键：序列里 ↑ 和 ← 都有重复，
        // 连按 ↑↑↑ 时第三个 ↑ 应当仍算「序列第 2 位的 ↑」，而不是从头再来。
        // 表外的键（含普通字母 / 数字 / 回车）一律清零 —— 否则用户先乱按一堆
        // 方向键、再敲 B A 也会被拼成一次命中。
        m_konamiProgress = (key == kKonamiSequence[0]) ? 1 : 0;
    }

    if (m_konamiProgress == kKonamiLength) {
        m_konamiProgress = 0;   // 立刻清零：按住不放的自动重复不会连着再触发一次
        return true;
    }
    return false;
}

void MainShell::resetKonamiProgress()
{
    m_konamiProgress = 0;
}

void MainShell::triggerKonamiEgg()
{
    // 2026-10-31：通知中心与 Toast 横幅已按用户要求整体移除，
    // 彩蛋不再有「演出」可做（原来靠三条 Toast 提示）。
    // 序列识别逻辑保留 —— 它是纯输入状态机，将来想换成别的反馈形式
    // （比如让灵动岛闪一下）时入口还在。
}

bool MainShell::eventFilter(QObject* watched, QEvent* event)
{
    // ── 彩蛋：全局 Konami 码（↑↑↓↓←→←→BA）─────────────────────────────────
    //  维护者须知（这不是 bug）：这是彩蛋「输入 Konami 码」的事件入口。
    //  · 只**观察** KeyPress，命中前一律不 consume —— 页面里的方向键导航、
    //    列表选择照常工作，不会因为装了彩蛋就变钝；
    //  · 焦点在输入框里时整段跳过：B / A 就是普通字母，吃了等于毁掉打字。
    //    QLineEdit（含 IosSearchField 等子类）/ QTextEdit / QPlainTextEdit 都算，
    //    所以两个方向都查一遍（watched 是事件目标，focusWidget 是真正的焦点）。
    if (event->type() == QEvent::KeyPress) {
        QWidget* focus = QApplication::focusWidget();
        if (isTextInputWidget(focus) || isTextInputWidget(qobject_cast<QWidget*>(watched))) {
            // 输入框里按的键一律不算，且清空进度：否则「框里按 ↑↑、出来再按 ↓↓」
            // 会被拼成一条完整序列。
            resetKonamiProgress();
        } else {
            auto* key = static_cast<QKeyEvent*>(event);
            // 带 Ctrl / Alt / Meta 的组合键不参与：Ctrl+A（全选）、Alt+←（后退）
            // 这类快捷键既不该被彩蛋吃掉，语义上也不属于「按顺序敲一串键」。
            // 按住不放的自动重复（isAutoRepeat）直接跳过：既不计数**也不清零** ——
            // 用户按住 ↑ 时进度停在 1，松手再按 ↓ 正好接上；若清零则按住 ↑ 会把
            // 刚按下的那一下也抹掉，看起来像彩蛋在抽风。
            if (key->isAutoRepeat()) {
                // 忽略
            } else if (key->modifiers().testFlag(Qt::ControlModifier)
                       || key->modifiers().testFlag(Qt::AltModifier)
                       || key->modifiers().testFlag(Qt::MetaModifier)) {
                resetKonamiProgress();
            } else if (feedKonamiKey(key->key())) {
                triggerKonamiEgg();
            }
        }
    }

    // 灵动岛展开后，点界面上任何别处都收起它（iOS 的行为）。
    // 全局过滤器只在不展开时做一次布尔判断，开销可忽略。
    if (m_island && m_island->isExpanded() && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint local = m_island->mapFromGlobal(mouse->globalPosition().toPoint());
        if (!m_island->rect().contains(local)) m_island->setExpanded(false);
    }

    // 待机计时：任何按键 / 按下都算「用户在场」。
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::MouseButtonPress) {
        if (!(m_standby && m_standby->isVisible())) noteUserActivity();
    }

    // ── 横滑：主屏 ⇄ 控制中心（跟手）───────────────────────────────────────
    // 用户口径：「这个页面向左滑是控制中心，默认显示为主页」。
    // 判定顺序（每一步都在下一个之前 return，避免两套手势抢同一笔拖动）：
    //   ① 按下时先看当前页有没有左右邻居，有就「备枪」；
    //   ② 移动时先过竖向死区（页面里全是竖向滚动列表，抢了就滚不动）；
    //   ③ 越过 26px 且横向明显压过竖向 → 真正开始跟手。
    if (m_central && m_stack && event->type() == QEvent::MouseButtonPress) {
        m_swipeArmed = false;
        m_swipeActive = false;
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            // 🔴 滑杆 / 输入框上的横拖不算横滑手势：控制中心页有两条滑杆，
            //    手势一旦接管，滑块就再也拖不动了（move 全被过滤器吞掉）。
            const bool onInteractive = qobject_cast<QAbstractSlider*>(watched) != nullptr
                                       || qobject_cast<QLineEdit*>(watched) != nullptr;
            const int target = onInteractive ? -2 : swipeTargetForCurrentPage();
            const QPoint pressAt = m_central->mapFromGlobal(mouse->globalPosition().toPoint());
            // 顶栏上的横拖不接：那里按下会走系统拖动窗口（startSystemMove），
            // 之后 move / release 都不再回到 Qt，手势会一直挂着。
            const bool onTopBar = m_topBar && pressAt.y() <= m_topBar->height();
            if (target != -2 && !onTopBar) {
                m_swipeArmed = true;
                m_swipeTargetPage = target;
                // 目标在主屏右边（控制中心）= 往左滑 = 前进；否则后退。
                m_swipeForward = (target >= 0);
                m_swipeStart = pressAt;
                m_swipeLastPos = pressAt;
                m_swipeLastMs = QDateTime::currentMSecsSinceEpoch();
                m_swipeVelocity = 0.0;
            }
        }
    } else if (m_swipeArmed && event->type() == QEvent::MouseMove) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const QPoint local = m_central->mapFromGlobal(mouse->globalPosition().toPoint());
        const int dx = local.x() - m_swipeStart.x();
        const int dy = local.y() - m_swipeStart.y();
        // 沿目标方向的分量：前进往左拖（dx 取负），后退往右拖。
        const int along = m_swipeForward ? -dx : dx;

        if (!m_swipeActive) {
            if (qAbs(dy) > 46 && qAbs(dy) > qAbs(dx)) {
                m_swipeArmed = false;          // 竖向意图，让给控件
            } else if (along > 26 && along > qAbs(dy) * 1.2) {
                beginSwipe(m_swipeForward);
            }
        }
        if (m_swipeActive) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 dt = now - m_swipeLastMs;
            if (dt > 0) {
                // 平滑一点：瞬时速度跳得厉害，单次采样会把一次抖动判成甩。
                const qreal v = (local.x() - m_swipeLastPos.x()) * 1000.0 / dt;
                m_swipeVelocity = m_swipeVelocity * 0.55 + v * 0.45;
                m_swipeLastPos = local;
                m_swipeLastMs = now;
            }
            const int w = qMax(1, m_stack->width());
            m_swipeProgress = qBound(0.0, along / qreal(w), 1.0);
            if (m_transition) m_transition->drive(m_swipeProgress);
            return true;   // 拖拽期间别让下面的控件再收 move（会误触发 hover / 选中）
        }
    } else if (m_swipeArmed && event->type() == QEvent::MouseButtonRelease) {
        const bool wasActive = m_swipeActive;
        endSwipe();
        // 跟手结束的这一次松手**必须吞掉**：否则手指正好停在某个图标 / 磁贴上，
        // 松手会被当成一次点击（「划完顺手点进了某一页」）。
        if (wasActive) return true;
    }

    // ── 左边缘右滑返回（iOS 手势）─────────────────────────────────────────
    // 从窗口左边缘 16px 内按下、往右拖过 110px 松手 → 回主屏。
    // 死区只有 16px，不会抢页面里正常控件的点击；一旦垂直位移压过水平位移就放弃
    // （页面左侧多半是竖向滚动列表，抢了会让人滚不动）。
    // 🔴 手势必须挂在 qApp 过滤器上：按下之后事件会被那个控件 grab 走，
    //    装在 central 上收不到 move / release。
    // 控制中心页不走这条：它有专门的横滑（跟手），两套一起上会互相抢。
    if (m_central && m_stack && m_stack->currentIndex() != kConsoleStackIndex
        && m_stack->currentIndex() != m_controlPageIndex + 1
        && m_topBar) {
        const QEvent::Type type = event->type();
        if (type == QEvent::MouseButtonPress) {
            // 新的一笔按下：先把上一笔丢掉 —— 上次按下后窗口失焦 / 被弹窗抢走，
            // release 就收不到，tracking 会一直挂着，之后每次移动都误判。
            m_edgeSwipeTracking = false;
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton) {
                const QPoint local = m_central->mapFromGlobal(mouse->globalPosition().toPoint());
                if (local.x() >= 0 && local.x() <= 16 && local.y() > m_topBar->height()
                    && local.y() < m_central->height()) {
                    m_edgeSwipeTracking = true;
                    m_edgeSwipeStart = local;
                }
            }
        } else if (m_edgeSwipeTracking && type == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint local = m_central->mapFromGlobal(mouse->globalPosition().toPoint());
            const int dx = local.x() - m_edgeSwipeStart.x();
            const int dy = qAbs(local.y() - m_edgeSwipeStart.y());
            if (dy > 60 && dy > dx) m_edgeSwipeTracking = false;   // 竖向意图，让给控件
        } else if (m_edgeSwipeTracking && type == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint local = m_central->mapFromGlobal(mouse->globalPosition().toPoint());
            const int dx = local.x() - m_edgeSwipeStart.x();
            const int dy = qAbs(local.y() - m_edgeSwipeStart.y());
            m_edgeSwipeTracking = false;
            if (dx > 110 && dy < 140) {
                showConsole();
                return true;
            }
        }
    }

    // 顶栏拖动 / 双击最大化（与既有 MainWindow 的 WindowTitleBar 同款行为）。
    // 🔴 用缓存的 m_topBar，**不要** findChild —— 本过滤器挂在 qApp 上，每个事件
    //    都会进来一次，findChild 是全树递归，事件量下会明显掉帧（原实现即如此，
    //    那时过滤器只挂在顶栏子控件上所以没暴露）。
    // 判定用 isAncestorOf 而不是 parent()==m_topBar：品牌组是嵌一层 QWidget 的
    // （logo / 名称 / 版本在里面），只比直接父级会让那一段拖不动窗口。
    if (m_topBar
        && (watched == m_topBar
            || (watched->isWidgetType()
                && m_topBar->isAncestorOf(static_cast<QWidget*>(watched))))) {
        // 顶栏上的按钮（菜单 / 登录 / 窗口钮）不参与拖动。
        if (qobject_cast<QPushButton*>(watched)) return QMainWindow::eventFilter(watched, event);
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && windowHandle()) {
                windowHandle()->startSystemMove();
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? showNormal() : showMaximized();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

#ifdef Q_OS_WIN
bool MainShell::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);
    const auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_NCHITTEST && !isMaximized()) {
        constexpr int border = 6;
        const QPoint cursor = mapFromGlobal(QCursor::pos());
        const bool left = cursor.x() >= 0 && cursor.x() < border;
        const bool right = cursor.x() <= width() && cursor.x() > width() - border;
        const bool top = cursor.y() >= 0 && cursor.y() < border;
        const bool bottom = cursor.y() <= height() && cursor.y() > height() - border;
        if (top && left) *result = HTTOPLEFT;
        else if (top && right) *result = HTTOPRIGHT;
        else if (bottom && left) *result = HTBOTTOMLEFT;
        else if (bottom && right) *result = HTBOTTOMRIGHT;
        else if (left) *result = HTLEFT;
        else if (right) *result = HTRIGHT;
        else if (top) *result = HTTOP;
        else if (bottom) *result = HTBOTTOM;
        else return QMainWindow::nativeEvent(eventType, message, result);
        return true;
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

} // namespace kc::ui
