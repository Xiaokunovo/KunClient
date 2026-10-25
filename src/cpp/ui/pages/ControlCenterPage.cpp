#include "ui/pages/ControlCenterPage.h"

#include "kc/config/AppSettings.h"
#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>

namespace kc::ui {
namespace {

// 动作串。外壳按这些值分派，页面本身不认识「设置」是什么。
const char* kActionTheme     = "theme";
const char* kActionGlass     = "glass";
const char* kActionBlur      = "blur";
const char* kActionAnim      = "anim";
const char* kActionAutoPort  = "autoport";
const char* kActionDebugLog  = "debuglog";
const char* kActionUpdate    = "update";
const char* kActionReset     = "reset";

// 持久化键。autoDetect9008 是串口自动识别，debugVisible 由 LogView 读。
const char* kKeyAutoPort  = "connection/autoDetect9008";
const char* kKeyDebugLog  = "log/debugVisible";

/// 滑杆的取值上限：0~100 直接当百分比用。
constexpr int kSliderMax = 100;

/// 内容列最大宽度：控制中心是「一屏开关」，宽屏下摊到 1600px 会让磁贴之间
/// 空得能跑马，视线要横跨半米才能从「深色模式」扫到「背景动效」。
constexpr int kSheetMaxWidth = 880;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  CcTile
// ─────────────────────────────────────────────────────────────────────────────

CcTile::CcTile(Icon icon, const QString& title, bool action, QWidget* parent)
    : QWidget(parent), m_icon(icon), m_title(title), m_action(action)
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    // 整页比旧面板宽松，磁贴给到 96 —— 旧面板 74 是为了塞进 404px 的面板高度，
    // 现在没有这个约束，再挤着就是浪费一屏空间。
    setFixedHeight(96);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_hint = action ? QString() : QStringLiteral("关");
}

void CcTile::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (!m_action) m_hint = active ? QStringLiteral("开") : QStringLiteral("关");
    update();
}

void CcTile::setHint(const QString& hint) {
    m_hint = hint;
    update();
}

// 图标全部手画：字体字形在部分机器上会落成豆腐块（见头文件里的说明）。
void CcTile::drawIcon(QPainter& p, const QRectF& box, const QColor& color) const {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal u = box.width();          // 以边长为单位，图标随磁贴缩放
    const QPointF c = box.center();

    QPen pen(color, qMax(1.4, u * 0.085));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    switch (m_icon) {
        case Icon::Theme: {
            // 月牙：大圆减去一个偏移的圆（比画弧稳，任何半径都不出毛刺）。
            QPainterPath a;
            a.addEllipse(box.adjusted(u * 0.06, u * 0.06, -u * 0.06, -u * 0.06));
            QPainterPath b;
            b.addEllipse(box.adjusted(u * 0.36, -u * 0.10, u * 0.24, u * 0.24));
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPath(a.subtracted(b));
            break;
        }
        case Icon::Glass: {
            // 玻璃：圆角方框 + 左上角一道斜高光。
            const QRectF r = box.adjusted(u * 0.08, u * 0.08, -u * 0.08, -u * 0.08);
            p.drawRoundedRect(r, u * 0.18, u * 0.18);
            p.setPen(QPen(color, qMax(1.2, u * 0.07), Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(r.left() + u * 0.10, r.top() + u * 0.42),
                       QPointF(r.left() + u * 0.42, r.top() + u * 0.10));
            p.drawLine(QPointF(r.left() + u * 0.12, r.bottom() - u * 0.16),
                       QPointF(r.right() - u * 0.16, r.bottom() - u * 0.16));
            break;
        }
        case Icon::Blur: {
            // 磨砂：三条横线，长度与浓度递减，表达「越远越糊」。
            const qreal cx = box.center().x();
            for (int i = 0; i < 3; ++i) {
                QColor c = color;
                c.setAlphaF(color.alphaF() * (1.0 - i * 0.28));
                p.setPen(QPen(c, qMax(1.6, u * 0.11), Qt::SolidLine, Qt::RoundCap));
                const qreal w = u * (0.78 - i * 0.16);
                const qreal y = box.top() + u * (0.26 + i * 0.24);
                p.drawLine(QPointF(cx - w / 2, y), QPointF(cx + w / 2, y));
            }
            break;
        }
        case Icon::Motion: {
            // 动效：一段圆弧 + 箭头（顺时针），表示「会动」。
            const QRectF arc = box.adjusted(u * 0.14, u * 0.14, -u * 0.14, -u * 0.14);
            p.drawArc(arc, 40 * 16, 265 * 16);
            const QPointF tip(arc.center().x() + arc.width() / 2 * std::cos(qDegreesToRadians(40.0)),
                              arc.center().y() - arc.height() / 2 * std::sin(qDegreesToRadians(40.0)));
            p.setBrush(color);
            p.setPen(Qt::NoPen);
            QPolygonF head;
            head << tip + QPointF(u * 0.02, -u * 0.10) << tip + QPointF(u * 0.12, u * 0.02)
                 << tip + QPointF(-u * 0.06, u * 0.06);
            p.drawPolygon(head);
            break;
        }
        case Icon::Port: {
            // 9008：闪电（EDL 就是靠它进 9008 的）。
            QPolygonF bolt;
            bolt << QPointF(c.x() + u * 0.10, box.top() + u * 0.10)
                 << QPointF(c.x() - u * 0.26, c.y() + u * 0.04)
                 << QPointF(c.x() - u * 0.02, c.y() + u * 0.04)
                 << QPointF(c.x() - u * 0.10, box.bottom() - u * 0.10)
                 << QPointF(c.x() + u * 0.26, c.y() - u * 0.04)
                 << QPointF(c.x() + u * 0.02, c.y() - u * 0.04);
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPolygon(bolt);
            break;
        }
        case Icon::Log: {
            // 日志：四行文本行，宽度不等（像一段流水）。
            const qreal widths[4] = { 0.74, 0.54, 0.66, 0.40 };
            for (int i = 0; i < 4; ++i) {
                p.setPen(QPen(color, qMax(1.4, u * 0.085), Qt::SolidLine, Qt::RoundCap));
                const qreal y = box.top() + u * (0.20 + i * 0.20);
                p.drawLine(QPointF(box.left() + u * 0.14, y),
                           QPointF(box.left() + u * (0.14 + widths[i] * 0.86), y));
            }
            break;
        }
        case Icon::Update: {
            // 检查更新：向上的箭头 + 底部一条托线。
            p.drawLine(QPointF(c.x(), box.top() + u * 0.18),
                       QPointF(c.x(), box.bottom() - u * 0.30));
            p.setBrush(Qt::NoBrush);
            QPolygonF head;
            head << QPointF(c.x(), box.top() + u * 0.14)
                 << QPointF(c.x() - u * 0.20, box.top() + u * 0.36)
                 << QPointF(c.x() + u * 0.20, box.top() + u * 0.36);
            p.setPen(pen);
            p.drawPolyline(head);
            p.drawLine(QPointF(box.left() + u * 0.20, box.bottom() - u * 0.16),
                       QPointF(box.right() - u * 0.20, box.bottom() - u * 0.16));
            break;
        }
        case Icon::Reset: {
            // 恢复默认：逆时针圆弧 + 箭头。
            const QRectF arc = box.adjusted(u * 0.14, u * 0.14, -u * 0.14, -u * 0.14);
            p.drawArc(arc, 140 * 16, 265 * 16);
            const QPointF tip(arc.center().x() + arc.width() / 2 * std::cos(qDegreesToRadians(140.0)),
                              arc.center().y() - arc.height() / 2 * std::sin(qDegreesToRadians(140.0)));
            p.setBrush(color);
            p.setPen(Qt::NoPen);
            QPolygonF head;
            head << tip + QPointF(-u * 0.02, -u * 0.10) << tip + QPointF(u * 0.06, u * 0.06)
                 << tip + QPointF(-u * 0.12, u * 0.02);
            p.drawPolygon(head);
            break;
        }
    }
    p.restore();
}

void CcTile::paintEvent(QPaintEvent*) {
    const ThemeManager& tm = ThemeManager::instance();
    const bool dark = tm.isDark();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const qreal rad = 18.0;

    // 底：未激活 = 一层极淡的白/黑（玻璃上的浮层），激活 = 强调色。
    QColor accent = tm.color(ColorToken::Accent);
    QColor fill;
    if (m_active) {
        fill = accent;
    } else if (dark) {
        fill = QColor(255, 255, 255, m_hover ? 34 : 20);
    } else {
        fill = QColor(15, 23, 42, m_hover ? 26 : 14);
    }
    if (m_hover && !m_active) fill = dark ? QColor(255, 255, 255, 40) : QColor(15, 23, 42, 30);

    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawRoundedRect(r, rad, rad);

    // 未激活态加「顶部高光 + 1px 发丝边」：只铺一层极淡的墨，磁贴在浅色玻璃
    // 页面上就是一块灰雾，看不出材质（2026-10-18 截图实测）。这两笔是玻璃感的来源。
    if (!m_active) {
        QPainterPath clip;
        clip.addRoundedRect(r, rad, rad);
        p.save();
        p.setClipPath(clip);
        QLinearGradient gloss(r.topLeft(), QPointF(r.left(), r.top() + r.height() * 0.55));
        gloss.setColorAt(0.0, dark ? QColor(255, 255, 255, 22) : QColor(255, 255, 255, 168));
        gloss.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.fillRect(r, gloss);
        p.restore();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(dark ? QColor(255, 255, 255, 34) : QColor(15, 23, 42, 26), 1.0));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    }

    // 激活态加一圈亮边：纯色块在玻璃上会显得「贴」不住。
    if (m_active) {
        QColor rim = QColor(255, 255, 255, 90);
        p.setPen(QPen(rim, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), rad, rad);
    }

    const QColor fg = m_active ? QColor(Qt::white)
                              : (dark ? QColor(0xEC, 0xF1, 0xF8) : QColor(0x1B, 0x22, 0x2C));
    const QColor fgDim = m_active ? QColor(255, 255, 255, 190)
                                  : (dark ? QColor(0xA9, 0xB6, 0xC6) : QColor(0x6A, 0x76, 0x85));

    // 图标：左上角。
    drawIcon(p, QRectF(r.left() + 15, r.top() + 14, 26, 26), fg);

    // 状态小字：右上角。
    if (!m_hint.isEmpty()) {
        QFont hf = font();
        hf.setPointSizeF(font().pointSizeF() - 0.5);
        hf.setBold(m_active);
        p.setFont(hf);
        p.setPen(fgDim);
        p.drawText(QRectF(r.left(), r.top() + 15, r.width() - 16, 22),
                   Qt::AlignRight | Qt::AlignVCenter, m_hint);
    }

    // 标题：底部。
    QFont tf = font();
    tf.setPointSizeF(font().pointSizeF() + 0.5);
    tf.setBold(true);
    p.setFont(tf);
    p.setPen(fg);
    p.drawText(QRectF(r.left() + 15, r.bottom() - 34, r.width() - 30, 24),
               Qt::AlignLeft | Qt::AlignVCenter, m_title);
}

void CcTile::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint())) {
        emit clicked();
    }
    QWidget::mouseReleaseEvent(e);
}

void CcTile::enterEvent(QEnterEvent* e) {
    m_hover = true;
    update();
    QWidget::enterEvent(e);
}

void CcTile::leaveEvent(QEvent* e) {
    m_hover = false;
    update();
    QWidget::leaveEvent(e);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ControlCenterPage
// ─────────────────────────────────────────────────────────────────────────────

ControlCenterPage::ControlCenterPage(QWidget* parent) : BasePage(parent)
{
    setObjectName(QStringLiteral("controlCenterPage"));
    buildUi();
    syncTiles();
}

void ControlCenterPage::buildUi()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 12, 24, 18);
    root->setSpacing(0);

    // 内容列：限宽居中。整页铺满时磁贴会被拉到 300px 宽，一行只能看两三个，
    // 反而比旧面板更难扫。
    // 🔴 居中不能用 `addWidget(sheet, 0, Qt::AlignHCenter)`：那样布局只给
    //    sizeHint 宽度（自绘磁贴没有 sizeHint，整列会缩成一条窄带，2026-10-18
    //    截图实测 310px）。用「两端弹簧 + 中间给大拉伸比 + maximumWidth 封顶」，
    //    宽屏时封顶在 880 居中，窄屏时按比例收缩。
    auto* sheet = new QWidget(this);
    sheet->setObjectName(QStringLiteral("ccSheet"));
    sheet->setMaximumWidth(kSheetMaxWidth);
    auto* col = new QVBoxLayout(sheet);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(14);

    // ── 状态条 ──────────────────────────────────────────────────────────
    auto* chips = new QHBoxLayout;
    chips->setSpacing(10);
    auto makeChip = [sheet, chips](const QString& title) {
        auto* c = new QLabel(QStringLiteral("%1 · —").arg(title), sheet);
        c->setObjectName(QStringLiteral("ccChip"));
        c->setProperty("state", QStringLiteral("idle"));
        chips->addWidget(c, 1);
        return c;
    };
    m_chipDevice = makeChip(QStringLiteral("设备"));
    m_chipMode = makeChip(QStringLiteral("模式"));
    m_chipProtocol = makeChip(QStringLiteral("协议"));
    m_chipLink = makeChip(QStringLiteral("链路"));
    col->addLayout(chips);

    // ── 磁贴 4 × 2 ──────────────────────────────────────────────────────
    auto* grid = new QGridLayout;
    grid->setSpacing(12);
    auto addTile = [this, grid, sheet](CcTile* tile, int row, int col_, const char* action) {
        grid->addWidget(tile, row, col_);
        Q_UNUSED(sheet);
        connect(tile, &CcTile::clicked, this, [this, action] {
            emit actionTriggered(QString::fromLatin1(action));
            // 磁贴的「开/关」由外壳应用完再回读真值，这里先刷新一次给即时反馈。
            syncTiles();
        });
    };

    m_tileTheme = new CcTile(CcTile::Icon::Theme, QStringLiteral("深色模式"), false, sheet);
    m_tileGlass = new CcTile(CcTile::Icon::Glass, QStringLiteral("液态玻璃"), false, sheet);
    m_tileBlur = new CcTile(CcTile::Icon::Blur, QStringLiteral("背景模糊"), false, sheet);
    m_tileAnim = new CcTile(CcTile::Icon::Motion, QStringLiteral("背景动效"), false, sheet);
    m_tileAutoPort = new CcTile(CcTile::Icon::Port, QStringLiteral("自动识别 9008"), false, sheet);
    m_tileDebugLog = new CcTile(CcTile::Icon::Log, QStringLiteral("日志详细"), false, sheet);
    m_tileUpdate = new CcTile(CcTile::Icon::Update, QStringLiteral("检查更新"), true, sheet);
    m_tileReset = new CcTile(CcTile::Icon::Reset, QStringLiteral("恢复外观"), true, sheet);

    addTile(m_tileTheme, 0, 0, kActionTheme);
    addTile(m_tileGlass, 0, 1, kActionGlass);
    addTile(m_tileBlur, 0, 2, kActionBlur);
    addTile(m_tileAnim, 0, 3, kActionAnim);
    addTile(m_tileAutoPort, 1, 0, kActionAutoPort);
    addTile(m_tileDebugLog, 1, 1, kActionDebugLog);
    addTile(m_tileUpdate, 1, 2, kActionUpdate);
    addTile(m_tileReset, 1, 3, kActionReset);
    for (int c = 0; c < 4; ++c) grid->setColumnStretch(c, 1);
    col->addLayout(grid);

    // ── 滑杆 ────────────────────────────────────────────────────────────
    auto addSlider = [this, col, sheet](const QString& label, QSlider** out, QLabel** valueOut,
                                        const char* key) {
        auto* row = new QHBoxLayout;
        row->setSpacing(12);
        auto* name = new QLabel(label, sheet);
        name->setObjectName(QStringLiteral("ccSliderLabel"));
        name->setFixedWidth(104);
        row->addWidget(name);

        auto* slider = new QSlider(Qt::Horizontal, sheet);
        slider->setObjectName(QStringLiteral("ccSlider"));
        slider->setRange(0, kSliderMax);
        slider->setSingleStep(1);
        slider->setPageStep(5);
        slider->setCursor(Qt::PointingHandCursor);
        row->addWidget(slider, 1);

        auto* val = new QLabel(QStringLiteral("—"), sheet);
        val->setObjectName(QStringLiteral("ccSliderValue"));
        val->setFixedWidth(48);
        val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row->addWidget(val);

        const QString actionKey = QString::fromLatin1(key);
        connect(slider, &QSlider::valueChanged, this, [this, val, actionKey](int v) {
            val->setText(QStringLiteral("%1%").arg(v));
            emit actionTriggered(QStringLiteral("%1:%2").arg(actionKey).arg(v));
        });
        // 松手才落盘：拖动过程中每一帧都写 settings.json 是没必要的磁盘 IO。
        connect(slider, &QSlider::sliderReleased, this, [this, slider, actionKey] {
            emit actionTriggered(QStringLiteral("%1-commit:%2").arg(actionKey).arg(slider->value()));
        });

        *out = slider;
        *valueOut = val;
        col->addLayout(row);
    };
    addSlider(QStringLiteral("卡片不透明度"), &m_cardSlider, &m_cardValue, "card");
    addSlider(QStringLiteral("背景不透明度"), &m_bgSlider, &m_bgValue, "bg");

    // ── 快捷入口 ────────────────────────────────────────────────────────
    auto* nav = new QHBoxLayout;
    nav->setSpacing(10);
    const struct { const char* key; const char* label; } kNavs[] = {
        { "console", "主屏" },
        { "EdlFlash", "Qualcomm" },
        { "Settings", "设置" },
        { "About", "关于" },
    };
    for (const auto& n : kNavs) {
        auto* b = new QPushButton(QString::fromUtf8(n.label), sheet);
        b->setObjectName(QStringLiteral("ccNav"));
        b->setCursor(Qt::PointingHandCursor);
        const QString key = QString::fromLatin1(n.key);
        connect(b, &QPushButton::clicked, this, [this, key] {
            emit actionTriggered(QStringLiteral("nav:%1").arg(key));
        });
        nav->addWidget(b, 1);
    }
    col->addLayout(nav);

    // ── 底部提示 ────────────────────────────────────────────────────────
    // 横滑是这个页面唯一的「非常规」入口，不写一句没人会想到往右划。
    auto* hint = new QLabel(QStringLiteral("向右滑 · 返回主屏"), sheet);
    hint->setObjectName(QStringLiteral("ccHint"));
    hint->setAlignment(Qt::AlignHCenter);
    col->addWidget(hint);

    // 🔴 上下都要 stretch：只在末尾放一个的话内容会**贴死在顶栏下沿**（芯片行
    //    离顶栏只剩 5px，看着像被裁掉一截），而底下空出两百多像素（2026-10-18
    //    截图实测）。两端各 1 份，内容自然垂直居中。
    root->addStretch(1);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    row->addStretch(1);
    row->addWidget(sheet, 10);
    row->addStretch(1);
    root->addLayout(row);
    root->addStretch(1);
}

void ControlCenterPage::syncTiles()
{
    const ThemeManager& tm = ThemeManager::instance();
    const kc::AppSettings& s = kc::AppSettings::current();
    QSettings qs;

    m_tileTheme->setActive(tm.isDark());
    // 「液态玻璃」= 卡片更透（磨砂感更明显）；关掉就回到实心卡片。
    m_tileGlass->setActive(s.cardOpacity < 0.70);
    m_tileBlur->setActive(s.backgroundBlur > 0.5);
    m_tileAnim->setActive(s.backgroundAnimation != QStringLiteral("None"));
    m_tileAutoPort->setActive(qs.value(QString::fromLatin1(kKeyAutoPort), true).toBool());
    m_tileDebugLog->setActive(qs.value(QString::fromLatin1(kKeyDebugLog), false).toBool());
    m_tileUpdate->setHint(QStringLiteral("›"));
    m_tileReset->setHint(QStringLiteral("›"));

    const int card = qRound(qBound(0.0, s.cardOpacity, 1.0) * kSliderMax);
    const int bg = qRound(qBound(0.0, s.backgroundOpacity, 1.0) * kSliderMax);
    if (m_cardSlider) {
        QSignalBlocker b(m_cardSlider);
        m_cardSlider->setValue(card);
        m_cardValue->setText(QStringLiteral("%1%").arg(card));
    }
    if (m_bgSlider) {
        QSignalBlocker b(m_bgSlider);
        m_bgSlider->setValue(bg);
        m_bgValue->setText(QStringLiteral("%1%").arg(bg));
    }
}

void ControlCenterPage::setStatus(const QString& device, const QString& mode,
                                  const QString& protocol, const QString& link,
                                  const QString& linkState) {
    auto put = [](QLabel* label, const QString& title, const QString& value,
                  const QString& state) {
        if (!label) return;
        label->setText(QStringLiteral("%1 · %2").arg(title, value.isEmpty() ? QStringLiteral("—")
                                                                           : value));
        label->setProperty("state", state);
        label->style()->unpolish(label);
        label->style()->polish(label);
    };
    const QString linkState2 = linkState.isEmpty() ? QStringLiteral("idle") : linkState;
    put(m_chipDevice, QStringLiteral("设备"), device, QStringLiteral("idle"));
    put(m_chipMode, QStringLiteral("模式"), mode, QStringLiteral("idle"));
    put(m_chipProtocol, QStringLiteral("协议"), protocol,
        protocol == QStringLiteral("异常") ? QStringLiteral("failed") : QStringLiteral("ok"));
    put(m_chipLink, QStringLiteral("链路"), link, linkState2);
}

void ControlCenterPage::onActivated()
{
    syncTiles();
    animateTilesIn();
}

void ControlCenterPage::animateTilesIn()
{
    // 顺序 = 网格的行优先序（先第一排从左到右，再第二排）。
    // 🔴 只能用 Motion::fadeIn（纯透明度）：磁贴位置归 QGridLayout 管，
    //    走 reveal 的话它内部 move() 出来的位移会被下一次重排抹掉，
    //    只剩淡入 —— 而且中途还会闪一下错位。
    CcTile* const tiles[] = {m_tileTheme,    m_tileGlass,    m_tileBlur,    m_tileAnim,
                             m_tileAutoPort, m_tileDebugLog, m_tileUpdate, m_tileReset};
    constexpr int kStepMs = 18;    // 相邻两格的点亮间隔
    constexpr int kFadeMs = 200;   // 单格淡入时长
    int index = 0;
    for (CcTile* tile : tiles) {
        if (!tile) continue;
        Motion::fadeIn(tile, kFadeMs, index * kStepMs);
        ++index;
    }
}

} // namespace kc::ui
