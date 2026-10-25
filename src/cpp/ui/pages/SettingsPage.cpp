#include "ui/pages/SettingsPage.h"

#include "kc/diag/SessionLog.h"
#include "ui/dialogs/DisclaimerDialog.h"
#include "ui/dialogs/WallpaperEnginePickerDialog.h"
#include "ui/dialogs/WelcomeDialog.h"   // 「再看一次新手引导」

#include "kc/config/AppSettings.h"
#include "kc/config/ClientConfig.h"
#include "kc/media/VideoBackground.h"   // 背景文件对话框的扩展名清单（图片 + 视频）
#include "ui/theme/GlassSettings.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/GlassSurface.h"
#include "ui/widgets/IosGroupedList.h"
#include "ui/widgets/IosSwitch.h"
#include "ui/widgets/NotificationBar.h"
#include "ui/widgets/SegmentedControl.h"
#include "ui/widgets/Toast.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRunnable>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace kc::ui {
namespace {

// 滑杆刻度：不透明度用 0~100（百分比），模糊用 0~48（像素半径）。
constexpr int kPercentMax = 100;
constexpr int kBlurMax = 48;

QLabel* makeValueLabel(QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setFixedWidth(48);
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return label;
}

/// 滑杆（0~100）→ 「NN%」读数实时同步。
void bindPercentLabel(QSlider* slider, QLabel* label)
{
    QObject::connect(slider, &QSlider::valueChanged, label,
                     [slider, label] { label->setText(QString::number(slider->value()) + QStringLiteral("%")); });
    label->setText(QString::number(slider->value()) + QStringLiteral("%"));
}

/// 滑杆（0~48）→ 「NNpx」读数实时同步。
void bindPixelLabel(QSlider* slider, QLabel* label)
{
    QObject::connect(slider, &QSlider::valueChanged, label,
                     [slider, label] { label->setText(QString::number(slider->value()) + QStringLiteral("px")); });
    label->setText(QString::number(slider->value()) + QStringLiteral("px"));
}

/// 一次性的后台任务（与 AppealDialog 同款）。
/// 卡密兑换是**同步阻塞** HTTP，绝不能在 GUI 线程跑。
class LambdaTask : public QRunnable
{
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) { setAutoDelete(true); }
    void run() override
    {
        if (m_fn) m_fn();
    }

private:
    std::function<void()> m_fn;
};

} // namespace

SettingsPage::SettingsPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("设置"));
    buildUi();
    loadFromSettings();
}

void SettingsPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    // 设置项多，窗口不够高时要能滚动（与 C# 原页保留 ScrollViewer 同因）。
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll, 1);

    auto* host = new QWidget;
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(10);

    // ── 背景设置 ─────────────────────────────────────────────────────────
    auto* bgCard = makeCard(QStringLiteral("背景设置"));
    auto* bgForm = new QFormLayout;
    bgForm->setLabelAlignment(Qt::AlignRight);
    bgForm->setHorizontalSpacing(10);
    bgForm->setVerticalSpacing(6);

    auto* bgRow = new QHBoxLayout;
    bgRow->setSpacing(6);
    m_bgPath = new QLineEdit(host);
    m_bgPath->setPlaceholderText(QStringLiteral("未选择背景（支持图片与 mp4 等视频）"));
    auto* browseButton = new QPushButton(QStringLiteral("浏览"), host);
    browseButton->setCursor(Qt::PointingHandCursor);
    auto* clearButton = new QPushButton(QStringLiteral("清除"), host);
    clearButton->setCursor(Qt::PointingHandCursor);
    bgRow->addWidget(m_bgPath, 1);
    bgRow->addWidget(browseButton);
    bgRow->addWidget(clearButton);
    bgForm->addRow(QStringLiteral("背景文件:"), bgRow);

    m_bgAnimation = new QComboBox(host);
    m_bgAnimation->addItem(QStringLiteral("Ken Burns (缩放+平移)"), QStringLiteral("KenBurns"));
    m_bgAnimation->addItem(QStringLiteral("缩放"), QStringLiteral("Zoom"));
    m_bgAnimation->addItem(QStringLiteral("平移"), QStringLiteral("Pan"));
    m_bgAnimation->addItem(QStringLiteral("无动画"), QStringLiteral("None"));
    bgForm->addRow(QStringLiteral("动画效果:"), m_bgAnimation);

    m_bgFit = new QComboBox(host);
    m_bgFit->addItem(QStringLiteral("裁剪铺满（默认）"), QStringLiteral("Fill"));
    m_bgFit->addItem(QStringLiteral("完整显示"), QStringLiteral("Uniform"));
    m_bgFit->addItem(QStringLiteral("拉伸填满"), QStringLiteral("Stretch"));
    bgForm->addRow(QStringLiteral("显示方式:"), m_bgFit);

    m_bgOpacity = new QSlider(Qt::Horizontal, host);
    m_bgOpacity->setRange(0, kPercentMax);
    m_bgOpacityValue = makeValueLabel(host);
    auto* opacityRow = new QHBoxLayout;
    opacityRow->setSpacing(8);
    opacityRow->addWidget(m_bgOpacity, 1);
    opacityRow->addWidget(m_bgOpacityValue);
    bgForm->addRow(QStringLiteral("背景不透明度:"), opacityRow);
    bindPercentLabel(m_bgOpacity, m_bgOpacityValue);

    m_bgBlur = new QSlider(Qt::Horizontal, host);
    m_bgBlur->setRange(0, kBlurMax);
    m_bgBlurValue = makeValueLabel(host);
    auto* blurRow = new QHBoxLayout;
    blurRow->setSpacing(8);
    blurRow->addWidget(m_bgBlur, 1);
    blurRow->addWidget(m_bgBlurValue);
    bgForm->addRow(QStringLiteral("背景模糊:"), blurRow);
    bindPixelLabel(m_bgBlur, m_bgBlurValue);

    static_cast<QVBoxLayout*>(bgCard->layout())->addLayout(bgForm);
    connect(browseButton, &QPushButton::clicked, this, [this] {
        // 过滤器带上视频扩展名：此前只列图片，mp4 在对话框里根本选不中 ——
        // 用户看到的就是「不支持 mp4」。真正的解码能力见 kc/media/VideoBackground。
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择背景文件"), m_bgPath->text().trimmed(),
            kc::media::VideoBackground::dialogFilter());
        if (path.isEmpty()) return;
        m_bgPath->setText(path);
        // 选完立刻生效：保存钮在滚动区最底下，不即时应用的话用户会以为「没反应」。
        // 背景类的改动本来就是所见即所得，这里与两个壁纸选择器的行为对齐。
        applyAndSave();
    });

    // ── 壁纸引擎入口（对齐 C# BtnPickWallpaperEngine_Click）：选中 → 填进背景文件框
    //    → 走统一保存链路。
    //    2026-10-21：**云端背景库入口已删除**（服务端 /api/wallpapers 一并下线）——
    //    跨境上行只有 ~1MB/s，且客户端每次启动都要重下，收益不抵成本。
    auto* bgPickRow = new QHBoxLayout;
    bgPickRow->setSpacing(6);
    auto* wePickButton = new QPushButton(QStringLiteral("从 Wallpaper Engine 选择"), host);
    wePickButton->setCursor(Qt::PointingHandCursor);
    bgPickRow->addWidget(wePickButton);
    bgPickRow->addStretch();
    bgForm->addRow(QStringLiteral("动态壁纸:"), bgPickRow);

    auto applyPickedBackground = [this](const QString& path) {
        if (path.isEmpty()) return;
        m_bgPath->setText(path);
        applyAndSave();
    };
    connect(wePickButton, &QPushButton::clicked, this, [this, applyPickedBackground] {
        applyPickedBackground(WallpaperEnginePickerDialog::pick(this));
    });
    connect(clearButton, &QPushButton::clicked, this, [this] {
        m_bgPath->clear();
        applyAndSave();   // 「清除」也要立刻生效，否则背景会一直挂在那儿
    });

    // ── 界面设置 ─────────────────────────────────────────────────────────
    auto* uiCard = makeCard(QStringLiteral("界面设置"));
    auto* uiForm = new QFormLayout;
    uiForm->setLabelAlignment(Qt::AlignRight);
    uiForm->setHorizontalSpacing(10);
    uiForm->setVerticalSpacing(6);

    // ── 强调色 ────────────────────────────────────────────────────────────
    // 用户口径（2026-10-24）：「多加一点个性化功能」。强调色是唯一一个
    // 一眼就能看出「这是我调过的」的个性化项 —— 按钮、进度条、选中态、
    // 链接、焦点框全都跟着变，比换壁纸更能体现「我的工具」。
    //
    // 8 个预设色点 + 自定义。点一下**立即生效**（改 token 表 + 重放 QSS），
    // 并直接落盘（点色点不是拖滑条，没有「拖一半」的中间态，不必防抖）。
    auto* accentRow = new QWidget(host);
    auto* accentLayout = new QHBoxLayout(accentRow);
    accentLayout->setContentsMargins(0, 0, 0, 0);
    accentLayout->setSpacing(8);

    struct AccentChoice {
        const char* hex;
        const char* tip;
    };
    static const AccentChoice kChoices[] = {
        {"#0A84FF", "蓝（默认）"}, {"#8B5CF6", "紫"}, {"#EC4899", "粉"},
        {"#EF4444", "红"},         {"#F97316", "橙"}, {"#EAB308", "黄"},
        {"#22C55E", "绿"},         {"#06B6D4", "青"},
    };
    const QString ringColor = ThemeManager::instance().color(ColorToken::Text).name();
    for (const AccentChoice& choice : kChoices) {
        auto* chip = new QPushButton(accentRow);
        chip->setFixedSize(26, 26);
        chip->setCursor(Qt::PointingHandCursor);
        chip->setCheckable(true);
        chip->setToolTip(QString::fromUtf8(choice.tip));
        const QString hex = QString::fromLatin1(choice.hex);
        chip->setProperty("accentHex", hex);
        // 🔴 border-radius 写 13px（= 26/2），**不能**写 999px：超过盒子一半时
        //    Qt 不退化成胶囊、直接变直角（项目红线，MEMORY 里有）。
        // 🔴 选中态必须写完整的 `border: 2px solid X`，只写 `border-color`
        //    Qt 不认（基础规则里的 border 会整体覆盖掉它，看起来像没选中）。
        chip->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; border: 2px solid transparent; border-radius: 13px; }"
            "QPushButton:hover { border: 2px solid rgba(255,255,255,.55); }"
            "QPushButton:checked { border: 2px solid %2; }")
                                .arg(hex, ringColor));
        const QColor color(hex);
        connect(chip, &QPushButton::clicked, this, [this, color] { applyAccentColor(color); });
        accentLayout->addWidget(chip);
        m_accentChips.append(chip);
    }

    auto* accentCustom = new QPushButton(QStringLiteral("自定义…"), host);
    accentCustom->setCursor(Qt::PointingHandCursor);
    connect(accentCustom, &QPushButton::clicked, this, [this] {
        const QColor cur = ThemeManager::instance().accent().isValid()
                               ? ThemeManager::instance().accent()
                               : QColor(QStringLiteral("#0A84FF"));
        const QColor picked = QColorDialog::getColor(cur, this, QStringLiteral("选择强调色"));
        if (!picked.isValid()) return;
        applyAccentColor(picked);
    });
    accentLayout->addWidget(accentCustom);
    accentLayout->addStretch(1);
    uiForm->addRow(QStringLiteral("强调色:"), accentRow);

    m_cardOpacity = new QSlider(Qt::Horizontal, host);
    m_cardOpacity->setRange(0, kPercentMax);
    m_cardOpacityValue = makeValueLabel(host);
    auto* cardOpacityRow = new QHBoxLayout;
    cardOpacityRow->setSpacing(8);
    cardOpacityRow->addWidget(m_cardOpacity, 1);
    cardOpacityRow->addWidget(m_cardOpacityValue);
    uiForm->addRow(QStringLiteral("卡片不透明度:"), cardOpacityRow);
    bindPercentLabel(m_cardOpacity, m_cardOpacityValue);

    m_uiTransparency = new QSlider(Qt::Horizontal, host);
    m_uiTransparency->setRange(0, kPercentMax);
    m_uiTransparencyValue = makeValueLabel(host);
    auto* uiTransparencyRow = new QHBoxLayout;
    uiTransparencyRow->setSpacing(8);
    uiTransparencyRow->addWidget(m_uiTransparency, 1);
    uiTransparencyRow->addWidget(m_uiTransparencyValue);
    uiForm->addRow(QStringLiteral("界面透明:"), uiTransparencyRow);
    bindPercentLabel(m_uiTransparency, m_uiTransparencyValue);

    // ── 四条滑杆：拖动即时预览 ────────────────────────────────────────────
    // 原来的行为是「拖了界面纹丝不动，必须去点保存」——用户口径就是「透明度不能调节」。
    // 现在 valueChanged → 90ms 防抖 → applyLive()（只改内存 + 重放样式表，不写盘），
    // 松手时静默落盘。防抖是必需的：重放一次样式表要 repolish 整棵控件树，
    // 每个像素变动都跑一次会明显掉帧。
    m_liveTimer = new QTimer(this);
    m_liveTimer->setSingleShot(true);
    m_liveTimer->setInterval(90);
    connect(m_liveTimer, &QTimer::timeout, this, [this] { applyLive(); });

    // 落盘定时器：点轨道 / 键盘调值不会发 sliderReleased，靠它兜住（700ms 静默后写一次）。
    auto* saveTimer = new QTimer(this);
    saveTimer->setSingleShot(true);
    saveTimer->setInterval(700);
    connect(saveTimer, &QTimer::timeout, this, [this] { persist(); });

    const auto scheduleLive = [this, saveTimer] {
        if (m_loading) return;              // 回填期间不预览
        if (m_liveTimer) m_liveTimer->start();
        saveTimer->start();
    };
    for (QSlider* s : {m_bgOpacity, m_bgBlur, m_cardOpacity, m_uiTransparency}) {
        connect(s, &QSlider::valueChanged, this, scheduleLive);
        // 松手：把预览值立刻落盘（不弹通知，否则拖四次弹四次「设置已保存」）
        connect(s, &QSlider::sliderReleased, this, [this, saveTimer] {
            saveTimer->stop();
            persist();
        });
    }

    static_cast<QVBoxLayout*>(uiCard->layout())->addLayout(uiForm);

    // ── 灵动岛 ───────────────────────────────────────────────────────────
    // 用户口径（2026-10-18）：「灵动岛可玩性太少了、个性化也太少了」。
    // 这一整张卡的每一项都**即时生效**（写内存 → 让外壳重读 → 岛自己重排重绘），
    // 不走「先点保存才见效」—— 配色 / 列数 / 显示方式这类东西必须一边调一边看，
    // 否则等于闭着眼睛选。落盘仍然交给 700ms 静默的 saveTimer。
    auto* islandCard = makeCard(QStringLiteral("灵动岛"));
    // iOS 分组列表：标题在左、控件在右、行间有分隔线（原来是 QFormLayout +
    // 铺满整行的下拉框，观感不像 iOS）。
    auto* islandList = new IosGroupedList(islandCard);

    // 下拉框统一压窄：分组行右侧的控件不该铺满整行。
    const auto compactCombo = [](QComboBox* combo) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(10);
        combo->setMinimumWidth(190);
        combo->setMaximumWidth(250);
        return combo;
    };

    // iOS 分段选择器（替换下拉框：三档配色一眼可见，点一下直接切）
    m_islandTheme = new SegmentedControl(islandList);
    m_islandTheme->addItem(QStringLiteral("纯黑"), QStringLiteral("ink"));
    m_islandTheme->addItem(QStringLiteral("品牌蓝"), QStringLiteral("accent"));
    m_islandTheme->addItem(QStringLiteral("深色玻璃"), QStringLiteral("glass"));
    islandList->addCustomRow(QStringLiteral("外观配色"), m_islandTheme);

    m_islandRadius = new QSlider(Qt::Horizontal, islandList);
    m_islandRadius->setRange(10, 40);
    m_islandRadiusValue = makeValueLabel(islandList);
    auto* radiusBox = new QWidget(islandList);
    radiusBox->setMinimumWidth(240);
    auto* radiusRow = new QHBoxLayout(radiusBox);
    radiusRow->setContentsMargins(0, 0, 0, 0);
    radiusRow->setSpacing(8);
    radiusRow->addWidget(m_islandRadius, 1);
    radiusRow->addWidget(m_islandRadiusValue);
    islandList->addCustomRow(QStringLiteral("展开圆角"), radiusBox);
    bindPixelLabel(m_islandRadius, m_islandRadiusValue);

    m_islandCollapsed = compactCombo(new QComboBox(islandList));
    m_islandCollapsed->addItem(QStringLiteral("自动（进度 / 连接 / 设备）"),
                               QStringLiteral("auto"));
    m_islandCollapsed->addItem(QStringLiteral("设备名"), QStringLiteral("device"));
    m_islandCollapsed->addItem(QStringLiteral("当前页面名"), QStringLiteral("page"));
    m_islandCollapsed->addItem(QStringLiteral("时间"), QStringLiteral("clock"));
    islandList->addCustomRow(QStringLiteral("折叠态显示"), m_islandCollapsed);

    m_islandVisibility = compactCombo(new QComboBox(islandList));
    m_islandVisibility->addItem(QStringLiteral("常驻显示"), QStringLiteral("always"));
    m_islandVisibility->addItem(QStringLiteral("悬停才浮现"), QStringLiteral("hover"));
    m_islandVisibility->addItem(QStringLiteral("完全隐藏"), QStringLiteral("hidden"));
    islandList->addCustomRow(QStringLiteral("显示方式"), m_islandVisibility);

    m_islandColumns = compactCombo(new QComboBox(islandList));
    m_islandColumns->addItem(QStringLiteral("4 列（紧凑，行数变多）"), 4);
    m_islandColumns->addItem(QStringLiteral("6 列（标准）"), 6);
    m_islandColumns->addItem(QStringLiteral("8 列（宽屏）"), 8);
    islandList->addCustomRow(QStringLiteral("面板列数"), m_islandColumns);

    // iOS 开关（替换 QCheckBox）：文案在左、开关在右，与 iOS 设置页一致。
    m_islandDim = islandList->addToggleRow(QStringLiteral("展开时压暗背景"), false);
    m_islandLite = islandList->addToggleRow(QStringLiteral("动效精简（低配机 / 嫌晃眼）"), false);

    islandList->setFooter(QStringLiteral(
        "玩法：折叠时按住左右拖动可以切页；点开后顶上是搜索框（输入即过滤页面），"
        "下面一排是截图 / 主屏 / 控制中心 / 设置，再往下是页面卡片。"));

    static_cast<QVBoxLayout*>(islandCard->layout())->addWidget(islandList);

    const auto scheduleIsland = [this, saveTimer] {
        if (m_loading) return;
        applyIslandLive();
        saveTimer->start();
    };
    connect(m_islandTheme, &SegmentedControl::currentIndexChanged, this, scheduleIsland);
    connect(m_islandCollapsed, &QComboBox::currentIndexChanged, this, scheduleIsland);
    connect(m_islandVisibility, &QComboBox::currentIndexChanged, this, scheduleIsland);
    connect(m_islandColumns, &QComboBox::currentIndexChanged, this, scheduleIsland);
    connect(m_islandRadius, &QSlider::valueChanged, this, scheduleIsland);
    connect(m_islandDim, &IosSwitch::toggled, this, scheduleIsland);
    connect(m_islandLite, &IosSwitch::toggled, this, scheduleIsland);

    // ── 液态玻璃 ─────────────────────────────────────────────────────────
    // 用户口径（2026-10-23）：「能不能仿照 ios 系统做液态玻璃效果」。
    //
    // 这一整张卡是**所见即所得**的：上面一块真的液态玻璃面板（GlassSurface，
    // 带边缘折射透镜 / 色散彩边 / 指针镜面高光 / 按压果冻回弹），下面五条滑条
    // 直接驱动它。玻璃参数属于「看着调」的东西 —— 闭着眼睛拖滑条等于没做。
    //
    // 🔴 这五条滑条**不走 applyLive()**：那条路径会 refreshStyleSheet()
    //    重放整棵控件树的 QSS，玻璃参数只影响自绘面板，重放是纯浪费
    //    （一次 repolish 让拖动明显掉帧）。走 applyGlassLive()。
    auto* glassCard = makeCard(QStringLiteral("液态玻璃"));
    auto* glassForm = new QFormLayout;
    glassForm->setLabelAlignment(Qt::AlignRight);
    glassForm->setHorizontalSpacing(10);
    glassForm->setVerticalSpacing(6);

    m_glassPreview = new GlassSurface(host);
    // 420×190（≈2.2:1）：**不能再宽扁**。
    // 折射带宽度是按短边算的（短边 × edgeBand），5:1 的横条上带子只占 2.8%，
    // 边缘形变被压在两条窄缝里，用户会得出「调了没反应」的结论。
    // 接近 3:2 才是液态玻璃的正常形态（iOS 上的面板基本都是这个比例）。
    m_glassPreview->setFixedSize(420, 190);
    m_glassPreview->setRadius(22);
    m_glassPreview->setInteractive(true);
    {
        // 玻璃上压一行字：既当标题，也顺便验证「文字压在这块玻璃上够不够清楚」——
        // 浓度调太低时这行字会先糊掉，比任何数值都直观。
        // 🔴 不写死颜色：让它继承全局 QSS 的 QLabel 色（深色主题浅字 / 浅色主题深字）。
        auto* capLayout = new QVBoxLayout(m_glassPreview);
        capLayout->setContentsMargins(16, 12, 16, 12);
        auto* cap = new QLabel(QStringLiteral("液态玻璃 · 实时预览"), m_glassPreview);
        cap->setAttribute(Qt::WA_TransparentForMouseEvents);
        cap->setAlignment(Qt::AlignCenter);
        cap->setStyleSheet(QStringLiteral("background: transparent;"));
        QFont capFont = cap->font();
        capFont.setBold(true);
        capFont.setPointSizeF(capFont.pointSizeF() + 1.0);
        cap->setFont(capFont);
        capLayout->addWidget(cap, 0, Qt::AlignHCenter | Qt::AlignTop);
    }
    auto* previewRow = new QHBoxLayout;
    previewRow->setSpacing(16);
    previewRow->addStretch(1);
    previewRow->addWidget(m_glassPreview, 0, Qt::AlignVCenter);

    auto* previewHint = new QLabel(host);
    previewHint->setWordWrap(true);
    previewHint->setObjectName(QStringLiteral("settingsHint"));
    previewHint->setMinimumWidth(180);
    previewHint->setText(QStringLiteral(
        "这是一块真的液态玻璃：背板磨砂、边缘折射、顶沿高光都是实时算的。\n\n"
        "把鼠标移到它上面，镜面高光会跟着走；按住它会像果冻一样被压扁再弹回来。\n\n"
        "这两项是动态效果，截图里看不出来 —— 实际点一下就知道了。"));
    previewRow->addWidget(previewHint, 1);
    glassForm->addRow(QString(), previewRow);

    m_glassPreset = new QComboBox(host);
    for (const GlassPreset& p : glassPresets()) {
        m_glassPreset->addItem(p.name, p.id);
    }
    m_glassPreset->addItem(QStringLiteral("自定义"), QStringLiteral("custom"));
    glassForm->addRow(QStringLiteral("风格预设:"), m_glassPreset);

    m_glassHint = new QLabel(host);
    m_glassHint->setWordWrap(true);
    m_glassHint->setObjectName(QStringLiteral("settingsHint"));
    glassForm->addRow(QString(), m_glassHint);

    const auto makeGlassSlider = [&](const QString& label, int max, QSlider*& slider,
                                     QLabel*& value) {
        slider = new QSlider(Qt::Horizontal, host);
        slider->setRange(0, max);
        value = makeValueLabel(host);
        auto* row = new QHBoxLayout;
        row->setSpacing(8);
        row->addWidget(slider, 1);
        row->addWidget(value);
        glassForm->addRow(label, row);
    };
    makeGlassSlider(QStringLiteral("玻璃浓度:"), kPercentMax, m_glassTint, m_glassTintValue);
    makeGlassSlider(QStringLiteral("磨砂强度:"), kBlurMax, m_glassFrost, m_glassFrostValue);
    makeGlassSlider(QStringLiteral("边缘折射:"), kPercentMax, m_glassRefraction,
                    m_glassRefractionValue);
    makeGlassSlider(QStringLiteral("色散彩边:"), kPercentMax, m_glassDispersion,
                    m_glassDispersionValue);
    makeGlassSlider(QStringLiteral("高光强度:"), 160, m_glassHighlight, m_glassHighlightValue);

    bindPercentLabel(m_glassTint, m_glassTintValue);
    bindPixelLabel(m_glassFrost, m_glassFrostValue);
    bindPercentLabel(m_glassRefraction, m_glassRefractionValue);
    bindPercentLabel(m_glassDispersion, m_glassDispersionValue);
    bindPercentLabel(m_glassHighlight, m_glassHighlightValue);

    auto* glassNote = new QLabel(host);
    glassNote->setWordWrap(true);
    glassNote->setObjectName(QStringLiteral("settingsHint"));
    glassNote->setText(QStringLiteral(
        "边缘折射是液态玻璃的灵魂：玻璃边缘像厚透镜，把背后的画面向内挤压放大，"
        "少了它磨砂再细也只是塑料。调不出想要的就先用预设，再微调两三条。"));
    glassForm->addRow(QString(), glassNote);

    static_cast<QVBoxLayout*>(glassCard->layout())->addLayout(glassForm);

    m_glassLiveTimer = new QTimer(this);
    m_glassLiveTimer->setSingleShot(true);
    m_glassLiveTimer->setInterval(90);
    connect(m_glassLiveTimer, &QTimer::timeout, this, [this] { applyGlassLive(); });

    const auto scheduleGlass = [this, saveTimer] {
        if (m_loading || m_glassSyncing) return;
        // 🔴 不能直接 applyGlassLive()：磨砂 / 折射两条滑条每变一个像素就要重算
        //    整块背板（5~10ms），拖动时会明显掉帧。走 90ms 防抖，手感与那四条
        //    透明度滑条一致；松手时 persist() 会再调一次（保证最终值一定落上）。
        if (m_glassLiveTimer) m_glassLiveTimer->start();
        else applyGlassLive();
        saveTimer->start();
    };
    for (QSlider* s : {m_glassTint, m_glassFrost, m_glassRefraction,
                       m_glassDispersion, m_glassHighlight}) {
        connect(s, &QSlider::valueChanged, this, [this, scheduleGlass] {
            // 手调任意一条 → 预设落到「自定义」。用 QSignalBlocker 挡住
            // currentIndexChanged，否则会反过来把滑条值按预设覆盖一遍。
            if (!m_glassSyncing && m_glassPreset) {
                const int customIdx = m_glassPreset->findData(QStringLiteral("custom"));
                if (customIdx >= 0 && m_glassPreset->currentIndex() != customIdx) {
                    const QSignalBlocker block(m_glassPreset);
                    m_glassPreset->setCurrentIndex(customIdx);
                }
            }
            scheduleGlass();
        });
        connect(s, &QSlider::sliderReleased, this, [this, saveTimer] {
            saveTimer->stop();
            persist();
        });
    }
    connect(m_glassPreset, &QComboBox::currentIndexChanged, this,
            [this, saveTimer] {
                if (m_loading || m_glassSyncing) return;
                const QString id = m_glassPreset->currentData().toString();
                if (id != QStringLiteral("custom")) {
                    applyGlassPreset(id);      // 预设 → AppSettings（内存）
                    syncGlassFromSettings();   // → 滑条 + 预览
                }
                applyGlassLive();
                saveTimer->start();
            });

    // ── 刷机引擎（COM 速率 / 线程核心 / 兼容模式）──────────────────────────
    // 对齐 TMFTool 的同名三组开关。这三项**不进 applyLive**（不是所见即所得的东西：
    // 波特率要重新握手才生效、分片大小在下一次 program 才读到），改完直接落盘，
    // 下次连接设备时由引擎侧读 AppSettings。
    auto* engineCard = makeCard(QStringLiteral("刷机引擎"));
    auto* engineForm = new QFormLayout;
    engineForm->setLabelAlignment(Qt::AlignRight);
    engineForm->setHorizontalSpacing(10);
    engineForm->setVerticalSpacing(6);

    // 🔴 两行都用「控件 + addStretch」包一层：QFormLayout 的字段位会把单个控件
    //    拉满整行宽度 —— 下拉框拉满像输入框还行，spinbox 拉满则是**箭头跑到屏幕最右**，
    //    看着像坏掉了。stretch 让它们保持 sizeHint 宽度。
    m_engineBaud = new QComboBox(host);
    for (int baud : { 115200, 230400, 460800, 921600 })
        m_engineBaud->addItem(QString::number(baud), baud);
    auto* baudRow = new QHBoxLayout;
    baudRow->setSpacing(6);
    baudRow->addWidget(m_engineBaud);
    baudRow->addStretch();
    engineForm->addRow(QStringLiteral("COM 速率:"), baudRow);

    m_engineThreads = new QSpinBox(host);
    m_engineThreads->setRange(0, 64);
    // 🔴 setSpecialValueText 必须在 setRange 之后：范围变化会重置显示文本。
    m_engineThreads->setSpecialValueText(QStringLiteral("自动（按 CPU 核心数）"));
    m_engineThreads->setSuffix(QStringLiteral(" 个"));
    // 🔴 必须给下限：specialValueText 是「自动（按 CPU 核心数）」，而 QSS 的 padding
    //    与上下按钮会再吃掉一截内容盒 —— 只靠 sizeHint 会被裁成「自动（按 CPU 核心」。
    m_engineThreads->setMinimumWidth(200);
    auto* threadsRow = new QHBoxLayout;
    threadsRow->setSpacing(6);
    threadsRow->addWidget(m_engineThreads);
    threadsRow->addStretch();
    engineForm->addRow(QStringLiteral("线程核心:"), threadsRow);

    m_engineCompat = new QCheckBox(QStringLiteral("兼容模式（慢一点，但更不容易断）"), host);
    m_engineCompat->setCursor(Qt::PointingHandCursor);
    engineForm->addRow(QString(), m_engineCompat);

    auto* engineHint = new QLabel(host);
    engineHint->setObjectName(QStringLiteral("settingsHint"));
    engineHint->setWordWrap(true);
    engineHint->setText(QStringLiteral(
        "COM 速率只对 MTK（BROM 串口）生效，高通 EDL 走 USB 传输、不看波特率。"
        "三项都在下一次连接设备时生效。兼容模式会把 EDL 每次写入的分片从 64 MB 收到 8 MB、"
        "并在 MTK 每个传输包之间插 3 ms 间隔 —— 老固件、劣质线材下更不容易断流。"));
    engineForm->addRow(QString(), engineHint);

    static_cast<QVBoxLayout*>(engineCard->layout())->addLayout(engineForm);

    // 改动直接落盘，但**不走 persist()** —— persist() 会顺带 applyIslandLive()，
    // 点一下 spinbox 的箭头就让灵动岛重读重排一次，没必要。这里只写这三项。
    const auto scheduleEngine = [this] {
        if (m_loading) return;
        kc::AppSettings& s = kc::AppSettings::current();
        s.engineMtkBaud = m_engineBaud->currentData().toInt();
        s.engineThreads = m_engineThreads->value();
        s.engineCompatMode = m_engineCompat->isChecked();
        // 线程数必须立刻应用到全局线程池，否则用户改完要重启才生效。
        // 其余两项由引擎在「下一次连接设备」时自行读取，不需要运行时钩子。
        s.applyRuntime();
        s.save();
    };
    connect(m_engineBaud, &QComboBox::currentIndexChanged, this, scheduleEngine);
    connect(m_engineThreads, &QSpinBox::valueChanged, this, scheduleEngine);
    connect(m_engineCompat, &QCheckBox::toggled, this, scheduleEngine);

    // 2026-10-13：移除「服务器设置」「FDL 命令」两张卡（对齐 C# 设置页，C# 无这两块）。
    //   · 服务器地址 / FDL 命令改由配置文件与内置默认值决定，设置页不再提供就地编辑；
    //   · 主题切换入口也随「服务器设置」卡一并移除（与标题栏 / 「⋯」菜单的口径一致）。

    // ── 账号 ──────────────────────────────────────────────────────────────
    // 2026-10-20：会员状态 / 模块分级 / 卡密兑换随付费体系整体下线，
    // 这张卡只剩「当前登录的是谁」——它跟刷机功能无关，但用户需要能确认身份。
    // 账号卡片已随登录体系移除（停更离线版 2.0.6）。

    // ── 法务 ─────────────────────────────────────────────────────────────
    auto* legalCard = makeCard(QStringLiteral("法务"));
    auto* legalForm = new QFormLayout;
    legalForm->setLabelAlignment(Qt::AlignRight);
    legalForm->setHorizontalSpacing(10);
    legalForm->setVerticalSpacing(6);

    m_legalState = new QLabel(host);
    m_legalState->setWordWrap(true);
    legalForm->addRow(QStringLiteral("条款版本:"), m_legalState);

    // 「查看使用条款与隐私说明」（对齐 C# BtnShowLegal_Click）：只读模式打开
    // DisclaimerDialog(enforce=false)，关掉不影响使用、也不会退出程序。
    auto* legalRow = new QHBoxLayout;
    legalRow->setSpacing(6);
    auto* legalViewButton = new QPushButton(QStringLiteral("查看使用条款与隐私说明"), host);
    legalViewButton->setCursor(Qt::PointingHandCursor);
    legalRow->addWidget(legalViewButton);
    legalRow->addStretch();
    legalForm->addRow(QString(), legalRow);
    connect(legalViewButton, &QPushButton::clicked, this, [this] {
        DisclaimerDialog dialog(false, this);
        dialog.exec();
        loadFromSettings();   // 与 C# UpdateLegalState() 同步条款版本读数
    });

    static_cast<QVBoxLayout*>(legalCard->layout())->addLayout(legalForm);

    // ── 诊断与日志 ─────────────────────────────────────────────────────────
    // 停更离线版（2.0.6）：「上传日志给开发者」按钮已移除（服务器关停，没有接收端），
    // 只保留「打开日志目录」—— 本地日志仍然有用，用户可以自己打包发给朋友或贴到群里。
    auto* diagCard = makeCard(QStringLiteral("诊断与日志"));
    auto* diagForm = new QFormLayout;
    diagForm->setLabelAlignment(Qt::AlignRight);
    diagForm->setHorizontalSpacing(10);
    diagForm->setVerticalSpacing(6);

    auto* diagHint = new QLabel(host);
    diagHint->setWordWrap(true);
    diagHint->setText(
        QStringLiteral("遇到刷机失败、工具出错时，日志目录里保存着本次运行的完整记录，"
                       "可以自己打开查看，或复制出来反馈给开发者。"));
    diagForm->addRow(QString(), diagHint);

    auto* diagRow = new QHBoxLayout;
    diagRow->setSpacing(6);
    auto* openLogDirButton = new QPushButton(QStringLiteral("打开日志目录"), host);
    openLogDirButton->setCursor(Qt::PointingHandCursor);
    diagRow->addWidget(openLogDirButton);
    diagRow->addStretch();
    diagForm->addRow(QStringLiteral("问题反馈:"), diagRow);

    static_cast<QVBoxLayout*>(diagCard->layout())->addLayout(diagForm);
    connect(openLogDirButton, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(kc::diag::SessionLog::logDir()));
    });

    // ── 安全与隐私：已随环境检测功能移除（停更离线版 2.0.6，服务器关停）──────

    // ── 帮助与引导 ─────────────────────────────────────────────────────────
    // 首次启动的引导只弹一次（判据 QSettings onboarding/completed），换台机器或
    // 重装才会再见。用户想复看「主屏怎么用 / 动手前检查什么」，得有个入口。
    auto* helpCard = makeCard(QStringLiteral("帮助与引导"));
    auto* helpForm = new QFormLayout;
    helpForm->setLabelAlignment(Qt::AlignRight);
    helpForm->setHorizontalSpacing(10);
    helpForm->setVerticalSpacing(6);

    auto* helpHint = new QLabel(host);
    helpHint->setWordWrap(true);
    helpHint->setText(QStringLiteral(
        "新手引导只在第一次打开时自动出现一次。想再看一遍，点右边的按钮重新走一遍。"));
    helpForm->addRow(QString(), helpHint);

    auto* helpRow = new QHBoxLayout;
    helpRow->setSpacing(6);
    auto* replayTourButton = new QPushButton(QStringLiteral("再看一次新手引导"), host);
    replayTourButton->setCursor(Qt::PointingHandCursor);
    helpRow->addWidget(replayTourButton);
    helpRow->addStretch();
    helpForm->addRow(QStringLiteral("新手引导:"), helpRow);

    static_cast<QVBoxLayout*>(helpCard->layout())->addLayout(helpForm);
    // 🔴 这里**不动** onboarding 标记：重看只是重看，看完再置位一次也无害，
    //    中途关掉不该把「已看过」抹掉、害得下次启动又弹一遍。
    connect(replayTourButton, &QPushButton::clicked, this, [this] {
        WelcomeDialog dialog(this);
        dialog.exec();
    });

    // ── 保存 ─────────────────────────────────────────────────────────────
    // 🔴 「保存设置」必须钉在滚动区**外面**：它原来跟卡片一起放在滚动内容的最底部，
    //    窗口一矮就被裁掉一半（2026-10-17 截图实测：只露出按钮的上沿）。
    //    用户选完背景找不到保存钮，反馈出来就是「自定义背景怎么都打不开」。
    auto* saveRow = new QHBoxLayout;
    saveRow->addStretch();
    auto* saveButton = new QPushButton(QStringLiteral("保存设置"), this);
    saveButton->setObjectName(QStringLiteral("primaryButton"));
    saveButton->setCursor(Qt::PointingHandCursor);
    connect(saveButton, &QPushButton::clicked, this, [this] { applyAndSave(); });
    saveRow->addWidget(saveButton);

    // 卡序：账号与会员放在**第一张**。它原来排在「刷机引擎」之后（第五张），
    // 而设置页内容比窗口高一大截 —— 用户不滚到底根本看不到，反馈就是
    // 「卡密兑换 / 模块分级客户端没有显示」（2026-10-18）。
    //
    // 「液态玻璃」紧随背景设置：它是**视觉材质**的总开关，与壁纸强相关
    // （同一组参数在深色照片墙和浅色纯色墙上观感差很多），而且它自带实时
    // 预览面板 —— 放在第四张就得先滚一屏才看得见，等于白做。
    hostLayout->addWidget(bgCard);
    hostLayout->addWidget(glassCard);
    hostLayout->addWidget(uiCard);
    hostLayout->addWidget(islandCard);
    hostLayout->addWidget(engineCard);
    hostLayout->addWidget(diagCard);
    hostLayout->addWidget(helpCard);
    hostLayout->addWidget(legalCard);
    hostLayout->addStretch();
    scroll->setWidget(host);
    layout->addLayout(saveRow);   // 常驻底栏，不随内容滚动
}

QFrame* SettingsPage::makeCard(const QString& title)
{
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("settingsCard"));
    card->setFrameShape(QFrame::StyledPanel);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto* label = new QLabel(title, card);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    layout->addWidget(label);
    return card;
}

void SettingsPage::loadFromSettings()
{
    // 回填期间屏蔽「拖动预览」：setValue 会发 valueChanged，不挡住就会在切页时
    // 白白重放一次样式表（还会把别的页面也 repolish 一遍）。
    m_loading = true;
    const kc::AppSettings& s = kc::AppSettings::current();

    m_bgPath->setText(s.backgroundImagePath);
    m_bgAnimation->setCurrentIndex(
        std::max(0, m_bgAnimation->findData(s.backgroundAnimation)));
    m_bgFit->setCurrentIndex(std::max(0, m_bgFit->findData(s.backgroundFit)));
    m_bgOpacity->setValue(qRound(qBound(0.0, s.backgroundOpacity, 1.0) * kPercentMax));
    m_bgBlur->setValue(qBound(0, qRound(s.backgroundBlur), kBlurMax));

    m_cardOpacity->setValue(qRound(qBound(0.0, s.cardOpacity, 1.0) * kPercentMax));
    m_uiTransparency->setValue(qRound(qBound(0.0, s.uiTransparency, 1.0) * kPercentMax));
    syncAccentChips();

    // 灵动岛七项
    m_islandTheme->setCurrentIndex(std::max(0, m_islandTheme->findData(s.islandTheme)));
    m_islandRadius->setValue(qBound(10, s.islandRadius, 40));
    m_islandCollapsed->setCurrentIndex(
        std::max(0, m_islandCollapsed->findData(s.islandCollapsed)));
    m_islandVisibility->setCurrentIndex(
        std::max(0, m_islandVisibility->findData(s.islandVisibility)));
    m_islandColumns->setCurrentIndex(
        std::max(0, m_islandColumns->findData(s.islandColumns)));
    m_islandDim->setCheckedSilently(s.islandDim);
    m_islandLite->setCheckedSilently(s.islandLiteMotion);

    // 液态玻璃五项 + 预览
    syncGlassFromSettings();
    loadGlassPreviewBackdrop();

    // 刷机引擎三项
    m_engineBaud->setCurrentIndex(std::max(0, m_engineBaud->findData(s.engineMtkBaud)));
    m_engineThreads->setValue(qBound(0, s.engineThreads, 64));
    m_engineCompat->setChecked(s.engineCompatMode);

    m_legalState->setText(
        s.legalAcceptedVersion > 0
            ? QStringLiteral("已同意条款版本 v%1").arg(s.legalAcceptedVersion)
            : QStringLiteral("尚未同意条款（首次启动时会提示）"));

    // 安全与隐私分区已移除（停更离线版 2.0.6），不再有环境检测开关。

    m_loading = false;
}

// 控件 → AppSettings（内存）＋ 重放样式表：滑条拖动时的即时预览，**不写盘**。
// 写盘交给松手（persist）或「保存设置」，避免拖一次滑条写几十次 settings.json。
void SettingsPage::applyLive()
{
    kc::AppSettings& s = kc::AppSettings::current();

    s.backgroundOpacity = m_bgOpacity->value() / double(kPercentMax);
    s.backgroundBlur = m_bgBlur->value();
    s.cardOpacity = m_cardOpacity->value() / double(kPercentMax);
    s.uiTransparency = m_uiTransparency->value() / double(kPercentMax);

    // 外壳的动态背景（不透明度 / 模糊）由 MainShell 自己画，QSS 改不动。
    if (QWidget* w = window())
        QMetaObject::invokeMethod(w, "reloadBackground", Qt::DirectConnection);

    // 🔴 玻璃 alpha 是**替换进 QSS 文本**的（Qt QSS 没有 CSS 变量），不重放就还是旧值。
    ThemeManager::instance().refreshStyleSheet();

    // 重放样式表会给整棵树发 StyleChange，但 QStackedWidget 里非当前页的控件
    // 不会立刻重绘；显式刷一次窗口，拖动时画面才跟手。
    if (QWidget* w = window()) w->update();
}

// 控件 → AppSettings + save()，不弹通知（松手 / 滑条连拖时用）。
void SettingsPage::persist()
{
    kc::AppSettings& s = kc::AppSettings::current();

    s.backgroundImagePath = m_bgPath->text().trimmed();
    s.backgroundAnimation = m_bgAnimation->currentData().toString();
    s.backgroundFit = m_bgFit->currentData().toString();
    s.backgroundOpacity = m_bgOpacity->value() / double(kPercentMax);
    s.backgroundBlur = m_bgBlur->value();
    s.cardOpacity = m_cardOpacity->value() / double(kPercentMax);
    s.uiTransparency = m_uiTransparency->value() / double(kPercentMax);

    // 灵动岛七项也走这里：单独改某一项时 saveTimer 只调 applyIslandLive()（不落盘），
    // 点「保存设置」或松手才真正写盘 —— 与四条滑杆同一套节奏。
    applyIslandLive();

    // 液态玻璃五项：同上，滑条改动只走 applyGlassLive（不落盘），这里统一收口。
    applyGlassLive();

    // 刷机引擎三项：改了立刻落盘（这三项不是预览类，没有「拖一下看一眼」的需求）。
    s.engineMtkBaud = m_engineBaud->currentData().toInt();
    s.engineThreads = m_engineThreads->value();
    s.engineCompatMode = m_engineCompat->isChecked();
    // 线程核心必须立刻应用到全局线程池，否则用户改完要重启才生效。
    // 其余两项由引擎在「下一次连接设备」时自行读取，不需要运行时钩子。
    s.applyRuntime();

    s.save();
}

// refreshEnvStatus() 已随环境检测功能移除（停更离线版 2.0.6）。

// 灵动岛七项 → AppSettings（内存），再让外壳重读一遍。
// 🔴 岛是 MainShell 的子控件，设置页拿不到它的指针（也不该拿）—— 走窗口上的
//    Q_INVOKABLE applyIslandSettings()，与 reloadBackground() 是同一条路子。
void SettingsPage::applyIslandLive()
{
    kc::AppSettings& s = kc::AppSettings::current();

    s.islandTheme = m_islandTheme->currentData().toString();
    s.islandRadius = m_islandRadius->value();
    s.islandCollapsed = m_islandCollapsed->currentData().toString();
    s.islandVisibility = m_islandVisibility->currentData().toString();
    s.islandColumns = m_islandColumns->currentData().toInt();
    s.islandDim = m_islandDim->isChecked();
    s.islandLiteMotion = m_islandLite->isChecked();

    if (QWidget* w = window())
        QMetaObject::invokeMethod(w, "applyIslandSettings", Qt::DirectConnection);
}

// 液态玻璃五项 → AppSettings（内存）+ 刷新预览。
// 🔴 不调 refreshStyleSheet()：玻璃参数只被自绘面板读取，重放 QSS 是纯浪费。
void SettingsPage::applyGlassLive()
{
    glass::Style st;
    st.tint = m_glassTint->value() / double(kPercentMax);
    st.frost = m_glassFrost->value();
    st.refraction = m_glassRefraction->value() / double(kPercentMax);
    st.dispersion = m_glassDispersion->value() / double(kPercentMax);
    st.highlight = m_glassHighlight->value() / double(kPercentMax);

    // 光源位置 / 按压形变是运行时状态（指针驱动），不参与持久化，原样继承。
    const glass::Style cur = currentGlassStyle();
    st.lightPos = cur.lightPos;
    st.press = cur.press;

    // 手调出来的值如果恰好等于某一档预设，就回显那一档（而不是一直挂在「自定义」）。
    storeGlassStyle(st, matchGlassPreset(st));

    if (m_glassPreview) m_glassPreview->setStyle(st);
}

void SettingsPage::syncGlassFromSettings()
{
    const kc::AppSettings& s = kc::AppSettings::current();

    m_glassSyncing = true;
    m_glassTint->setValue(qRound(qBound(0.0, s.glassTint, 1.0) * kPercentMax));
    m_glassFrost->setValue(qBound(0, qRound(s.glassFrost), kBlurMax));
    m_glassRefraction->setValue(qRound(qBound(0.0, s.glassRefraction, 1.0) * kPercentMax));
    m_glassDispersion->setValue(qRound(qBound(0.0, s.glassDispersion, 1.0) * kPercentMax));
    m_glassHighlight->setValue(qRound(qBound(0.0, s.glassHighlight, 1.6) * kPercentMax));

    int idx = m_glassPreset->findData(s.glassPreset);
    if (idx < 0) idx = m_glassPreset->findData(QStringLiteral("custom"));
    if (idx >= 0) m_glassPreset->setCurrentIndex(idx);
    m_glassSyncing = false;

    if (m_glassHint) {
        const GlassPreset* p = findGlassPreset(s.glassPreset);
        m_glassHint->setText(p ? p->hint
                               : QStringLiteral("自定义参数：拖动任意滑条都会落到这一档"));
    }
    if (m_glassPreview) m_glassPreview->setStyle(currentGlassStyle());
}

void SettingsPage::loadGlassPreviewBackdrop()
{
    if (!m_glassPreview) return;

    // 预览必须跟真实观感对得上，所以优先用用户自己的壁纸；没有壁纸才退回控件
    // 内置的「环境光」兜底图。用内置图骗自己 = 调出来的参数在真机上不对。
    const QString path = kc::AppSettings::current().resolveBackgroundPath();
    if (path == m_glassBackdropPath && !path.isEmpty()) return;   // 同一张，不重解码
    m_glassBackdropPath = path;

    QSize target = m_glassPreview->size();
    if (target.isEmpty() || target.width() < 32) target = QSize(560, 136);

    if (!path.isEmpty()) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize orig = reader.size();
        if (orig.isValid()) {
            // 解码时就缩到预览尺寸：4K 壁纸按原尺寸解出来再缩是纯浪费。
            QSize scaled = orig;
            scaled.scale(target, Qt::KeepAspectRatioByExpanding);
            reader.setScaledSize(scaled);
        }
        const QImage img = reader.read();
        if (!img.isNull()) {
            m_glassPreview->setRawBackdrop(img);
            return;
        }
    }
    // 空图 = 让控件自己造一张环境光（否则预览是一块纯色，什么都看不出来）。
    m_glassPreview->setRawBackdrop(QImage());
}

void SettingsPage::applyAccentColor(const QColor& c)
{
    kc::AppSettings& s = kc::AppSettings::current();
    s.accentColor = c.isValid() ? c.name(QColor::HexRgb) : QString();

    // setAccent 内部会重建 token 表 + 重放样式表 + 发 themeChanged。
    ThemeManager::instance().setAccent(c);
    syncAccentChips();

    // 重放样式表会给整棵树发 StyleChange，但非当前页的控件不会立刻重绘；
    // 显式刷一次窗口，点完色点马上就能看到效果（与 applyLive 同因）。
    if (QWidget* w = window()) w->update();

    // 强调色没有「拖到一半」的中间态，点一下就该定下来 —— 直接落盘。
    s.save();
}

void SettingsPage::syncAccentChips()
{
    const QColor cur = ThemeManager::instance().accent();
    const QString curHex = cur.isValid() ? cur.name(QColor::HexRgb).toUpper()
                                         : QStringLiteral("#0A84FF");
    for (QPushButton* chip : m_accentChips) {
        if (!chip) continue;
        // 色点自己的颜色存在 property 里：stylesheet 里的 %1 在 setStyleSheet
        // 时已经展开成字面量，反查字符串不可靠。
        const QString hex = chip->property("accentHex").toString();
        const QSignalBlocker block(chip);   // setChecked 不该再触发 clicked
        chip->setChecked(hex.compare(curHex, Qt::CaseInsensitive) == 0);
    }
}

void SettingsPage::applyAndSave()
{
    persist();

    // 外壳动态背景即时生效（QSS 改不动窗底，必须让 MainShell 重画）。
    if (QWidget* w = window())
        QMetaObject::invokeMethod(w, "reloadBackground", Qt::DirectConnection);

    // 🔴 「界面透明 / 卡片不透明度」必须重放样式表：这两个值是以 alpha 形式替换进
    //    QSS 文本的（Qt QSS 没有 CSS 变量），只 save() 不重新 apply = 滑条动了界面不动。
    ThemeManager::instance().refreshStyleSheet();
    if (QWidget* w = window()) w->update();

    // iOS 玻璃横幅（系统级浮层），比页内通知条更接近 iOS 的反馈方式。
    Toast::show(QStringLiteral("设置已保存"), Toast::Kind::Success);
}

void SettingsPage::onActivated()
{
    loadFromSettings();
    // 账号名显示已随登录体系移除（停更离线版 2.0.6，服务器关停）。
}

} // namespace kc::ui
