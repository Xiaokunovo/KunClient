#include "ui/widgets/Toast.h"

#include "ui/MainShell.h"
#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>

#include <algorithm>

namespace kc::ui {

namespace {

constexpr int kToastH = 40;
constexpr int kToastPadX = 16;
constexpr int kIconBox = 18;
constexpr int kIconGap = 10;

Toast* g_toast = nullptr;

QColor kindColor(Toast::Kind kind)
{
    switch (kind) {
    case Toast::Kind::Success: return QColor(0x30, 0xD1, 0x58);   // iOS green
    case Toast::Kind::Warning: return QColor(0xFF, 0x9F, 0x0A);   // iOS orange
    case Toast::Kind::Error:   return QColor(0xFF, 0x45, 0x3A);   // iOS red
    case Toast::Kind::Info:
    default:                   return QColor(0x0A, 0x84, 0xFF);   // 品牌蓝（会被主题强调色覆盖）
    }
}

} // namespace

Toast::Toast(QWidget* parent)
    : GlassPanel(parent)
{
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint
                   | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);   // 通知不挡点击
    setRadius(kToastH / 2.0);
    setHighlight(true);

    glass::Style st = style();
    st.tint = 0.80;
    st.frost = 24.0;
    st.edgeBand = 0.22;
    st.lightPos = QPointF(0.5, 0.0);
    setStyle(st);

    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this] { dismiss(true); });
}

Toast* Toast::ensure()
{
    if (!g_toast) {
        g_toast = new Toast(MainShell::instance());
    }
    return g_toast;
}

void Toast::show(const QString& text, Kind kind, int durationMs)
{
    if (text.trimmed().isEmpty()) return;
    Toast* t = ensure();
    t->m_queue.enqueue(Entry{text, kind, std::max(800, durationMs)});
    t->advance();
}

void Toast::clearAll()
{
    if (!g_toast) return;
    g_toast->m_queue.clear();
    g_toast->dismiss(false);
}

void Toast::advance()
{
    if (m_busy || m_queue.isEmpty()) return;
    const Entry e = m_queue.dequeue();
    present(e.text, e.kind, e.durationMs);
}

void Toast::present(const QString& text, Kind kind, int durationMs)
{
    m_busy = true;
    m_dismissing = false;
    m_text = text;
    m_kind = kind;
    m_durationMs = durationMs;

    // ── 尺寸：图标 + 间距 + 文字 + 两侧内边距 ──────────────────────────────
    // 🔴 字号必须和 QSS 全局 `* { font-size: 13px; }` 一致：QSS 会在 polish 时
    //    覆盖 setFont，按 12.5px 量出来的宽度偏窄，末尾会被省略成「设置已…」。
    QFont f = font();
    f.setPixelSize(13);
    setFont(f);
    const QFontMetrics fm(font());
    const int textW = fm.horizontalAdvance(text) + 8;   // +8 留抗锯齿/字距余量
    const int w = kToastPadX * 2 + kIconBox + kIconGap + textW;
    setFixedSize(std::min(w, 520), kToastH);

    // ── 定位：主窗口顶部居中（没有主壳就退到屏幕顶部居中）─────────────────
    QRect host;
    if (MainShell* shell = MainShell::instance()) {
        host = shell->frameGeometry();
    }
    if (host.isEmpty()) {
        if (QScreen* s = QGuiApplication::primaryScreen()) host = s->availableGeometry();
    }
    const int x = host.center().x() - width() / 2;
    const int y = host.top() + 16;
    setGeometry(QRect(QPoint(x, y), size()));
    refreshBackdrop();

    const QPoint endPos(x, y);
    const QPoint startPos(x, y - 14);

    if (Motion::animationsEnabled()) {
        setWindowOpacity(0.0);
        move(startPos);
    } else {
        setWindowOpacity(1.0);
    }

    // 🔴 必须写 QWidget::show()：本类有个 static show(text, kind, ms) 重载，
    //    直接写 show() 会解析到那个静态函数（参数不够 → 编译失败）。
    QWidget::show();
    raise();

    if (Motion::animationsEnabled()) {
        QPointer<Toast> guard(this);
        auto* fade = Motion::tween(this, "windowOpacity", 0.0, 1.0, 200, Motion::silkEase());
        auto* slide = new QPropertyAnimation(this, "pos", this);
        slide->setDuration(200);
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
        QTimer::singleShot(200 + Motion::TimeoutFudgeMs, this, [guard, settle]() {
            if (!guard) return;
            settle();
        });
    }

    m_timer->start(m_durationMs);
}

void Toast::dismiss(bool animate)
{
    if (m_dismissing) return;
    m_dismissing = true;
    m_timer->stop();

    QPointer<Toast> guard(this);
    auto finish = [guard]() {
        if (!guard) return;
        guard->hide();
        guard->m_busy = false;
        guard->m_dismissing = false;
        QTimer::singleShot(0, guard, [guard]() {
            if (guard) guard->advance();   // 队列里还有就接着放
        });
    };

    if (!animate || !isVisible() || !Motion::animationsEnabled()) {
        finish();
        return;
    }

    const QPoint startPos = pos();
    const QPoint endPos = startPos + QPoint(0, -10);

    auto* fade = Motion::tween(this, "windowOpacity", windowOpacity(), 0.0, 160, Motion::exitEase());
    auto* slide = new QPropertyAnimation(this, "pos", this);
    slide->setDuration(160);
    slide->setStartValue(startPos);
    slide->setEndValue(endPos);
    slide->setEasingCurve(Motion::exitEase());
    slide->start(QAbstractAnimation::DeleteWhenStopped);

    QObject::connect(fade, &QAbstractAnimation::finished, this, finish);
    QTimer::singleShot(160 + Motion::TimeoutFudgeMs, this, finish);
}

void Toast::paintEvent(QPaintEvent* event)
{
    GlassPanel::paintEvent(event);

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool dark = ThemeManager::instance().isDark();
    QColor accent = ThemeManager::instance().accent();
    QColor dot = (m_kind == Kind::Info) ? accent : kindColor(m_kind);

    // 图标：实心圆 + 白色矢量字形
    const QRectF box(kToastPadX, (height() - kIconBox) / 2.0, kIconBox, kIconBox);
    p.setPen(Qt::NoPen);
    p.setBrush(dot);
    p.drawEllipse(box);

    QPainterPath glyph;
    const QPointF c = box.center();
    switch (m_kind) {
    case Kind::Success: {
        glyph.moveTo(c.x() - 4.2, c.y() + 0.2);
        glyph.lineTo(c.x() - 1.4, c.y() + 3.0);
        glyph.lineTo(c.x() + 4.4, c.y() - 3.2);
        break;
    }
    case Kind::Error: {
        glyph.moveTo(c.x() - 3.4, c.y() - 3.4);
        glyph.lineTo(c.x() + 3.4, c.y() + 3.4);
        glyph.moveTo(c.x() + 3.4, c.y() - 3.4);
        glyph.lineTo(c.x() - 3.4, c.y() + 3.4);
        break;
    }
    case Kind::Warning: {
        glyph.moveTo(c.x(), c.y() - 4.4);
        glyph.lineTo(c.x(), c.y() + 1.2);
        break;
    }
    case Kind::Info:
    default: {
        glyph.moveTo(c.x(), c.y() - 1.0);
        glyph.lineTo(c.x(), c.y() + 4.4);
        break;
    }
    }
    p.setPen(QPen(Qt::white, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(glyph);

    if (m_kind == Kind::Warning || m_kind == Kind::Info) {
        // 上方那个点是「i」/「!」的圆点（单独一个 1.6px 实心点）
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawEllipse(QPointF(c.x(), c.y() - 4.0), 1.05, 1.05);
    }

    // 文字
    const QColor textColor = dark ? QColor(255, 255, 255, 240) : QColor(18, 22, 28, 240);
    p.setPen(textColor);
    const QRect textRect(kToastPadX + kIconBox + kIconGap, 0,
                         width() - (kToastPadX + kIconBox + kIconGap) - kToastPadX, height());
    const QFontMetrics fm(font());
    p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
               fm.elidedText(m_text, Qt::ElideRight, textRect.width()));
}

} // namespace kc::ui
