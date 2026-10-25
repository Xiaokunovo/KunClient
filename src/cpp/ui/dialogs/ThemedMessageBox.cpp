#include "ui/dialogs/ThemedMessageBox.h"

#include "ui/dialogs/AnimatedDialog.h"

#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

/// 卡片宽度：440 是「两行能读完、长文案不挤」的实测平衡点（登录框 460 同级）。
constexpr int kCardWidth = 440;
/// 徽标（圆形底座）直径。
constexpr int kBadgeSize = 30;
/// 文案高度上限占屏幕可用高度的比例 —— 超过才退化出滚动条。
constexpr double kMaxTextRatio = 0.62;

/// 标准按钮的中文文案。🔴 与 .NET ThemedMessageBox 的文案保持一致（三端口径统一）。
QString buttonLabel(QMessageBox::StandardButton b)
{
    switch (b) {
    case QMessageBox::Ok:       return QStringLiteral("确定");
    case QMessageBox::Yes:      return QStringLiteral("是");
    case QMessageBox::No:       return QStringLiteral("否");
    case QMessageBox::Cancel:   return QStringLiteral("取消");
    case QMessageBox::Save:     return QStringLiteral("保存");
    case QMessageBox::Discard:  return QStringLiteral("不保存");
    case QMessageBox::Apply:    return QStringLiteral("应用");
    case QMessageBox::Close:    return QStringLiteral("关闭");
    case QMessageBox::Reset:    return QStringLiteral("重置");
    case QMessageBox::Help:     return QStringLiteral("帮助");
    case QMessageBox::Abort:    return QStringLiteral("中止");
    case QMessageBox::Retry:    return QStringLiteral("重试");
    case QMessageBox::Ignore:   return QStringLiteral("忽略");
    case QMessageBox::YesToAll: return QStringLiteral("全部是");
    case QMessageBox::NoToAll:  return QStringLiteral("全部否");
    default:                    return QStringLiteral("确定");
    }
}

/// 按「确定/是/否/取消」的固定顺序展开按钮组，保证多个弹窗的按钮顺序一致。
QVector<QPair<QMessageBox::StandardButton, QString>> expandButtons(
    QMessageBox::StandardButtons flags, QMessageBox::StandardButton* defaultOut)
{
    static const QMessageBox::StandardButton kOrder[] = {
        QMessageBox::Ok, QMessageBox::Yes, QMessageBox::No,
        QMessageBox::Cancel, QMessageBox::Save, QMessageBox::Discard,
    };
    QVector<QPair<QMessageBox::StandardButton, QString>> out;
    for (auto b : kOrder) {
        if (flags & b) out.append({b, buttonLabel(b)});
    }
    if (out.isEmpty()) out.append({QMessageBox::Ok, buttonLabel(QMessageBox::Ok)});

    if (defaultOut && *defaultOut == QMessageBox::NoButton) {
        // 未指定默认按钮：取第一个（与 QMessageBox 的行为一致）。
        *defaultOut = out.first().first;
    } else if (defaultOut) {
        bool found = false;
        for (const auto& p : out) {
            if (p.first == *defaultOut) { found = true; break; }
        }
        if (!found) *defaultOut = out.first().first;
    }
    return out;
}

/// 严重程度 → 徽标字形 + variant 属性（颜色全在 QSS 里，这里不写死）。
QString severityGlyph(ThemedMessageBox::Severity s)
{
    switch (s) {
    case ThemedMessageBox::Severity::Warning:  return QStringLiteral("!");
    case ThemedMessageBox::Severity::Critical: return QStringLiteral("!");
    case ThemedMessageBox::Severity::Question: return QStringLiteral("?");
    case ThemedMessageBox::Severity::Info:
    default:                                   return QStringLiteral("i");
    }
}

QString severityVariant(ThemedMessageBox::Severity s)
{
    switch (s) {
    case ThemedMessageBox::Severity::Warning:  return QStringLiteral("warning");
    case ThemedMessageBox::Severity::Critical: return QStringLiteral("critical");
    case ThemedMessageBox::Severity::Question: return QStringLiteral("question");
    case ThemedMessageBox::Severity::Info:
    default:                                   return QStringLiteral("info");
    }
}

/// 把弹窗移到 parent（或屏幕）正中。
void centerOn(QWidget* w, QWidget* parent)
{
    if (!w) return;
    QScreen* screen = nullptr;
    if (parent) {
        screen = QGuiApplication::screenAt(parent->mapToGlobal(parent->rect().center()));
    }
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const QSize sz = w->size();
    w->move(avail.center().x() - sz.width() / 2, avail.center().y() - sz.height() / 2);
}

} // namespace

// ══════════════════════════════════════════════════════════════════════════════

ThemedMessageBox::ThemedMessageBox(Severity severity, const QString& title, const QString& text,
                                   QMessageBox::StandardButtons buttons,
                                   QMessageBox::StandardButton defaultButton, QWidget* parent)
    : AnimatedDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    QMessageBox::StandardButton def = defaultButton;
    buildUi(severity, title, text, expandButtons(buttons, &def), def);
}

ThemedMessageBox::ThemedMessageBox(Severity severity, const QString& title, const QString& text,
                                   const QStringList& buttonTexts, int defaultIndex, QWidget* parent)
    : AnimatedDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);

    buildCustomUi(title, text, buttonTexts, defaultIndex, severity);
}

void ThemedMessageBox::buildUi(
    Severity severity, const QString& title, const QString& text,
    const QVector<QPair<QMessageBox::StandardButton, QString>>& buttons,
    QMessageBox::StandardButton defaultButton)
{
    // ── 卡片（无边框 + 半透明下，背景必须由内层 QFrame 提供，见头文件红线）──
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("msgRoot"));
    card->setFixedWidth(kCardWidth);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // ── 标题栏：标题 + 自绘关闭 ──
    auto* titleBar = new QFrame(card);
    titleBar->setObjectName(QStringLiteral("msgTitleBar"));
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(18, 10, 12, 10);
    titleLayout->setSpacing(8);
    auto* titleLabel = new QLabel(title, titleBar);
    titleLabel->setObjectName(QStringLiteral("msgTitle"));
    titleLayout->addWidget(titleLabel, 1);
    auto* closeButton = new QPushButton(QStringLiteral("✕"), titleBar);
    closeButton->setObjectName(QStringLiteral("msgClose"));
    closeButton->setToolTip(QStringLiteral("关闭"));
    closeButton->setCursor(Qt::ArrowCursor);
    titleLayout->addWidget(closeButton);

    // ── 正文：徽标 + 文案 ──
    auto* body = new QFrame(card);
    body->setObjectName(QStringLiteral("msgBody"));
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(20, 18, 20, 16);
    bodyLayout->setSpacing(12);

    auto* badge = new QLabel(severityGlyph(severity), body);
    badge->setObjectName(QStringLiteral("msgIcon"));
    badge->setProperty("variant", severityVariant(severity));
    badge->setFixedSize(kBadgeSize, kBadgeSize);
    badge->setAlignment(Qt::AlignCenter);
    bodyLayout->addWidget(badge, 0, Qt::AlignTop | Qt::AlignLeft);

    auto* textLabel = new QLabel(text, body);
    textLabel->setObjectName(QStringLiteral("msgText"));
    textLabel->setWordWrap(true);
    // 长文案要能复制（报错信息用户会贴给我们），默认只允许鼠标选中。
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    bodyLayout->addWidget(textLabel, 1);

    // ── 按钮行（右对齐，主按钮在最右）──
    auto* buttonRow = new QFrame(card);
    buttonRow->setObjectName(QStringLiteral("msgButtonRow"));
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(16, 0, 16, 14);
    buttonLayout->setSpacing(10);
    buttonLayout->addStretch(1);
    for (const auto& b : buttons) {
        auto* btn = new QPushButton(b.second, buttonRow);
        btn->setObjectName(QStringLiteral("msgButton"));
        btn->setCursor(Qt::PointingHandCursor);
        if (b.first == defaultButton) btn->setProperty("variant", QStringLiteral("primary"));
        buttonLayout->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, b] { done(b.first); });
    }

    cardLayout->addWidget(titleBar);
    cardLayout->addWidget(body);
    cardLayout->addWidget(buttonRow);

    connect(closeButton, &QPushButton::clicked, this, [this] { done(QMessageBox::Cancel); });

    // ── 高度：文案过高才退化出滚动条（默认无滚动）──
    const int textBudget = static_cast<int>(
        (QGuiApplication::primaryScreen()
             ? QGuiApplication::primaryScreen()->availableSize().height()
             : QSize(1280, 800).height()) * kMaxTextRatio);
    card->adjustSize();
    if (card->height() > textBudget && textLabel->sizeHint().height() > 200) {
        auto* scroll = new QScrollArea(card);
        scroll->setObjectName(QStringLiteral("msgScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        bodyLayout->removeWidget(textLabel);
        textLabel->setParent(scroll);
        textLabel->setMaximumWidth(kCardWidth - 40 - kBadgeSize - 12);
        scroll->setWidget(textLabel);
        scroll->setFixedHeight(textBudget / 2);
        bodyLayout->addWidget(scroll, 1);
    }

    auto* lay = layout();
    if (!lay) {
        setLayout(new QVBoxLayout(this));
        lay = layout();
    }
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(card);
    adjustSize();
    centerOn(this, parentWidget());
}

void ThemedMessageBox::buildCustomUi(const QString& title, const QString& text,
                                     const QStringList& buttonTexts, int defaultIndex,
                                     Severity severity)
{
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("msgRoot"));
    card->setFixedWidth(kCardWidth);
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto* titleBar = new QFrame(card);
    titleBar->setObjectName(QStringLiteral("msgTitleBar"));
    auto* titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(18, 10, 12, 10);
    titleLayout->setSpacing(8);
    auto* titleLabel = new QLabel(title, titleBar);
    titleLabel->setObjectName(QStringLiteral("msgTitle"));
    titleLayout->addWidget(titleLabel, 1);
    auto* closeButton = new QPushButton(QStringLiteral("✕"), titleBar);
    closeButton->setObjectName(QStringLiteral("msgClose"));
    closeButton->setToolTip(QStringLiteral("关闭"));
    titleLayout->addWidget(closeButton);

    auto* body = new QFrame(card);
    body->setObjectName(QStringLiteral("msgBody"));
    auto* bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(20, 18, 20, 16);
    bodyLayout->setSpacing(12);

    auto* badge = new QLabel(severityGlyph(severity), body);
    badge->setObjectName(QStringLiteral("msgIcon"));
    badge->setProperty("variant", severityVariant(severity));
    badge->setFixedSize(kBadgeSize, kBadgeSize);
    badge->setAlignment(Qt::AlignCenter);
    bodyLayout->addWidget(badge, 0, Qt::AlignTop | Qt::AlignLeft);

    auto* textLabel = new QLabel(text, body);
    textLabel->setObjectName(QStringLiteral("msgText"));
    textLabel->setWordWrap(true);
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    bodyLayout->addWidget(textLabel, 1);

    auto* buttonRow = new QFrame(card);
    buttonRow->setObjectName(QStringLiteral("msgButtonRow"));
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(16, 0, 16, 14);
    buttonLayout->setSpacing(10);
    buttonLayout->addStretch(1);
    for (int i = 0; i < buttonTexts.size(); ++i) {
        auto* btn = new QPushButton(buttonTexts.at(i), buttonRow);
        btn->setObjectName(QStringLiteral("msgButton"));
        btn->setCursor(Qt::PointingHandCursor);
        if (i == defaultIndex) btn->setProperty("variant", QStringLiteral("primary"));
        buttonLayout->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, i] { done(i + 1); });
    }

    cardLayout->addWidget(titleBar);
    cardLayout->addWidget(body);
    cardLayout->addWidget(buttonRow);

    connect(closeButton, &QPushButton::clicked, this, [this] { done(0); });

    auto* lay = layout();
    if (!lay) {
        setLayout(new QVBoxLayout(this));
        lay = layout();
    }
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(card);
    adjustSize();
    centerOn(this, parentWidget());
}

// ══════════════════════════════════════════════════════════════════════════════
//  静态入口
// ══════════════════════════════════════════════════════════════════════════════

QMessageBox::StandardButton ThemedMessageBox::information(QWidget* parent, const QString& title,
                                                         const QString& text,
                                                         QMessageBox::StandardButtons buttons,
                                                         QMessageBox::StandardButton defaultButton)
{
    ThemedMessageBox box(Severity::Info, title, text, buttons, defaultButton, parent);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

QMessageBox::StandardButton ThemedMessageBox::warning(QWidget* parent, const QString& title,
                                                     const QString& text,
                                                     QMessageBox::StandardButtons buttons,
                                                     QMessageBox::StandardButton defaultButton)
{
    ThemedMessageBox box(Severity::Warning, title, text, buttons, defaultButton, parent);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

QMessageBox::StandardButton ThemedMessageBox::critical(QWidget* parent, const QString& title,
                                                      const QString& text,
                                                      QMessageBox::StandardButtons buttons,
                                                      QMessageBox::StandardButton defaultButton)
{
    ThemedMessageBox box(Severity::Critical, title, text, buttons, defaultButton, parent);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

QMessageBox::StandardButton ThemedMessageBox::question(QWidget* parent, const QString& title,
                                                      const QString& text,
                                                      QMessageBox::StandardButtons buttons,
                                                      QMessageBox::StandardButton defaultButton)
{
    ThemedMessageBox box(Severity::Question, title, text, buttons, defaultButton, parent);
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

int ThemedMessageBox::custom(QWidget* parent, const QString& title, const QString& text,
                             const QStringList& buttonTexts, int defaultIndex, Severity severity)
{
    if (buttonTexts.isEmpty()) return -1;
    ThemedMessageBox box(severity, title, text, buttonTexts, qBound(0, defaultIndex,
                                                                   buttonTexts.size() - 1), parent);
    return box.exec();
}

bool ThemedMessageBox::noticeWithAction(QWidget* parent, const QString& title, const QString& text,
                                        const QString& actionText, const QString& dismissText,
                                        Severity severity)
{
    // 按钮顺序：[申诉/动作] [关闭]，动作按钮是主按钮（右侧 primary）。
    const int picked = custom(parent, title, text, {actionText, dismissText}, 0, severity);
    return picked == 1;
}

} // namespace kc::ui
