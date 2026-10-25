#include "ui/widgets/NotificationBar.h"

#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>

namespace kc::ui {
namespace {

struct LevelStyle {
    ColorToken bg;
    ColorToken fg;
    QString glyph;
};

LevelStyle styleFor(NotifyLevel level) {
    switch (level) {
        case NotifyLevel::Success: return {ColorToken::SuccessBg, ColorToken::Success, QStringLiteral("\u2713")};
        case NotifyLevel::Warning: return {ColorToken::WarningBg, ColorToken::WarningText, QStringLiteral("!")};
        case NotifyLevel::Error:   return {ColorToken::DangerBg,  ColorToken::Danger,  QStringLiteral("\u2717")};
        case NotifyLevel::Info:
        default:                   return {ColorToken::InfoBg,    ColorToken::Info,    QStringLiteral("i")};
    }
}

} // namespace

NotificationBar::NotificationBar(QWidget* parent, NotifyLevel level)
    : QWidget(parent), m_level(level) {
    buildUi();
    applyTheme();
    // 主题切换后重取令牌色（否则深色主题下留着浅色主题算出来的那一支）。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](bool) { applyTheme(); });
}

NotificationBar::~NotificationBar() = default;

void NotificationBar::buildUi() {
    setObjectName(QStringLiteral("notificationBar"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(12, 8, 12, 8);
    row->setSpacing(8);

    m_icon = new QLabel(this);
    m_icon->setObjectName(QStringLiteral("notifyIcon"));
    m_icon->setAlignment(Qt::AlignCenter);
    m_icon->setFixedWidth(16);

    m_text = new QLabel(this);
    m_text->setObjectName(QStringLiteral("notifyText"));
    m_text->setWordWrap(true);
    m_text->setTextInteractionFlags(Qt::TextSelectableByMouse);

    row->addWidget(m_icon, 0, Qt::AlignTop);
    row->addWidget(m_text, 1);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() { concealAndHide(); });
}

void NotificationBar::applyTheme() {
    // 🔴 重入守卫（2026-10-11，栈溢出修复）：
    //    本函数用 setStyleSheet() 刷样式，而 setStyleSheet 会**同步**给本控件投递
    //    QEvent::StyleChange → changeEvent() → 又调 applyTheme() → 再 setStyleSheet()…
    //    构成无限递归，首次布局/显示时必崩（STATUS_STACK_OVERFLOW 0xC00000FD）。
    //    changeEvent 那一侧保留（主题切换仍要重取色），用本标志把回灌挡掉。
    if (m_applyingTheme) return;
    m_applyingTheme = true;
    const LevelStyle s = styleFor(m_level);
    const ThemeManager& tm = ThemeManager::instance();

    const QColor bg = tm.color(s.bg);
    const QColor fg = tm.color(s.fg);
    const int r = tm.radius(RadiusToken::Small);

    setStyleSheet(QStringLiteral(
                      "QWidget#notificationBar { background: rgba(%1,%2,%3,%4);"
                      " border: 1px solid rgba(%5,%6,%7,%8); border-radius: %9px; }")
                      .arg(bg.red()).arg(bg.green()).arg(bg.blue()).arg(bg.alpha())
                      .arg(fg.red()).arg(fg.green()).arg(fg.blue()).arg(fg.alpha())
                      .arg(r));

    m_icon->setText(s.glyph);
    QFont iconFont = m_icon->font();
    iconFont.setBold(true);
    iconFont.setPointSize(tm.fontSize(FontToken::Subtitle));
    m_icon->setFont(iconFont);

    m_icon->setStyleSheet(QStringLiteral("color: rgb(%1,%2,%3); background: transparent;")
                              .arg(fg.red()).arg(fg.green()).arg(fg.blue()));
    m_text->setStyleSheet(QStringLiteral("color: rgb(%1,%2,%3); background: transparent;")
                              .arg(fg.red()).arg(fg.green()).arg(fg.blue()));
    m_applyingTheme = false;
}

void NotificationBar::setLevel(NotifyLevel level) {
    m_level = level;
    applyTheme();
}

NotificationBar* NotificationBar::show(QWidget* parent, const QString& text, NotifyLevel level,
                                       int ms) {
    if (!parent || text.isEmpty()) return nullptr;

    // 复用 parent 上已有的通知条实例（不重播动画，只换内容）。
    auto* bar = parent->findChild<NotificationBar*>(QString(), Qt::FindDirectChildrenOnly);
    if (!bar) {
        bar = new NotificationBar(parent, level);
        // 贴顶通栏：交给父布局/手动布局；这里只保证位置锚在顶部。
    }
    bar->notify(text, level, ms);
    return bar;
}

void NotificationBar::notify(const QString& text, NotifyLevel level, int ms) {
    // 取消这个容器**自己**的隐藏计时器（不影响别的页面的提示条）。
    m_timer->stop();

    // 判断是不是「已经在展示中」——必须在动透明度之前读，
    // 否则下面的分支永远走不到（这正是第一次实现会漏掉动画的坑）。
    const bool alreadyShown = m_shown && isVisible();

    m_level = level;
    m_text->setText(text);
    applyTheme();

    m_autoHideMs = ms;
    if (!alreadyShown) {
        m_shown = true;
        // 🔴 通知条是**布局管辖**的（页面构造里 addWidget 进来）——
        //    只能用纯透明度原语。原来走 Motion::reveal 会 `w->move()`，
        //    布局下一次重排就把它推回去，结果是「淡生效、位移被吃掉」，
        //    中途还闪一下错位（记忆里的红线：布局管辖控件禁用 reveal/conceal）。
        Motion::fadeIn(this, RevealMs);
    }

    if (ms > 0) {
        m_timer->start(ms);
    }
}

void NotificationBar::concealAndHide() {
    m_timer->stop();
    m_shown = false;
    // 淡出后折叠（同样不能带位移，理由见 notify）。
    Motion::fadeOut(this, ConcealMs, /*hideAfter=*/true);
}

void NotificationBar::changeEvent(QEvent* event) {
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

} // namespace kc::ui
