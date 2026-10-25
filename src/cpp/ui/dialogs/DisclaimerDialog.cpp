#include "ui/dialogs/DisclaimerDialog.h"

#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QWidget>

namespace kc::ui {
namespace {

/// 一段条款：标题 + 富文本正文。
struct LegalSection {
    const char* title;
    const char* body;   // QLabel 富文本（支持 <b> <ul> <li> <br>）
};

// 🔴 富文本里**不要写颜色**：深浅主题共用同一份文本，写死的颜色在另一边必然
//    低对比度。着色一律交给 QSS 的 #legalSectionTitle / #legalBody。
const LegalSection kSections[] = {
    { "一、使用限制",
      "<ul style='margin:0; padding-left:18px;'>"
      "<li>本工具仅供学习交流与个人设备维护使用，不得用于任何商业用途。</li>"
      "<li>仅限对本人合法所有、或已获得所有者明确授权的设备使用。</li>"
      "<li>严禁用于来历不明、遗失或被盗的设备；严禁用于绕开设备的防盗与账号保护机制。</li>"
      "</ul>" },

    { "二、法律责任（重点）",
      "<p style='margin:0 0 6px 0'>下列行为不只是违约，还可能构成违法甚至犯罪，责任由您本人独立承担：</p>"
      "<ul style='margin:0; padding-left:18px;'>"
      "<li>未经授权获取、修改他人设备数据或对其实施控制 —— 《刑法》第 285 条（非法获取计算机"
      "信息系统数据、非法控制计算机信息系统罪；提供侵入、非法控制计算机信息系统的程序、工具罪）。</li>"
      "<li>对他人设备的系统功能、数据或应用程序进行删除、修改、增加、干扰 —— 《刑法》第 286 条"
      "（破坏计算机信息系统罪）。</li>"
      "<li>恶意刷写、格式化、变砖他人设备 —— 《刑法》第 275 条（故意毁坏财物罪），并须依"
      "《民法典》承担赔偿责任。</li>"
      "<li>解锁、刷机、重置后转卖或自用他人遗失、被盗设备 —— 《刑法》第 264 条（盗窃罪）。</li>"
      "<li>明知设备系犯罪所得仍代为刷机、改机、解锁、销赃 —— 《刑法》第 312 条"
      "（掩饰、隐瞒犯罪所得、犯罪所得收益罪）。</li>"
      "<li>读取、收集、出售或提供他人设备中的个人信息 —— 《刑法》第 253 条之一"
      "（侵犯公民个人信息罪）。</li>"
      "<li>明知他人利用信息网络实施犯罪仍提供技术支持、工具、账号等帮助 —— 《刑法》第 287 条之二"
      "（帮助信息网络犯罪活动罪）。</li>"
      "</ul>"
      "<p style='margin:6px 0 0 0'>另：《反电信网络诈骗法》第 14 条禁止制造、买卖、提供或使用"
      "「批量账号、网络地址自动切换系统」等设备、软件；批量改机、批量注册、批量登录即落入该禁止"
      "情形（第 42 条：没收违法所得、罚款，情节严重者可行政拘留）。未达刑事标准的，仍可能依"
      "《网络安全法》第 27 条、《治安管理处罚法》关于侵入、干扰计算机信息系统的规定被行政处罚。</p>" },

    { "三、刷机风险",
      "<ul style='margin:0; padding-left:18px;'>"
      "<li>设备变砖（无法正常启动）</li>"
      "<li>数据丢失（个人文件、应用、设置等）</li>"
      "<li>失去官方保修资格</li>"
      "<li>硬件损坏（极端情况下）</li>"
      "</ul>"
      "<p style='margin:6px 0 0 0'>使用本工具即表示您已了解并接受上述风险。开发者不对任何因使用"
      "本工具造成的直接或间接损失承担责任（法律强制性规定不得排除的除外）。操作前务必备份重要"
      "数据并确认了解步骤，过程中请保持设备电量充足，避免中途断电。</p>" },

    { "四、个人信息与隐私",
      "<ul style='margin:0; padding-left:18px;'>"
      "<li>仅收集账号运行所必需的最少信息：用户名、邮箱、登录记录、功能调用记录。</li>"
      "<li>不会读取或上传您设备中的照片、通讯录、短信、应用列表等任何内容，也不会回传您处理的"
      "固件与分区镜像。</li>"
      "<li>登录记录中会包含本次登录的来源 IP 地址，仅用于识别盗号、异常登录与滥用行为；该字段"
      "只有最高权限的开发者账号可以查看，不会下发给其它管理角色，也不会向第三方提供。</li>"
      "<li>本工具不含第三方统计与广告组件。</li>"
      "</ul>" },

    { "五、账号、注销与未成年人",
      "<ul style='margin:0; padding-left:18px;'>"
      "<li>您可随时联系我们查询、更正、删除个人信息或申请注销账号，我们将在核验后依法处理。</li>"
      "<li>本工具面向成年人。未满 18 周岁请在监护人陪同下使用；未满 14 周岁须由监护人代为同意。</li>"
      "</ul>" },

    { "六、作者承诺",
      "<p style='margin:0 0 6px 0'>作者：小坤ovo</p>"
      "<p style='margin:0'>承诺不会在工具中植入任何恶意代码，不会收集无关的个人信息，也不会以任何"
      "方式向您索要账号密码；不会为任何违法用途提供帮助或规避手段。本工具与任何设备厂商无隶属、"
      "代理或合作关系；相关商标归各自权利人所有。</p>" },
};

} // namespace

DisclaimerDialog::DisclaimerDialog(bool enforce, QWidget* parent)
    : AnimatedDialog(parent), m_enforce(enforce)
{
    setWindowTitle(QStringLiteral("使用条款与隐私说明"));
    setModal(true);
    // 只给这一个弹窗上底色（objectName 限定）：全局 `QWidget { background: transparent }`
    // 会让顶层窗口的客户区透出调色板色，条款正文压在灰底上很难读。
    setObjectName(QStringLiteral("disclaimerDialog"));
    // 尺寸改成「可缩放 + 有下限」，而不是写死 580×640：
    // 老版本在 1366×768 的小屏上占掉八成高度，条款又挤成一条竖缝。
    setMinimumSize(560, 500);
    resize(660, 720);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ── 顶部品牌头 ──────────────────────────────────────────────────────
    auto* header = new QFrame(this);
    header->setObjectName(QStringLiteral("legalHeader"));
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(22, 18, 22, 16);
    headerRow->setSpacing(14);

    if (QPixmap logo(QStringLiteral(":/icons/logo.png")); !logo.isNull()) {
        auto* logoLabel = new QLabel(header);
        logoLabel->setPixmap(logo.scaled(44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        logoLabel->setFixedSize(44, 44);
        logoLabel->setAlignment(Qt::AlignCenter);
        headerRow->addWidget(logoLabel, 0, Qt::AlignTop);
    }

    auto* titleCol = new QVBoxLayout;
    titleCol->setSpacing(4);
    auto* title = new QLabel(QStringLiteral("使用条款与隐私说明"), header);
    title->setObjectName(QStringLiteral("legalTitle"));
    titleCol->addWidget(title);
    auto* subtitle = new QLabel(
        QStringLiteral("使用本工具前，请先读完下面这六节。它们决定你能用它做什么、"
                       "不能做什么，以及我们怎么处理你的数据。"),
        header);
    subtitle->setObjectName(QStringLiteral("legalSubtitle"));
    subtitle->setWordWrap(true);
    titleCol->addWidget(subtitle);
    headerRow->addLayout(titleCol, 1);

    auto* version = new QLabel(QStringLiteral("v%1").arg(LegalVersion), header);
    version->setObjectName(QStringLiteral("legalVersionPill"));
    version->setAlignment(Qt::AlignCenter);
    headerRow->addWidget(version, 0, Qt::AlignTop);

    root->addWidget(header);

    // ── 摘要卡：先给三句话，再让人去读细则 ───────────────────────────────
    auto* summary = new QFrame(this);
    summary->setObjectName(QStringLiteral("legalSummary"));
    auto* summaryLayout = new QVBoxLayout(summary);
    summaryLayout->setContentsMargins(18, 14, 18, 14);
    summaryLayout->setSpacing(0);
    auto* summaryText = new QLabel(summary);
    summaryText->setObjectName(QStringLiteral("legalSummaryText"));
    summaryText->setTextFormat(Qt::RichText);
    summaryText->setWordWrap(true);
    summaryText->setText(
        QStringLiteral(
            "<p style='margin:0 0 7px 0'><b>仅限本人设备</b> —— 未经授权操作他人设备，"
            "可能构成犯罪，责任由你本人承担。</p>"
            "<p style='margin:0 0 7px 0'><b>数据最小化</b> —— 只收集账号运行必需的信息，"
            "不读取、不上传你的照片、通讯录、固件与分区镜像。</p>"
            "<p style='margin:0'><b>刷机有风险</b> —— 变砖、数据丢失、失去保修均有可能，"
            "操作前请务必备份。</p>"));
    summaryLayout->addWidget(summaryText);
    auto* summaryWrap = new QWidget(this);
    auto* summaryWrapLayout = new QVBoxLayout(summaryWrap);
    summaryWrapLayout->setContentsMargins(22, 14, 22, 0);
    summaryWrapLayout->setSpacing(0);
    summaryWrapLayout->addWidget(summary);
    root->addWidget(summaryWrap);

    // ── 正文：滚动区分节 ────────────────────────────────────────────────
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("legalScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QScrollArea::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* host = new QWidget(scroll);
    host->setObjectName(QStringLiteral("legalHost"));
    auto* hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(22, 6, 22, 10);
    hostLayout->setSpacing(18);

    for (const LegalSection& section : kSections) {
        auto* block = new QWidget(host);
        auto* blockLayout = new QVBoxLayout(block);
        blockLayout->setContentsMargins(0, 0, 0, 0);
        blockLayout->setSpacing(7);

        auto* head = new QLabel(QString::fromUtf8(section.title), block);
        head->setObjectName(QStringLiteral("legalSectionTitle"));
        blockLayout->addWidget(head);

        auto* body = new QLabel(QString::fromUtf8(section.body), block);
        body->setObjectName(QStringLiteral("legalBody"));
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        blockLayout->addWidget(body);

        hostLayout->addWidget(block);
    }

    auto* versionNote = new QLabel(
        QStringLiteral("条款版本 v%1 · 更新日期 2026-09-24 · 可在「设置 › 关于」随时重新查看")
            .arg(LegalVersion),
        host);
    versionNote->setObjectName(QStringLiteral("dialogFooter"));
    versionNote->setWordWrap(true);
    hostLayout->addWidget(versionNote);
    hostLayout->addStretch(1);

    scroll->setWidget(host);
    scroll->verticalScrollBar()->setValue(0);   // 打开必须停在顶部（对应 C# ScrollToTop）
    root->addWidget(scroll, 1);

    // ── 底部：勾选 + 按钮 ───────────────────────────────────────────────
    auto* footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("legalFooter"));
    auto* footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(22, 12, 22, 16);
    footerLayout->setSpacing(10);

    // 同意按钮：enforce 模式下必须先勾选才能点 —— 这是最轻的一道「真的读了」的门槛，
    // 又不像「必须滚到底」那样把正常用户卡住。
    auto* agree = new QPushButton(m_enforce ? QStringLiteral("同意并继续")
                                            : QStringLiteral("我已阅读"),
                                  footer);
    agree->setObjectName(QStringLiteral("primaryButton"));
    agree->setCursor(Qt::PointingHandCursor);
    agree->setMinimumHeight(38);

    auto* agreeCheck = new QCheckBox(
        QStringLiteral("我已阅读并理解上述全部条款，且仅将本工具用于本人合法所有的设备"), footer);
    agreeCheck->setObjectName(QStringLiteral("legalAgree"));
    agreeCheck->setChecked(!m_enforce);
    agreeCheck->setVisible(m_enforce);
    agree->setEnabled(!m_enforce);

    auto* disagree = new QPushButton(QStringLiteral("不同意"), footer);
    disagree->setMinimumHeight(38);
    disagree->setVisible(m_enforce);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setSpacing(10);
    buttonRow->addWidget(agreeCheck, 1);
    buttonRow->addWidget(disagree);
    buttonRow->addWidget(agree);
    footerLayout->addLayout(buttonRow);

    auto* footerHint = new QLabel(
        m_enforce ? QStringLiteral("勾选后「同意并继续」才会亮起；不同意将退出程序。")
                  : QStringLiteral("本窗口仅供查看，关闭不影响使用。"),
        footer);
    footerHint->setObjectName(QStringLiteral("dialogHint"));
    footerHint->setWordWrap(true);
    footerLayout->addWidget(footerHint);

    root->addWidget(footer);

    connect(agreeCheck, &QCheckBox::toggled, agree, &QPushButton::setEnabled);
    connect(agree, &QPushButton::clicked, this, [this] {
        m_agreed = true;
        accept();
    });
    connect(disagree, &QPushButton::clicked, this, &QDialog::reject);
    // enforce 模式下直接关窗 = reject → 调用方（main.cpp）负责退出程序。
}

} // namespace kc::ui
