#include "ui/widgets/PageHeaderBar.h"

#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace kc::ui {

PageHeaderBar::PageHeaderBar(QWidget* parent) : QWidget(parent) {
    buildUi();
    applyTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](bool) { applyTheme(); });
}

PageHeaderBar::PageHeaderBar(const QString& title, const QString& subtitle, QWidget* parent)
    : PageHeaderBar(parent) {
    setTitle(title);
    setSubtitle(subtitle);
}

void PageHeaderBar::buildUi() {
    setObjectName(QStringLiteral("pageHeaderBar"));
    setAttribute(Qt::WA_StyledBackground, true);

    // 底部发丝线：BorderBrush=Theme.BorderSubtle, BorderThickness=0,0,0,1。
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(2, 0, 2, 12);
    outer->setSpacing(0);

    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(12);

    auto* textCol = new QVBoxLayout();
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(1);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName(QStringLiteral("pageTitle"));
    m_titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_subtitleLabel = new QLabel(this);
    m_subtitleLabel->setObjectName(QStringLiteral("pageSubtitle"));
    m_subtitleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

    textCol->addWidget(m_titleLabel);
    textCol->addWidget(m_subtitleLabel);

    row->addLayout(textCol, 1);

    // 右侧动作区（可为空）
    m_actionsHost = new QWidget(this);
    m_actionsHost->setObjectName(QStringLiteral("pageHeaderActions"));
    m_actionsLayout = new QHBoxLayout(m_actionsHost);
    m_actionsLayout->setContentsMargins(0, 0, 0, 0);
    m_actionsLayout->setSpacing(8);
    row->addWidget(m_actionsHost, 0, Qt::AlignVCenter);

    outer->addLayout(row);
    outer->addSpacing(1); // 发丝线占位高度
}

void PageHeaderBar::applyTheme() {
    // 🔴 重入守卫（2026-10-11，栈溢出修复）：setStyleSheet() 会同步投递
    //    StyleChange → changeEvent() → applyTheme()，无限递归（0xC00000FD）。
    if (m_applyingTheme) return;
    m_applyingTheme = true;
    const ThemeManager& tm = ThemeManager::instance();
    const int r = tm.radius(RadiusToken::Small);

    const QColor title = tm.color(ColorToken::Text);
    const QColor sub = tm.color(ColorToken::TextFaint);
    const QColor line = tm.color(ColorToken::BorderSubtle);

    // 发丝线 + 令牌色（标题/副标题字色）。圆角只用于动作区容器。
    setStyleSheet(
        QStringLiteral(
            "QWidget#pageHeaderBar { background: transparent;"
            " border-bottom: 1px solid rgba(%1,%2,%3,%4); border-radius: %5px; }")
            .arg(line.red()).arg(line.green()).arg(line.blue()).arg(line.alpha())
            .arg(r) +
        QStringLiteral("QLabel#pageTitle { color: rgb(%1,%2,%3); font-size: 16px;"
                       " font-weight: 600; background: transparent; }")
            .arg(title.red()).arg(title.green()).arg(title.blue()) +
        QStringLiteral("QLabel#pageSubtitle { color: rgb(%1,%2,%3); font-size: 11px;"
                       " background: transparent; }")
            .arg(sub.red()).arg(sub.green()).arg(sub.blue()));

    m_subtitleLabel->setVisible(!m_subtitle.isEmpty());
    m_applyingTheme = false;
}

void PageHeaderBar::setTitle(const QString& title) {
    m_title = title;
    m_titleLabel->setText(title);
}

void PageHeaderBar::setSubtitle(const QString& subtitle) {
    m_subtitle = subtitle;
    m_subtitleLabel->setText(subtitle);
    m_subtitleLabel->setVisible(!subtitle.isEmpty());
}

void PageHeaderBar::addAction(QWidget* action) {
    if (!action) return;
    m_actionsLayout->addWidget(action);
}

void PageHeaderBar::changeEvent(QEvent* event) {
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

} // namespace kc::ui
