#include "ui/widgets/GlassMenu.h"

#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QEnterEvent>
#include <QEventLoop>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScreen>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace kc::ui {

namespace {

constexpr int kRowH = 32;
constexpr int kSepH = 8;
constexpr int kPadX = 6;      // 内容层左右内边距
constexpr int kPadY = 6;
constexpr int kIconSize = 16;
constexpr int kRowPadL = 10;
constexpr int kRowPadR = 12;
constexpr int kIconGap = 10;

QColor menuTextColor(bool dark, bool dim)
{
    if (dark) return dim ? QColor(255, 255, 255, 130) : QColor(255, 255, 255, 235);
    return dim ? QColor(20, 24, 30, 130) : QColor(20, 24, 30, 235);
}

} // namespace

// ── 菜单行（自绘，无 Q_OBJECT：只要 hover / 点击，不需要信号）─────────────────
class GlassMenuRow : public QWidget {
public:
    GlassMenuRow(const QIcon& icon, const QString& text, bool checked,
                 std::function<void()> onTrigger, GlassMenu* owner)
        : QWidget(owner->content())
        , m_icon(icon)
        , m_text(text)
        , m_checked(checked)
        , m_on(std::move(onTrigger))
        , m_owner(owner)
    {
        setFixedHeight(kRowH);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);

        QFont f = font();
        f.setPixelSize(13);   // 对齐 QSS 全局 `* { font-size: 13px; }`
        setFont(f);
    }

    /// 这一行需要多宽（不含内容层内边距）。
    int desiredWidth() const
    {
        const QFontMetrics fm(font());
        int w = kRowPadL + (m_icon.isNull() ? 0 : kIconSize + kIconGap)
                + fm.horizontalAdvance(m_text) + kRowPadR;
        if (m_checked) w += 18;
        return w;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const bool dark = ThemeManager::instance().isDark();
        const QColor accent = ThemeManager::instance().accent();

        // hover / 按下：强调色淡染，圆角 7。不位移不放大（客户端视觉令牌）。
        if (m_pressed || m_hover) {
            QColor bg = accent;
            bg.setAlpha(m_pressed ? 62 : 40);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(QRectF(rect()).adjusted(0, 1, 0, -1), 7, 7);
        }

        int x = kRowPadL;
        if (!m_icon.isNull()) {
            const QPixmap pm = m_icon.pixmap(QSize(kIconSize, kIconSize));
            if (!pm.isNull()) {
                p.drawPixmap(QRect(x, (height() - kIconSize) / 2, kIconSize, kIconSize), pm);
            }
            x += kIconSize + kIconGap;
        }

        p.setPen(menuTextColor(dark, false));
        const QRect textRect(x, 0, width() - x - (m_checked ? 30 : kRowPadR), height());
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(font()).elidedText(m_text, Qt::ElideRight, textRect.width()));

        if (m_checked) {
            // 矢量对勾（不用字体字形：◐ ✓ 之类在小字号会渲染成方块/小点）
            QPainterPath path;
            const qreal cx = width() - 18, cy = height() / 2.0;
            path.moveTo(cx - 5.0, cy + 0.5);
            path.lineTo(cx - 1.5, cy + 4.0);
            path.lineTo(cx + 5.0, cy - 4.0);
            QPen pen(accent, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawPath(path);
        }
    }

    void enterEvent(QEnterEvent*) override { m_hover = true; update(); }
    void leaveEvent(QEvent*) override { m_hover = false; m_pressed = false; update(); }

    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) { m_pressed = true; update(); }
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) return;
        m_pressed = false;
        update();
        if (!rect().contains(e->pos())) return;

        // 🔴 先收菜单再执行回调：回调常常要开模态弹窗，菜单得先消失，
        //    否则弹窗底下还压着一块玻璃。std::function 先拷出来再调，
        //    避免回调里把菜单删了之后再去访问成员。
        auto on = m_on;
        if (m_owner) m_owner->dismiss(false);
        if (on) on();
    }

private:
    QIcon m_icon;
    QString m_text;
    bool m_checked = false;
    std::function<void()> m_on;
    GlassMenu* m_owner = nullptr;
    bool m_hover = false;
    bool m_pressed = false;
};

// ── 分隔线 ────────────────────────────────────────────────────────────────────
namespace {

class GlassMenuSeparator : public QWidget {
public:
    explicit GlassMenuSeparator(QWidget* parent) : QWidget(parent)
    {
        setFixedHeight(kSepH);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const bool dark = ThemeManager::instance().isDark();
        p.setPen(QPen(dark ? QColor(255, 255, 255, 30) : QColor(0, 0, 0, 26), 1));
        const int y = height() / 2;
        p.drawLine(kRowPadL, y, width() - kRowPadR, y);
    }
};

} // namespace

// ── GlassMenu ─────────────────────────────────────────────────────────────────

GlassMenu::GlassMenu(QWidget* parent)
    : GlassPanel(parent)
{
    // Tool：不抢焦点、不进任务栏，但**不**用 Qt::Popup —— Popup 会在外部点击时
    // 立刻 hide()，把淡出动画掐掉。关闭逻辑自己用 qApp 事件过滤器做。
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_DeleteOnClose, false);

    setRadius(14);
    glass::Style st = style();
    st.tint = 0.74;
    st.frost = 26.0;
    st.edgeBand = 0.20;
    st.lightPos = QPointF(0.5, 0.0);
    setStyle(st);

    if (auto* lay = contentLayout()) {
        lay->setContentsMargins(kPadX, kPadY, kPadX, kPadY);
        lay->setSpacing(1);
    }
}

GlassMenu::~GlassMenu() = default;

void GlassMenu::addItem(const QIcon& icon, const QString& text,
                        std::function<void()> onTrigger, bool checked)
{
    auto* row = new GlassMenuRow(icon, text, checked, std::move(onTrigger), this);
    m_rows.append(row);
    m_all.append(row);
    contentLayout()->addWidget(row);
}

void GlassMenu::addSeparator()
{
    auto* sep = new GlassMenuSeparator(content());
    m_all.append(sep);
    contentLayout()->addWidget(sep);
}

int GlassMenu::computeWidth() const
{
    int w = 0;
    for (const GlassMenuRow* row : m_rows) w = std::max(w, row->desiredWidth());
    return std::clamp(w + kPadX * 2, 170, 380);
}

int GlassMenu::computeHeight() const
{
    int h = kPadY * 2;
    const int spacing = contentLayout() ? contentLayout()->spacing() : 0;
    for (int i = 0; i < m_all.size(); ++i) {
        h += m_all.at(i)->height();
        if (i + 1 < m_all.size()) h += spacing;
    }
    return h;
}

void GlassMenu::popupAt(const QRect& anchorGlobal)
{
    if (m_rows.isEmpty()) return;

    m_dismissing = false;
    m_openUp = false;

    const int w = computeWidth();
    const int h = computeHeight();
    setFixedSize(w, h);

    QScreen* screen = QGuiApplication::screenAt(anchorGlobal.center());
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);

    QPoint pos(anchorGlobal.left(), anchorGlobal.bottom() + 6);
    if (pos.y() + h > avail.bottom()) {
        pos.setY(anchorGlobal.top() - h - 6);
        m_openUp = true;
    }
    if (pos.y() < avail.top()) pos.setY(avail.top() + 4);
    pos.setX(std::clamp(pos.x(), avail.left() + 4, std::max(avail.left() + 4, avail.right() - w - 4)));

    setGeometry(QRect(pos, QSize(w, h)));
    refreshBackdrop();

    const QPoint endPos = pos;
    const QPoint startPos = endPos + QPoint(0, m_openUp ? -8 : 8);

    if (Motion::animationsEnabled()) {
        setWindowOpacity(0.0);
        move(startPos);
    } else {
        setWindowOpacity(1.0);
    }

    show();
    raise();

    if (Motion::animationsEnabled()) {
        QPointer<GlassMenu> guard(this);
        auto* fade = Motion::tween(this, "windowOpacity", 0.0, 1.0, 170, Motion::silkEase());

        auto* slide = new QPropertyAnimation(this, "pos", this);
        slide->setDuration(170);
        slide->setStartValue(startPos);
        slide->setEndValue(endPos);
        slide->setEasingCurve(Motion::silkEase());
        slide->start(QAbstractAnimation::DeleteWhenStopped);

        auto settle = [guard, endPos]() {
            if (!guard) return;
            guard->setWindowOpacity(1.0);
            guard->move(endPos);
        };
        QObject::connect(fade, &QAbstractAnimation::finished, this, settle);
        QTimer::singleShot(170 + Motion::TimeoutFudgeMs, this, [guard, settle]() {
            if (!guard) return;
            settle();
        });
    }
}

void GlassMenu::execAt(const QRect& anchorGlobal)
{
    popupAt(anchorGlobal);
    if (!isVisible()) return;

    QEventLoop loop;
    QObject::connect(this, &GlassMenu::hidden, &loop, &QEventLoop::quit);
    // 兜底：万一 hidden 因为某条路径没发出来（被外部 hide），也别把主线程挂死。
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(30000);
    loop.exec();
}

void GlassMenu::dismiss(bool animate)
{
    if (m_dismissing) return;
    if (!isVisible()) {
        emit hidden();
        return;
    }

    m_dismissing = true;
    qApp->removeEventFilter(this);

    if (!animate || !Motion::animationsEnabled()) {
        hide();
        return;
    }

    const QPoint startPos = pos();
    const QPoint endPos = startPos + QPoint(0, m_openUp ? -6 : 6);

    QPointer<GlassMenu> guard(this);
    auto finish = [guard]() {
        if (!guard) return;
        guard->hide();
    };

    auto* fade = Motion::tween(this, "windowOpacity", windowOpacity(), 0.0, 120, Motion::exitEase());
    auto* slide = new QPropertyAnimation(this, "pos", this);
    slide->setDuration(120);
    slide->setStartValue(startPos);
    slide->setEndValue(endPos);
    slide->setEasingCurve(Motion::exitEase());
    slide->start(QAbstractAnimation::DeleteWhenStopped);

    QObject::connect(fade, &QAbstractAnimation::finished, this, finish);
    QTimer::singleShot(120 + Motion::TimeoutFudgeMs, this, finish);
}

void GlassMenu::showEvent(QShowEvent* event)
{
    GlassPanel::showEvent(event);
    qApp->installEventFilter(this);
}

void GlassMenu::hideEvent(QHideEvent* event)
{
    GlassPanel::hideEvent(event);
    qApp->removeEventFilter(this);
    m_dismissing = false;
    emit hidden();
}

bool GlassMenu::eventFilter(QObject* watched, QEvent* event)
{
    Q_UNUSED(watched);
    if (!isVisible()) return false;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(event);
        if (!geometry().contains(me->globalPosition().toPoint())) dismiss(true);
        break;
    }
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
            dismiss(true);
            return true;
        }
        break;
    }
    case QEvent::ApplicationDeactivate:
        dismiss(true);
        break;
    default:
        break;
    }
    return false;
}

void GlassMenu::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        dismiss(true);
        return;
    }
    QWidget::keyPressEvent(event);
}

} // namespace kc::ui
