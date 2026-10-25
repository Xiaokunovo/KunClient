#include "ui/dialogs/EraseConfirmDialog.h"

#include "ui/theme/ThemeManager.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

constexpr int kCardWidth = 480;
constexpr int kBadgeSize = 30;
constexpr int kListMaxHeight = 240;
constexpr int kListMinHeight = 150;

void centerOn(QWidget* w, QWidget* parent)
{
    if (!w) return;
    QScreen* screen = nullptr;
    if (parent) {
        screen = QApplication::screenAt(parent->mapToGlobal(parent->rect().center()));
    }
    if (!screen) screen = QApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
    const QSize sz = w->size();
    w->move(avail.center().x() - sz.width() / 2, avail.center().y() - sz.height() / 2);
}

} // namespace

EraseConfirmDialog::EraseConfirmDialog(QWidget* parent, const QString& summary,
                                       const QStringList& items)
    : GlassDialog(parent)
{
    const bool dark = ThemeManager::instance().isDark();

    setMinimumWidth(kCardWidth);
    setTitle(QStringLiteral("确认擦除"));

    // ── 顶部：危险徽标 + 摘要 ─────────────────────────────────────────────
    auto* topRow = new QWidget;
    topRow->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* topLayout = new QHBoxLayout(topRow);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(12);

    auto* badge = new QLabel(QStringLiteral("!"), topRow);
    badge->setFixedSize(kBadgeSize, kBadgeSize);
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral(
        "background: rgba(255,69,58,0.18); color: #FF7B72;"
        " border: 1px solid rgba(255,69,58,0.40);"
        " border-radius: %1px; font-size: 15px; font-weight: 700;")
        .arg(kBadgeSize / 2));
    topLayout->addWidget(badge, 0, Qt::AlignTop | Qt::AlignLeft);

    auto* summaryLabel = new QLabel(
        summary + QStringLiteral("\n此操作不可恢复，请确认已备份重要数据。"), topRow);
    summaryLabel->setWordWrap(true);
    summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summaryLabel->setStyleSheet(QStringLiteral(
        "background: transparent; color: %1; font-size: 12.5px;")
        .arg(dark ? QStringLiteral("rgba(255,255,255,0.84)")
                  : QStringLiteral("rgba(18,22,28,0.84)")));
    topLayout->addWidget(summaryLabel, 1);
    bodyLayout()->addWidget(topRow);

    // ── 分区列表：固定最大高度，内容多时内部滚动 ─────────────────────────
    auto* list = new QListWidget;
    list->setObjectName(QStringLiteral("partitionList"));   // 复用 QSS 列表皮肤
    list->setSelectionMode(QAbstractItemView::NoSelection);
    list->setFocusPolicy(Qt::NoFocus);
    list->setAlternatingRowColors(true);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->addItems(items);
    list->setMinimumHeight(kListMinHeight);
    list->setMaximumHeight(kListMaxHeight);
    bodyLayout()->addWidget(list);

    // ── 按钮 ──────────────────────────────────────────────────────────────
    addButton(QStringLiteral("取消"), ButtonKind::Ghost, [this] { reject(); });
    addButton(QStringLiteral("擦除"), ButtonKind::Danger, [this] {
        m_confirmed = true;
        accept();
    });

    adjustSize();
    centerOn(this, parentWidget());
}

} // namespace kc::ui
