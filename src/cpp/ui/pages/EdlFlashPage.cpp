#include "ui/pages/EdlFlashPage.h"

#include "ui/MainWindow.h"
#include "ui/theme/ThemeManager.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace kc::ui {

EdlFlashPage::EdlFlashPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("Qualcomm"));

    // 子树标记。2026-10-14 ColorOS 17 改造时曾用它配 QSS 里的「#edlLegacyPage …」
    // 保护区，把内嵌 swt-edl 旧窗的圆角钉回改造前的值（红线「edl刷写界面不要改
    // 布局」）。2026-10-16 用户放开放弃该红线、要求一并改造 EDL 界面，保护区段已
    // 从 light.qss / dark.qss 删除，这里不再有 QSS 规则挂靠 —— 仅作历史标记保留，
    // 便于以后按子树定位内嵌旧窗。名字保留以免外部脚本/测试按名查找时失配。
    setObjectName(QStringLiteral("edlLegacyPage"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // 顶部快捷条：提示 + 「FDL 进入 EDL」独立入口。
    // 原「云端引导库」按钮 2026-10-16 已并入内嵌 EDL 窗的「高通引导选择框」那一行
    // （见 ui/MainWindow.cpp 的 programmerRow）。
    // FDL（小米 fastboot → 9008）：2026-10-17 用户要求从右下角「重启设备 ▾」菜单里
    // 提出来做成独立按钮 —— 它是一条完整的入口流程，藏在重启菜单里既不好找，
    // 语义上也和「重启」不搭。点击仍然弹流程窗并自动开跑（见 openFdlFlow）。
    auto* quickBar = new QFrame(this);
    quickBar->setObjectName(QStringLiteral("quickBar"));
    quickBar->setFixedHeight(38);
    auto* quickLayout = new QHBoxLayout(quickBar);
    quickLayout->setContentsMargins(14, 0, 14, 0);
    quickLayout->setSpacing(8);

    auto* hint = new QLabel(QStringLiteral("Qualcomm 9008 · EDL 刷写"), quickBar);
    hint->setObjectName(QStringLiteral("statusLabel"));
    quickLayout->addWidget(hint);
    quickLayout->addStretch();

    // FDL（小米 fastboot → 9008）独立按钮（见上方注释）。
    auto* fdlButton = new QPushButton(QStringLiteral("FDL 进入 EDL"), quickBar);
    fdlButton->setObjectName(QStringLiteral("secondaryButton"));
    fdlButton->setCursor(Qt::PointingHandCursor);
    fdlButton->setToolTip(QStringLiteral(
        "小米 fastboot 通道：取 token → 卡密求签 → stage → oem edl，直接进 9008"));
    connect(fdlButton, &QPushButton::clicked, this, [this] {
        if (m_edl) m_edl->startFdlFlow();
    });
    quickLayout->addWidget(fdlButton);

    // 浮条的外边距交给容器：QSS 的 margin 是向内压内容盒，写在 #quickBar 上
    // 会把条里的按钮挤扁（2026-10-17 实测）。
    auto* quickHost = new QWidget(this);
    auto* quickHostLayout = new QHBoxLayout(quickHost);
    quickHostLayout->setContentsMargins(12, 8, 12, 6);
    quickHostLayout->setSpacing(0);
    quickHostLayout->addWidget(quickBar);
    layout->addWidget(quickHost);

    // ── 内嵌 EDL 窗：**延迟到第一次显示才建**（2026-10-18）─────────────────
    // 🔴 这个窗构造一次要 2.1~2.8s，而且几乎全花在 `new QTableWidget` 那一行
    //    （分段实测：页签 0ms / 串口行 2ms / **QTableWidget 2126ms** / 其余 5ms）。
    //    那是 Qt 在本进程里第一次初始化 QAbstractItemView + 表头 + Windows 无障碍
    //    桥的一次性开销，跟本页代码无关 —— 但它原来被算进了冷启动，
    //    「主界面已构造」的 3.3s 里 2/3 是它。
    //
    //    现在：构造期只摆一个占位标签，真正建窗放到第一次显示（showEvent /
    //    onActivated）。冷启动立刻少 2.2s；代价是第一次点「Qualcomm」多等这一下，
    //    而那时用户本来就是要刷机，等一下是可预期的。
    m_edlHost = new QWidget(this);
    auto* hostLayout = new QVBoxLayout(m_edlHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    m_edlPlaceholder = new QLabel(QStringLiteral("正在准备 EDL 引擎…"), m_edlHost);
    m_edlPlaceholder->setObjectName(QStringLiteral("edlPlaceholder"));
    m_edlPlaceholder->setAlignment(Qt::AlignCenter);
    hostLayout->addWidget(m_edlPlaceholder);
    layout->addWidget(m_edlHost, 1);

    // EDL 页自带完整左栏（配置卡）与日志，不依赖外壳共用栏。
}

void EdlFlashPage::showEvent(QShowEvent* event)
{
    BasePage::showEvent(event);
    ensureEdlBuilt();
}

void EdlFlashPage::onActivated()
{
    BasePage::onActivated();
    ensureEdlBuilt();
}

void EdlFlashPage::ensureEdlBuilt()
{
    if (m_edlBuilt) return;
    m_edlBuilt = true;

    // 以普通子控件形式嵌入：Qt::Widget 去掉 QMainWindow 的窗口属性，
    // 让它老老实实当本页的一个子部件（不再是独立顶层窗口）。
    // embedded=true：主题由外壳 ThemeManager 驱动，本窗不碰全局样式表/QSettings。
    m_edl = new swt::edl::MainWindow(this, /*embedded=*/true);
    m_edl->setWindowFlags(Qt::Widget);
    // 🔴 内嵌后必须**忽略**它的尺寸诉求（2026-10-17）：
    //    这个窗是从 C# 移植的独立窗口，自带 `resize(1152,748)` + `setMinimumSize(1024,700)`。
    //    挂在 QVBoxLayout 里时，布局会按它的 minimumSizeHint 给它留位置 ——
    //    窗口一窄/一矮（用户 125% 缩放下 1309×732 物理 = 1047×586 逻辑），
    //    内嵌窗就顶出页面：右侧卡片溢出、文字被切在窗口边上，底部状态栏和跑马灯被裁。
    //    Ignored 让它「要多大给多大」，内容超出时在窗内自己挤，不再去挤外壳。
    m_edl->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // 内嵌后无需无边框 + 半透明（那是独立窗口的玻璃效果）；这里用不透明，
    // 否则在父页背景上会透出下层、且无边框会让它丢失标题栏拖动以外的全部窗口语义。
    m_edl->setAttribute(Qt::WA_TranslucentBackground, false);
    // 隐藏内层标题栏（外壳已有全局标题栏，双标题栏是视觉冗余）。
    m_edl->setEmbeddedMode(true);

    // 🔴 主题转发：内嵌 EDL 窗里分区表状态列等自绘颜色由它自己的 m_darkTheme
    //    决定（QTableWidgetItem 的 foreground 是设上去就定格的），外壳切主题必须
    //    转发进来；日志那侧由 kc::ui::LogView 自己接 ThemeManager::themeChanged
    //    重绘，这里一并兜底。历史教训：浅色外壳 + 深色色板 = 正文 #d8e1eb 画在
    //    浅色背景上，整片日志发白看不见（2026-10-12 用户截图）。
    //    EdlFlashPage.h 里 edlWindow() 的注释（「如转发主题切换」）说的就是这条线。
    m_edl->applyTheme(ThemeManager::instance().isDark());
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](bool dark) { if (m_edl) m_edl->applyTheme(dark); });

    // 塞进占位容器、把占位标签摘掉。
    // 🔴 用 QBoxLayout 而不是 QLayout：`QLayout::addWidget` 没有 (widget, stretch)
    //    这个重载，写成 `layout()->addWidget(m_edl, 1)` 会直接编译不过。
    if (auto* host = qobject_cast<QBoxLayout*>(m_edlHost->layout())) {
        if (m_edlPlaceholder) {
            host->removeWidget(m_edlPlaceholder);
            m_edlPlaceholder->deleteLater();
            m_edlPlaceholder = nullptr;
        }
        host->addWidget(m_edl, 1);
    }
}

EdlFlashPage::~EdlFlashPage() = default;

} // namespace kc::ui
