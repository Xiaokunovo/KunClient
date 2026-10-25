#include "ui/widgets/BottomSheet.h"

#include "ui/MainShell.h"
#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/GlassSurface.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace kc::ui {

namespace {

constexpr int kSideMargin = 20;
constexpr int kHandleH = 5;
constexpr int kHandleW = 40;

QColor primaryText(bool dark)
{
    return dark ? QColor(255, 255, 255, 238) : QColor(18, 22, 28, 238);
}

QColor secondaryText(bool dark)
{
    return dark ? QColor(255, 255, 255, 150) : QColor(18, 22, 28, 150);
}

/// 🔴 Qt QSS 的颜色坑：`#aarrggbb`（QColor::name(HexArgb)）不认，会静默回退黑；
///    `rgba(r,g,b,255)` 也不可靠。不透明色一律输出 `#rrggbb`，半透明才用 rgba()。
QString cssColor(const QColor& c)
{
    if (c.alpha() >= 255) return c.name(QColor::HexRgb);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

} // namespace

BottomSheet::BottomSheet(QWidget* parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setModal(true);

    // 玻璃面板：贴底、只圆上两角在视觉上更像 iOS sheet。GlassSurface 四角
    // 都圆，面板贴到窗口底沿时下两角被窗口裁掉一半，观感可接受。
    m_panel = new GlassSurface(this);
    m_panel->setInteractive(false);
    m_panel->setFallbackBackdrop(true);
    m_panel->setRadius(24);

    glass::Style st;
    st.tint = 0.86;          // 抽屉上要放正文，必须够实
    st.frost = 26.0;
    st.edgeBand = 0.16;
    st.lightPos = QPointF(0.5, 0.0);
    m_panel->setStyle(st);

    m_contentHost = new QWidget(m_panel);
    m_contentHost->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* hostLayout = new QVBoxLayout(m_contentHost);
    hostLayout->setContentsMargins(18, 30, 18, 18);   // 顶部 30 给抓取把手留位
    hostLayout->setSpacing(10);

    m_handle = new QWidget(this);
    m_handle->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_handle->setFixedSize(kHandleW, kHandleH);
    m_handle->setStyleSheet(QStringLiteral(
        "background: rgba(%1,%2,%3,90); border-radius: 2px;")
        .arg(ThemeManager::instance().isDark() ? 255 : 20)
        .arg(ThemeManager::instance().isDark() ? 255 : 24)
        .arg(ThemeManager::instance().isDark() ? 255 : 30));
}

BottomSheet::~BottomSheet() = default;

void BottomSheet::setContent(QWidget* content, int height)
{
    if (m_content == content) return;
    if (m_content) {
        m_content->setParent(nullptr);
        m_content->deleteLater();
    }
    m_content = content;
    m_wantedHeight = height;

    if (m_content) {
        m_content->setParent(m_contentHost);
        if (auto* lay = qobject_cast<QVBoxLayout*>(m_contentHost->layout())) {
            lay->addWidget(m_content);
        }
    }
    relayout();
}

void BottomSheet::setMaxPanelWidth(int px)
{
    m_maxPanelW = std::max(0, px);
    relayout();
}

void BottomSheet::relayout()
{
    if (width() <= 0 || height() <= 0) return;

    int panelW = std::max(220, width() - kSideMargin * 2);
    if (m_maxPanelW > 0) panelW = std::min(panelW, m_maxPanelW);

    int panelH = m_wantedHeight;
    if (panelH <= 0) {
        const int contentH = m_content ? m_content->sizeHint().height() : 120;
        panelH = 30 + 18 + contentH + 18;
    }
    panelH = std::clamp(panelH, 140, std::max(140, height() - 80));

    const int px = (width() - panelW) / 2;
    const int py = height() - panelH;

    m_panel->setGeometry(px, py, panelW, panelH);
    m_contentHost->setGeometry(0, 0, panelW, panelH);
    m_handle->move(px + (panelW - kHandleW) / 2, py + 12);

    refreshPanelBackdrop();
}

void BottomSheet::refreshPanelBackdrop()
{
    MainShell* shell = MainShell::instance();
    if (!shell || !m_panel) return;

    // 遮罩本身就是顶层窗口：先取整块背板，再按面板在遮罩里的矩形裁一刀。
    const QImage full = shell->backdropForWidget(this);
    if (full.isNull()) return;

    const QRect pr = m_panel->geometry();
    const QRect clipped = pr.intersected(full.rect());
    if (clipped.isEmpty()) return;
    m_panel->setRawBackdrop(full.copy(clipped));
}

void BottomSheet::showSheet()
{
    // 覆盖主壳整块区域（主壳无边框，frameGeometry == geometry）。
    QRect host;
    if (MainShell* shell = MainShell::instance()) host = shell->frameGeometry();
    if (host.isEmpty()) {
        if (QScreen* s = QGuiApplication::primaryScreen()) host = s->availableGeometry();
    }
    setGeometry(host);

    relayout();

    const int targetY = m_panel->y();
    setScrimAlpha(0.0);
    m_panel->move(m_panel->x(), height());

    show();
    raise();
    qApp->installEventFilter(this);

    if (!Motion::animationsEnabled()) {
        setScrimAlpha(0.52);
        m_panel->move(m_panel->x(), targetY);
        return;
    }

    Motion::tween(this, "scrimAlpha", 0.0, 0.52, 240, Motion::silkEase());

    auto* slide = new QPropertyAnimation(m_panel, "pos", this);
    slide->setDuration(300);
    slide->setStartValue(QPoint(m_panel->x(), height()));
    slide->setEndValue(QPoint(m_panel->x(), targetY));
    slide->setEasingCurve(Motion::silkEase());
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void BottomSheet::closeSheet()
{
    if (m_closing) return;
    m_closing = true;
    qApp->removeEventFilter(this);

    if (!Motion::animationsEnabled() || !isVisible()) {
        finishClose();
        return;
    }

    Motion::tween(this, "scrimAlpha", m_scrimAlpha, 0.0, 180, Motion::exitEase());

    auto* slide = new QPropertyAnimation(m_panel, "pos", this);
    slide->setDuration(220);
    slide->setStartValue(m_panel->pos());
    slide->setEndValue(QPoint(m_panel->x(), height()));
    slide->setEasingCurve(Motion::exitEase());
    slide->start(QAbstractAnimation::DeleteWhenStopped);

    QTimer::singleShot(240 + Motion::TimeoutFudgeMs, this, [this] { finishClose(); });
}

void BottomSheet::finishClose()
{
    hide();
    emit closed();
    deleteLater();
}

void BottomSheet::setScrimAlpha(qreal alpha)
{
    m_scrimAlpha = std::clamp(alpha, 0.0, 1.0);
    update();
}

void BottomSheet::paintEvent(QPaintEvent*)
{
    // 🔴 不调 QDialog::paintEvent：QSS 里有 `QDialog { background: #14181F; }`，
    //    走基类会先糊一层不透明底，遮罩就不是半透明的了。
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, int(m_scrimAlpha * 255.0)));
}

void BottomSheet::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    relayout();
}

void BottomSheet::mousePressEvent(QMouseEvent* event)
{
    // 点面板外 = 关（面板是子控件，点到面板不会进这里）。
    if (!m_panel->geometry().contains(event->pos())) {
        closeSheet();
        return;
    }
    QDialog::mousePressEvent(event);
}

bool BottomSheet::eventFilter(QObject* watched, QEvent* event)
{
    if (isVisible() && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            closeSheet();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

// ── 便捷确认框 ────────────────────────────────────────────────────────────────
namespace {

/// 建一个「标题 + 正文 + 取消/确认」的抽屉。返回的 sheet 已经填好内容，
/// 但**还没** showSheet() —— 调用方自己决定是异步弹还是阻塞弹。
BottomSheet* buildConfirmSheet(const QString& title, const QString& message,
                               const QString& confirmText, const QString& cancelText,
                               std::function<void()> onConfirm)
{
    const bool dark = ThemeManager::instance().isDark();

    auto* sheet = new BottomSheet(MainShell::instance());

    auto* box = new QWidget;
    box->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    auto* titleLabel = new QLabel(title, box);
    titleLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 15px; font-weight: 700;")
                                  .arg(cssColor(primaryText(dark))));
    titleLabel->setWordWrap(true);

    auto* msgLabel = new QLabel(message, box);
    msgLabel->setStyleSheet(QStringLiteral("background: transparent; color: %1; font-size: 12.5px; line-height: 1.5;")
                                .arg(cssColor(secondaryText(dark))));
    msgLabel->setWordWrap(true);

    lay->addWidget(titleLabel);
    lay->addWidget(msgLabel);
    lay->addSpacing(6);

    auto* row = new QHBoxLayout;
    row->setSpacing(10);

    // 🔴 按钮外观走全局 QSS（objectName），不用控件级 setStyleSheet：
    //    在这种 frameless 透明窗口里，控件级 `background` 会被别处规则盖掉，
    //    实测主按钮永远是黑块（color 却正常）。
    auto* cancel = new QPushButton(cancelText, box);
    cancel->setObjectName(QStringLiteral("glassBtnGhost"));
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setFixedHeight(38);

    auto* ok = new QPushButton(confirmText, box);
    ok->setObjectName(QStringLiteral("glassBtnPrimary"));
    ok->setCursor(Qt::PointingHandCursor);
    ok->setFixedHeight(38);

    row->addWidget(cancel, 1);
    row->addWidget(ok, 1);
    lay->addLayout(row);

    QObject::connect(cancel, &QPushButton::clicked, sheet, &BottomSheet::closeSheet);
    QObject::connect(ok, &QPushButton::clicked, sheet, [sheet, onConfirm] {
        sheet->closeSheet();
        if (onConfirm) onConfirm();
    });
    QObject::connect(sheet, &BottomSheet::closed, sheet, &QObject::deleteLater);

    sheet->setContent(box);
    return sheet;
}

} // namespace

void BottomSheet::confirm(const QString& title, const QString& message,
                          const QString& confirmText, std::function<void()> onConfirm,
                          const QString& cancelText)
{
    buildConfirmSheet(title, message, confirmText, cancelText, std::move(onConfirm))
        ->showSheet();
}

bool BottomSheet::confirmSync(const QString& title, const QString& message,
                              const QString& confirmText, const QString& cancelText)
{
    bool confirmed = false;
    auto* sheet = buildConfirmSheet(title, message, confirmText, cancelText,
                                    [&confirmed] { confirmed = true; });

    QEventLoop loop;
    QObject::connect(sheet, &BottomSheet::closed, &loop, &QEventLoop::quit);
    sheet->showSheet();
    loop.exec();
    return confirmed;
}

void BottomSheet::showActions(const QString& title, const QVector<Action>& actions,
                              const QString& cancelText)
{
    if (actions.isEmpty()) return;

    const bool dark = ThemeManager::instance().isDark();
    auto* sheet = new BottomSheet(MainShell::instance());

    auto* box = new QWidget;
    box->setAttribute(Qt::WA_TranslucentBackground, true);
    auto* lay = new QVBoxLayout(box);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    if (!title.trimmed().isEmpty()) {
        auto* titleLabel = new QLabel(title, box);
        titleLabel->setWordWrap(true);
        titleLabel->setAlignment(Qt::AlignCenter);
        titleLabel->setStyleSheet(QStringLiteral(
            "background: transparent; color: %1; font-size: 12px; padding-bottom: 10px;")
            .arg(cssColor(secondaryText(dark))));
        lay->addWidget(titleLabel);
    }

    // 动作行：整宽、居中、分隔线。危险项红字。
    for (const Action& action : actions) {
        auto* row = new QPushButton(action.text, box);
        row->setObjectName(action.destructive ? QStringLiteral("glassActionDanger")
                                              : QStringLiteral("glassActionRow"));
        row->setCursor(Qt::PointingHandCursor);
        row->setFixedHeight(46);
        auto onPick = action.onPick;
        QObject::connect(row, &QPushButton::clicked, sheet, [sheet, onPick] {
            sheet->closeSheet();
            if (onPick) onPick();
        });
        lay->addWidget(row);
    }

    // 取消：单独一块，与动作组隔开（iOS 动作面板的经典布局）。
    auto* cancel = new QPushButton(cancelText, box);
    cancel->setObjectName(QStringLiteral("glassBtnGhost"));
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setFixedHeight(44);
    auto* cancelWrap = new QVBoxLayout;
    cancelWrap->setContentsMargins(0, 10, 0, 0);
    cancelWrap->addWidget(cancel);
    lay->addLayout(cancelWrap);

    QObject::connect(cancel, &QPushButton::clicked, sheet, &BottomSheet::closeSheet);
    QObject::connect(sheet, &BottomSheet::closed, sheet, &QObject::deleteLater);

    // 🔴 显式给面板高度：靠 content->sizeHint() 推算时，动作行的 QSS
    //    min-height 还没生效，算出来偏小 → 面板只有一百来像素、行全挤在一起。
    //    140 = 上下内边距(30+18) + 标题(22) + 组间距(10) + 取消块上间距(10) + 取消(44) + 余量。
    const int wanted = 140 + actions.size() * 46;
    sheet->setMaxPanelWidth(520);   // iOS 动作面板是窄条居中，不铺满整屏
    sheet->setContent(box, wanted);
    sheet->showSheet();
}

} // namespace kc::ui
