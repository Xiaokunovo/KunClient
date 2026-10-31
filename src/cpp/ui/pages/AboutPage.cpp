#include "ui/pages/AboutPage.h"

#include "kc/version.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/NotificationBar.h"

#include <QColor>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

constexpr const char* kWebsiteUrl = "https://kunawa.cn";

// ── 彩蛋参数：连点版本号 7 次 ───────────────────────────────────────────────
//  触发方式见 AboutPage::eventFilter / triggerVersionEgg；改门槛只动这两个常量。
constexpr int kVersionClickTarget = 7;          ///< 需要连点的次数
constexpr qint64 kVersionClickWindowMs = 800;   ///< 相邻两次点击的最大间隔
constexpr int kVersionPulseOnMs = 160;          ///< 脉冲第一段（强调色）时长
constexpr int kVersionPulseSettleMs = 260;      ///< 脉冲第二段（回落到弱化色）时长

// 鸣谢 / 反馈人员名单：口径与官网 website/src/components/Showcase.tsx 的 Team()
// 一致，C# 参照页 tests/src/KunClient.App/Pages/AboutPage.xaml 同源。
//
// 为什么必须分成两块：鸣谢偏「代码 / 资源 / 测试」投入，反馈偏「缺陷与体验
// 问题上报」——两者是不同身份，混在一张表里会把这层区分抹掉。
//
// ⚠️ 中文/emoji 一律走 QString::fromUtf8（本文件是 UTF-8）。窄字面量经
//    QLatin1String 会把 UTF-8 多字节解成 "Â©" 这种乱码，同 copyrightText() 的红线。
struct CreditEntry {
    const char* name;
    const char* note;
};

constexpr CreditEntry kCreditList[] = {
    {"香菜", "服务器支持 · QQ: 3328761646"},
    {"魚", "UI 设计 · QQ: 1708315952"},
    {"张天师", "QQ: 3098258542"},
    {"Ramdisk", "QQ: 2669016593"},
    {"小楠xiaonan", "QQ: 487825006"},
    {"清辞", "QQ: 3756541352"},
    {"皓皓的小月", "QQ: 1541420886"},
    {"GitHub", "技术平台"},
};

constexpr CreditEntry kFeedbackList[] = {
    {"河南省省长-青墨", "QQ: 2117598242"},
    {"♂甩@棍🇨🇳", "QQ: 2433862624"},
    {"是宝不是贝^", "QQ: 1781653834"},
};

// 官方联系方式（C# 参照页 AboutPage.xaml 的「联系方式」块）。
constexpr CreditEntry kContactList[] = {
    {"QQ", "2089449185"},
    {"Email", "2089449185@qq.com"},
};

QString copyrightText()
{
    // © 是非 ASCII 字符：窄字面量经 QLatin1String 会把 UTF-8 字节解成 "Â©"，
    // 必须走 QStringLiteral（源码即 UTF-8，与文件内其他中文字符串同口径）。
    return QStringLiteral("© 2024-2026 KunClient");
}

} // namespace

AboutPage::AboutPage(QWidget* parent)
    : BasePage(parent)
{
    setTitle(QStringLiteral("关于"));
    buildUi();
}

void AboutPage::buildUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    m_notify = new NotificationBar(this);
    m_notify->hide();
    registerNotifyBar(m_notify);
    layout->addWidget(m_notify);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(scroll, 1);

    auto* host = new QWidget;
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(10);

    // ── 品牌区：logo + 名称 ──────────────────────────────────────────────
    auto* brandArea = new QVBoxLayout;
    brandArea->setSpacing(6);
    brandArea->setAlignment(Qt::AlignHCenter);

    if (QFile::exists(QStringLiteral(":/icons/logo.png"))) {
        auto* logo = new QLabel(host);
        logo->setPixmap(QPixmap(QStringLiteral(":/icons/logo.png"))
                            .scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logo->setAlignment(Qt::AlignCenter);
        brandArea->addWidget(logo);
    }

    auto* name = new QLabel(QStringLiteral("KunClient"), host);
    name->setAlignment(Qt::AlignCenter);
    QFont nameFont = name->font();
    nameFont.setBold(true);
    nameFont.setPointSize(16);
    name->setFont(nameFont);
    brandArea->addWidget(name);

    auto* subtitle = new QLabel(QStringLiteral("高通 EDL 线刷工具"), host);
    subtitle->setAlignment(Qt::AlignCenter);
    brandArea->addWidget(subtitle);
    hostLayout->addLayout(brandArea);

    // ── 版本信息 ─────────────────────────────────────────────────────────
    auto* versionCard = makeCard(QStringLiteral("版本信息"));
    auto* versionForm = new QFormLayout;
    versionForm->setLabelAlignment(Qt::AlignRight);
    versionForm->setHorizontalSpacing(10);
    versionForm->setVerticalSpacing(6);

    // 与 C# 同一红线：值里不带前缀 —— 左边标签已经是「版本:」。
    m_versionLabel = new QLabel(QString::fromLatin1(kc::kVersionString), versionCard);
    versionForm->addRow(QStringLiteral("版本:"), m_versionLabel);

    // ── 彩蛋：连点版本号 7 次 ───────────────────────────────────────────────
    //  给版本号标签装事件过滤器（判定逻辑在 eventFilter()）、换成手型光标当暗号。
    //  脉冲用 QTimer + QSS 推进（见 triggerVersionEgg）—— 绝不动 geometry：
    //  这个标签归 QFormLayout 管，改几何会被布局下一次重排推回去，中途还会闪。
    m_versionLabel->setCursor(Qt::PointingHandCursor);
    m_versionLabel->installEventFilter(this);

    m_versionPulseTimer = new QTimer(this);
    m_versionPulseTimer->setSingleShot(true);
    connect(m_versionPulseTimer, &QTimer::timeout, this, [this] {
        if (!m_versionLabel) return;
        // 第一段结束 → 第二段（主色落到亮一档的 Accent2），再等一拍收尾。
        if (m_versionPulseStage == 1) {
            m_versionPulseStage = 2;
            const QColor soft = ThemeManager::instance().color(ColorToken::Accent2);
            m_versionLabel->setStyleSheet(
                QStringLiteral("color: %1; font-weight: 600;").arg(soft.name()));
            m_versionPulseTimer->start(kVersionPulseSettleMs);
            return;
        }
        // 收尾：清掉局部 QSS，颜色 / 字重回到全局样式表的继承态。
        m_versionPulseStage = 0;
        m_versionLabel->setStyleSheet(QString());
    });

    auto* engine = new QLabel(QString::fromLatin1(kc::kEdlEngineVersion), versionCard);
    versionForm->addRow(QStringLiteral("EDL 引擎:"), engine);

    auto* website = new QLabel(
        QStringLiteral("<a href=\"%1\">%1</a>").arg(QLatin1String(kWebsiteUrl)), versionCard);
    website->setOpenExternalLinks(true);
    website->setTextInteractionFlags(Qt::TextBrowserInteraction);
    versionForm->addRow(QStringLiteral("官网:"), website);

    auto* author = new QLabel(QStringLiteral("小坤ovo"), versionCard);
    versionForm->addRow(QStringLiteral("作者:"), author);

    static_cast<QVBoxLayout*>(versionCard->layout())->addLayout(versionForm);

    // ── 检查更新：已随更新功能移除（停更离线版 2.0.6，服务器关停）──────────

    // ── 鸣谢 / 反馈人员 ──────────────────────────────────────────────────
    // 两张名片同款排布：左列姓名（右对齐成一列）+ 右列说明，与「版本信息」
    // 卡同一口径（QFormLayout），不另外发明列表样式。
    const auto makeRosterCard = [this](const QString& title, const CreditEntry* rows,
                                       int count) {
        auto* card = makeCard(title);
        auto* form = new QFormLayout;
        form->setLabelAlignment(Qt::AlignRight);
        form->setHorizontalSpacing(10);
        form->setVerticalSpacing(6);
        for (int i = 0; i < count; ++i) {
            auto* note = new QLabel(QString::fromUtf8(rows[i].note), card);
            form->addRow(QString::fromUtf8(rows[i].name), note);
        }
        static_cast<QVBoxLayout*>(card->layout())->addLayout(form);
        return card;
    };

    // ── 联系方式 ─────────────────────────────────────────────────────────
    // 名单口径同 C# 参照页 AboutPage.xaml 第 128-151 行（QQ / Email 两行）。
    auto* contactCard = makeRosterCard(QStringLiteral("联系方式"), kContactList,
                                       int(sizeof(kContactList) / sizeof(kContactList[0])));

    auto* creditCard =
        makeRosterCard(QStringLiteral("鸣谢"), kCreditList,
                       int(sizeof(kCreditList) / sizeof(kCreditList[0])));
    auto* feedbackCard =
        makeRosterCard(QStringLiteral("反馈人员"), kFeedbackList,
                       int(sizeof(kFeedbackList) / sizeof(kFeedbackList[0])));

    // ── 「关于收费」卡片已删除（2026-10-21）─────────────────────────────
    // 付费体系整体下线后，这张卡还写着「工具收费只会用于维护工具与维护服务器」——
    // 与「全部功能免费」直接冲突，用户要求删掉。以后若恢复收费再按新口径加回来。

    // ── 免责声明（摘要）──────────────────────────────────────────────────
    auto* legalCard = makeCard(QStringLiteral("免责声明"));
    auto* disclaimer = new QLabel(legalCard);
    disclaimer->setWordWrap(true);
    disclaimer->setText(
        QStringLiteral("本工具仅供学习交流与合法的设备维护使用。刷机操作存在风险，"
                       "包括但不限于设备无法开机、数据丢失、失去保修等。使用本工具即表示"
                       "您已了解并接受上述风险，并自行承担相应后果。")
        + QStringLiteral("\n"));
    auto* warning = new QLabel(QStringLiteral("请在操作前务必备份重要数据。"), legalCard);
    warning->setWordWrap(true);
    auto* legalLayout = static_cast<QVBoxLayout*>(legalCard->layout());
    legalLayout->addWidget(disclaimer);
    legalLayout->addWidget(warning);

    // ── 版权行 ───────────────────────────────────────────────────────────
    auto* copyright = new QLabel(copyrightText(), host);
    copyright->setAlignment(Qt::AlignCenter);
    copyright->setEnabled(false);

    hostLayout->addWidget(versionCard);
    // 顺序：联系方式 → 鸣谢 → 反馈人员 → 免责声明
    // （先「怎么找到我们」「谁帮了忙」，最后「责任边界」）。
    hostLayout->addWidget(contactCard);
    hostLayout->addWidget(creditCard);
    hostLayout->addWidget(feedbackCard);
    hostLayout->addWidget(legalCard);
    hostLayout->addWidget(copyright);
    hostLayout->addStretch();
    scroll->setWidget(host);
}

// ─────────────────────────────────────────────────────────────────────────────
//  彩蛋：连点版本号 7 次
//
//  维护者须知（这不是 bug）：
//    · 触发点 = 「版本信息」卡片里那一行的版本号标签；
//    · 门槛 = 连续 7 次左键点击，**相邻两次间隔 < 800ms**，任何一次超时都从头计数；
//    · 效果 = 版本号标签一次样式脉冲（强调色 → 回落）。（Toast 已于 2026-10-31 移除）
//  为什么用 QTimer + QSS 而不是 QPropertyAnimation：这个标签由 QFormLayout 摆位，
//  改 geometry 的动画会被布局下一次重排无声吃掉，还会在中途闪一下错位
//  （见 Motion.h 里「布局管辖的控件不能用 reveal/conceal」那条红线的同一原因）。
// ─────────────────────────────────────────────────────────────────────────────
bool AboutPage::eventFilter(QObject* watched, QEvent* event)
{
    // 只认版本号标签自己的左键**按下**。
    // 🔴 不用 MouseButtonRelease：QLabel 在 textInteractionFlags 为 NoTextInteraction
    //    时会把 press / release 都 ignore() 掉，事件随后沿父链上传；按下这一拍
    //    一定先送到指针下的控件（这里就是标签）并先过事件过滤器，最稳。
    //    对一个连点彩蛋来说「按下即算一次」与「松开才算」没有体验差异。
    if (watched == m_versionLabel && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) return BasePage::eventFilter(watched, event);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // 超时（或这是第一下）就从头数；连续点击才累加。
        if (m_versionClickCount == 0 || now - m_versionLastClickMs >= kVersionClickWindowMs) {
            m_versionClickCount = 1;
        } else {
            ++m_versionClickCount;
        }
        m_versionLastClickMs = now;

        if (m_versionClickCount >= kVersionClickTarget) {
            m_versionClickCount = 0;
            triggerVersionEgg();
        }
        // 不 return true：这个标签本来也不消费点击，照常让它走完流程。
    }
    return BasePage::eventFilter(watched, event);
}

void AboutPage::triggerVersionEgg()
{
    if (!m_versionLabel) return;

    // 文案口径：贴着「刷机工具」主题的开发者玩笑（9008 = 高通 EDL 的 QDLoader 端口）。

    // 样式脉冲：强调色 + 加粗 → 亮一档 → 清空局部 QSS 回落到全局样式表。
    // 只碰 color / font-weight，字形宽度变化由 QFormLayout 自己重排，不会与布局打架。
    m_versionPulseTimer->stop();
    m_versionPulseStage = 1;
    const QColor accent = ThemeManager::instance().color(ColorToken::Accent);
    m_versionLabel->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 700;").arg(accent.name()));
    m_versionPulseTimer->start(kVersionPulseOnMs);
}

QFrame* AboutPage::makeCard(const QString& title)
{
    auto* card = new QFrame(this);
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

// checkForUpdate() 已随更新功能移除（停更离线版 2.0.6，服务器关停）。

} // namespace kc::ui
