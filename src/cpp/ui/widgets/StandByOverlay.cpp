#include "ui/widgets/StandByOverlay.h"

#include "kc/diag/FlashHistory.h"
#include "ui/ShellStatus.h"
#include "ui/theme/Motion.h"
#include "ui/theme/ThemeManager.h"

#include <QAbstractAnimation>
#include <QApplication>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>

#include <cmath>

namespace kc::ui {

namespace {

constexpr int kClockPx = 96;
constexpr int kCardRadius = 18;
constexpr int kCardPadX = 16;
constexpr int kCardPadY = 11;

/// 日期行：`yyyy 年 M 月 d 日 · 星期X`。
/// 🔴 星期**自己拼**：QLocale 的中文星期格式取决于系统语言，英文机器上会吐出
///    "Wednesday"。手写一张 7 项表最稳，也与机器语言彻底解耦。
QString dateLine(const QDateTime& now)
{
    static const QStringList kWeek{
        QStringLiteral("日"), QStringLiteral("一"), QStringLiteral("二"), QStringLiteral("三"),
        QStringLiteral("四"), QStringLiteral("五"), QStringLiteral("六"),
    };
    const QDate d = now.date();
    // dayOfWeek：1=周一 … 7=周日；%7 把周日映射到 0，正好对上表的「日」在首位。
    return QStringLiteral("%1 年 %2 月 %3 日 · 星期%4")
        .arg(d.year())
        .arg(d.month())
        .arg(d.day())
        .arg(kWeek.value(d.dayOfWeek() % 7));
}

/// 造一个「透明底 + 固定字号 / 字重 / 白色 alpha」的标签，并挂一个淡入用的效果。
///
/// 🔴 字号 / 字重必须写进**控件级样式表**：全局 QSS 有 `* { font-size: 13px; }`，
///    它在 polish 时会把 setFont 的 96px 直接盖掉（Toast.cpp 里同一个坑），而
///    控件级样式表优先级高于应用级，所以这里再钉一遍。setFont 仍然要设 ——
///    构造后、polish 前量字宽（relayout 里算时钟宽度）靠的是它。
/// 🔴 淡入用 QGraphicsOpacityEffect **只挂在叶子标签上**：叶子是一次独立的小
///    合成，不参与本层自绘；把效果挂在自绘容器上才是要避开的那条路（见头文件）。
QLabel* makeLabel(QWidget* parent, int px, int alpha, int weight, Qt::Alignment align)
{
    auto* label = new QLabel(parent);
    // 整层是一颗大按钮：任何子控件都不能把点击截走。
    label->setAttribute(Qt::WA_TransparentForMouseEvents, true);

    QFont f = label->font();
    f.setPixelSize(px);
    f.setWeight(static_cast<QFont::Weight>(weight));
    label->setFont(f);

    label->setStyleSheet(QStringLiteral("background: transparent;"
                                        " color: rgba(255, 255, 255, %1);"
                                        " font-size: %2px;"
                                        " font-weight: %3;")
                             .arg(alpha)
                             .arg(px)
                             .arg(weight));
    label->setAlignment(align);

    auto* effect = new QGraphicsOpacityEffect(label);
    effect->setOpacity(1.0);
    label->setGraphicsEffect(effect);
    return label;
}

/// 改一个标签的淡入透明度（没有效果时静默跳过 —— 正常路径下一定有）。
void setLabelFade(QLabel* label, qreal alpha)
{
    if (!label) return;
    if (auto* effect = qobject_cast<QGraphicsOpacityEffect*>(label->graphicsEffect()))
        effect->setOpacity(alpha);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  ReadoutCard —— 待机层底部的一块读数卡
//
//  为什么自绘而不是纯 QSS：卡底是「10% 白填充 + 22% 白描边 + 18px 圆角」，
//  用 QSS 的话得给容器写一套、再给里面每个 QLabel 逐个 `background: transparent`
//  覆盖，主题切换时还要整表重放 —— 一个 paintEvent 十几行就说清了。
//
//  为什么省略号在这里做而不是交给 QLabel：设备串
//  （"COM7 · 小米 23127PN0CC · 0x1234abcd"）在 640 宽窗口里一张卡只有 ~150px
//  可用，QLabel 不省略就是硬裁。卡里留着**原始文本**，每次 resize / 换文案
//  按当前宽度重新 elide 一次。
// ─────────────────────────────────────────────────────────────────────────────
class ReadoutCard : public QWidget {
public:
    ReadoutCard(const QString& title, bool withSubtitle, QWidget* parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);

        m_title = makeLabel(this, 11, 140, 500, Qt::AlignLeft | Qt::AlignVCenter);
        m_title->setText(title);

        m_value = makeLabel(this, 17, 235, 500, Qt::AlignLeft | Qt::AlignVCenter);
        if (withSubtitle) {
            m_subtitle = makeLabel(this, 11, 110, 400, Qt::AlignLeft | Qt::AlignVCenter);
            m_subtitle->hide();   // 没内容时不占位
        }
    }

    QLabel* valueLabel() const { return m_value; }
    QLabel* subtitleLabel() const { return m_subtitle; }

    void setValueText(const QString& text)
    {
        m_valueRaw = text;
        layoutLabels();
    }

    void setSubtitleText(const QString& text)
    {
        m_subtitleRaw = text;
        if (m_subtitle) m_subtitle->setVisible(!text.isEmpty());
        layoutLabels();
    }

    /// 卡底与卡内标题的不透明度（0~1）。值 / 副标题由 StandByOverlay 统一分发。
    void setCardOpacity(qreal alpha)
    {
        if (qFuzzyCompare(m_opacity + 1.0, alpha + 1.0)) return;
        m_opacity = alpha;
        setLabelFade(m_title, alpha);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        // 0.5px 内缩：1px 描边落在像素中心，四角才不会被裁掉一半。
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(QColor(255, 255, 255, qRound(22 * m_opacity)), 1.0));
        p.setBrush(QColor(255, 255, 255, qRound(10 * m_opacity)));
        p.drawRoundedRect(r, kCardRadius, kCardRadius);
    }

    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        layoutLabels();
    }

private:
    void layoutLabels()
    {
        const int w = width();
        const int h = height();
        if (w <= 0 || h <= 0) return;

        const int innerW = qMax(24, w - kCardPadX * 2);
        const int titleH = 16;
        m_title->setGeometry(kCardPadX, kCardPadY, innerW, titleH);

        const bool hasSub = m_subtitle && !m_subtitleRaw.isEmpty();
        const int valueY = kCardPadY + titleH + 2;
        if (hasSub) {
            const int subH = 15;
            const int subY = h - kCardPadY - subH;
            m_subtitle->setGeometry(kCardPadX, subY, innerW, subH);
            m_value->setGeometry(kCardPadX, valueY, innerW, qMax(20, subY - 2 - valueY));
        } else {
            m_value->setGeometry(kCardPadX, valueY, innerW, qMax(20, h - kCardPadY - valueY));
        }

        elide(m_value, m_valueRaw);
        if (m_subtitle) elide(m_subtitle, m_subtitleRaw);
    }

    static void elide(QLabel* label, const QString& raw)
    {
        if (!label) return;
        const QFontMetrics fm(label->font());
        label->setText(fm.elidedText(raw, Qt::ElideRight, qMax(12, label->width())));
    }

    QLabel* m_title = nullptr;
    QLabel* m_value = nullptr;
    QLabel* m_subtitle = nullptr;
    QString m_valueRaw;
    QString m_subtitleRaw;
    qreal m_opacity = 1.0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  StandByOverlay
// ─────────────────────────────────────────────────────────────────────────────

StandByOverlay::StandByOverlay(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("standByOverlay"));
    setFocusPolicy(Qt::StrongFocus);
    // 自绘整层（含深底），不要 QSS 再糊一层底 —— 否则半透明底会被盖成实心。
    setAttribute(Qt::WA_StyledBackground, false);
    // 自绘控件不铺系统底，才能和底下的主界面做 alpha 混合（同 IslandScrim）。
    setAttribute(Qt::WA_NoSystemBackground, true);
    setCursor(Qt::PointingHandCursor);

    // ── 时钟 / 日期 / 底部提示 ──────────────────────────────────────────────
    // 时钟：96px 细字重（iOS StandBy 的主时钟只有时分，不要秒）。
    m_timeLabel = makeLabel(this, kClockPx, 240, QFont::Light, Qt::AlignCenter);
    m_timeLabel->setText(QStringLiteral("--:--"));
    // 日期：13px，右上角。
    m_dateLabel = makeLabel(this, 13, 150, 500, Qt::AlignRight | Qt::AlignVCenter);
    // 底部提示：12px，整行居中。
    m_hintLabel = makeLabel(this, 12, 115, 400, Qt::AlignCenter);
    m_hintLabel->setText(QStringLiteral("点击任意处返回"));

    // ── 三块读数卡：卡 3 多一行副标题（上次刷机时间）────────────────────────
    m_deviceCard = new ReadoutCard(QStringLiteral("设备"), false, this);
    m_linkCard = new ReadoutCard(QStringLiteral("链路"), false, this);
    m_lastCard = new ReadoutCard(QStringLiteral("上次刷机"), true, this);

    m_deviceValue = m_deviceCard->valueLabel();
    m_linkValue = m_linkCard->valueLabel();
    m_lastValue = m_lastCard->valueLabel();
    m_lastSubtitle = m_lastCard->subtitleLabel();

    // ── 秒级时钟 ────────────────────────────────────────────────────────────
    m_clock = new QTimer(this);
    m_clock->setInterval(1000);
    connect(m_clock, &QTimer::timeout, this, &StandByOverlay::tick);

    // 跟着宿主长：外壳即使忘了在 resizeEvent 里同步几何，这层也不会露出半截桌面。
    if (parent) {
        parent->installEventFilter(this);
        setGeometry(parent->rect());
    }

    m_alpha = 0.0;
    applyAlphaToChildren();
    relayout();
    hide();
}

// ── 显示 / 隐藏 ─────────────────────────────────────────────────────────────

void StandByOverlay::showOverlay()
{
    const bool wasVisible = isVisible();
    const qreal from = wasVisible ? m_alpha : 0.0;

    if (QWidget* host = parentWidget()) setGeometry(host->rect());

    // 读数与时间先摆好再 show：第一帧就是完整画面，不会闪一下 "--:--"。
    rebuildTexts();
    tick();

    if (!wasVisible) m_prevFocus = QApplication::focusWidget();

    // 🔴 先把 alpha 落到起点再 show：show() 之后到第一次 setFade 之间若插进一次
    //    重绘（同帧的 show 事件里就会），会看到满不透明度的一帧 —— 那才是闪。
    if (!Motion::animationsEnabled() || from >= 0.999) setFade(1.0);
    else setFade(from);

    show();
    raise();
    setFocus(Qt::OtherFocusReason);
    if (m_clock) m_clock->start();

    if (!Motion::animationsEnabled() || from >= 0.999) return;

    // 这一轮作废所有在飞的旧动画（收尾回调会检查代际号，对不上就让位）。
    const int gen = ++m_fadeGen;
    Motion::clearAnimation(this, "fade", std::nan(""));

    QPointer<StandByOverlay> guard(this);
    auto* anim = Motion::tween(this, "fade", from, 1.0, 220, Motion::silkEase());
    if (anim) {
        connect(anim, &QAbstractAnimation::finished, this, [guard, gen]() {
            if (!guard || guard->m_fadeGen != gen) return;   // 已被新的 show/hide 顶替
            guard->setFade(1.0);
        });
    }
}

void StandByOverlay::hideOverlay()
{
    if (!isVisible()) return;

    const int gen = ++m_fadeGen;
    if (m_clock) m_clock->stop();
    Motion::clearAnimation(this, "fade", std::nan(""));

    QPointer<StandByOverlay> guard(this);
    auto finish = [guard, gen]() {
        if (!guard || guard->m_fadeGen != gen) return;
        guard->hide();
        guard->setFade(0.0);
        // 焦点还回去：待机层吃掉了 Esc / Space，不还的话回工作界面后键盘导航是断的。
        if (guard->m_prevFocus) {
            guard->m_prevFocus->setFocus(Qt::OtherFocusReason);
            guard->m_prevFocus.clear();
        }
    };

    if (!Motion::animationsEnabled()) {
        finish();
        return;
    }

    auto* anim = Motion::tween(this, "fade", m_alpha, 0.0, 160, Motion::exitEase());
    if (anim) connect(anim, &QAbstractAnimation::finished, this, finish);
    // 兜底：动画被系统吞掉（窗口最小化 / 定时器被压）时也要真的收掉。
    QTimer::singleShot(160 + Motion::TimeoutFudgeMs, this, finish);
}

void StandByOverlay::setFade(qreal value)
{
    const qreal v = qBound(0.0, value, 1.0);
    if (qFuzzyCompare(m_alpha + 1.0, v + 1.0)) return;
    m_alpha = v;
    applyAlphaToChildren();
    update();
}

void StandByOverlay::applyAlphaToChildren()
{
    setLabelFade(m_timeLabel, m_alpha);
    setLabelFade(m_dateLabel, m_alpha);
    setLabelFade(m_hintLabel, m_alpha);
    setLabelFade(m_deviceValue, m_alpha);
    setLabelFade(m_linkValue, m_alpha);
    setLabelFade(m_lastValue, m_alpha);
    setLabelFade(m_lastSubtitle, m_alpha);
    if (m_deviceCard) m_deviceCard->setCardOpacity(m_alpha);
    if (m_linkCard) m_linkCard->setCardOpacity(m_alpha);
    if (m_lastCard) m_lastCard->setCardOpacity(m_alpha);
}

// ── 读数 ────────────────────────────────────────────────────────────────────

void StandByOverlay::setReadings(const QString& device, const QString& mode, const QString& link,
                                 const QString& task, int percent)
{
    m_device = device;
    m_mode = mode;
    m_link = link;
    m_task = task;
    m_percent = percent;
    m_haveReadings = true;
    rebuildTexts();
}

void StandByOverlay::rebuildTexts()
{
    QString device, mode, link, task;
    int percent = -1;

    if (m_haveReadings) {
        device = m_device;
        mode = m_mode;
        link = m_link;
        task = m_task;
        percent = m_percent;
    } else {
        const ShellStatus::Snapshot s = ShellStatus::instance().snapshot();
        device = s.device;
        mode = s.mode;
        link = s.link;
        task = s.task;
        // percent 只在真有任务时才有意义：空闲态 ShellStatus 停在 100%（endTask
        // 的口径），直接拿来显示会变成「就绪 · 100%」这种吓人的读数。
        percent = s.busy ? qRound(s.percent) : -1;
    }

    // ── 卡 1：设备 ──────────────────────────────────────────────────────────
    const QString dev = device.trimmed();
    const bool noDevice = dev.isEmpty() || dev == QLatin1String("-");
    if (m_deviceCard)
        m_deviceCard->setValueText(noDevice ? QStringLiteral("未连接") : dev);

    // ── 卡 2：链路（模式 · 连接状态；没有设备时这一格没有意义）──────────────
    QString linkText;
    if (noDevice) {
        linkText = QStringLiteral("—");
    } else {
        QStringList parts;
        const QString m = mode.trimmed();
        const QString l = link.trimmed();
        if (!m.isEmpty() && m != QLatin1String("-")) parts << m;
        if (!l.isEmpty() && l != QLatin1String("-")) parts << l;
        linkText = parts.isEmpty() ? QStringLiteral("—") : parts.join(QStringLiteral(" · "));
    }
    if (m_linkCard) m_linkCard->setValueText(linkText);

    // ── 卡 3：上次刷机（FlashHistory 最新在前）───────────────────────────────
    if (m_lastCard) {
        const QVector<kc::diag::FlashRecord> records =
            kc::diag::FlashHistory::instance().records();
        if (records.isEmpty()) {
            m_lastCard->setValueText(QStringLiteral("暂无记录"));
            m_lastCard->setSubtitleText(QString());
        } else {
            const kc::diag::FlashRecord& record = records.first();
            const QString op = record.operation.trimmed();
            m_lastCard->setValueText(QStringLiteral("%1 · %2")
                                         .arg(op.isEmpty() ? QStringLiteral("刷机") : op,
                                              record.success ? QStringLiteral("成功")
                                                             : QStringLiteral("失败")));
            m_lastCard->setSubtitleText(record.time.isValid()
                                            ? record.time.toString(
                                                  QStringLiteral("MM-dd HH:mm"))
                                            : QString());
        }
    }

    // ── 底部提示：常态是返回说明；有任务在跑时把「在干什么、到哪了」带上 ────
    //  待机层没有独立的「任务 / 进度」格，借这一行最省，也不多占版面。
    const QString taskName = task.trimmed();
    const bool hasTask = !taskName.isEmpty() && taskName != QLatin1String("-")
                         && taskName != QLatin1String("就绪");
    QString hint = QStringLiteral("点击任意处返回");
    if (hasTask) {
        hint = (percent >= 0)
                   ? QStringLiteral("正在 %1 · %2  ·  %3")
                         .arg(taskName, QStringLiteral("%1%").arg(percent),
                              QStringLiteral("点击任意处返回"))
                   : QStringLiteral("正在 %1  ·  %2")
                         .arg(taskName, QStringLiteral("点击任意处返回"));
    }
    if (m_hintLabel) m_hintLabel->setText(hint);
}

void StandByOverlay::tick()
{
    // 🔴 这里**只改文字**：每秒重建控件会闪，也会把淡入的代际搞乱。
    const QDateTime now = QDateTime::currentDateTime();
    if (m_timeLabel) m_timeLabel->setText(now.time().toString(QStringLiteral("HH:mm")));
    if (m_dateLabel) m_dateLabel->setText(dateLine(now));   // 跨零点要换星期
}

// ── 布局（绝对定位，不用布局管理器）──────────────────────────────────────────

void StandByOverlay::relayout()
{
    const int w = width();
    const int h = height();
    if (w <= 0 || h <= 0) return;

    const int pad = qMax(24, qRound(w * 0.055));

    // ① 底部提示：贴底留白
    const int hintH = 22;
    const int hintY = h - qMax(18, qRound(h * 0.045)) - hintH;

    // ② 三块读数卡：底部一行等分
    const int gap = qMax(12, qRound(w * 0.014));
    const int cardH = qBound(88, qRound(h * 0.12), 108);
    const int cardY = hintY - qMax(16, qRound(h * 0.026)) - cardH;
    const int totalW = qMax(300, w - pad * 2);
    const int cardW = qMax(120, (totalW - gap * 2) / 3);
    const int rowW = cardW * 3 + gap * 2;
    const int rowX = (w - rowW) / 2;   // 超宽窗口时两边留白对称

    if (m_deviceCard) m_deviceCard->setGeometry(rowX, cardY, cardW, cardH);
    if (m_linkCard) m_linkCard->setGeometry(rowX + cardW + gap, cardY, cardW, cardH);
    if (m_lastCard) m_lastCard->setGeometry(rowX + (cardW + gap) * 2, cardY, cardW, cardH);

    // ③ 主时钟：卡上方那块区域里居中偏上（0.42 是视觉重心，不是几何中点）
    if (m_timeLabel) {
        QFont clockFont = m_timeLabel->font();
        clockFont.setPixelSize(kClockPx);
        const QFontMetrics fm(clockFont);
        // 宽度按「88:88」量并定死：HH:mm 每分钟都在换数字，宽度不定就会左右抖 1px。
        const int timeW = fm.horizontalAdvance(QStringLiteral("88:88")) + 16;
        const int timeH = fm.height() + 8;
        const int areaTop = qMax(20, qRound(h * 0.08));
        const int areaBottom = cardY - qMax(16, qRound(h * 0.03));
        int timeY = areaTop;
        if (areaBottom - timeH > areaTop)
            timeY = areaTop + qRound((areaBottom - areaTop - timeH) * 0.42);
        m_timeLabel->setGeometry((w - timeW) / 2, timeY, timeW, timeH);
    }

    // ④ 日期 / 星期：右上角
    if (m_dateLabel) {
        const int dateW = qMin(300, qMax(160, w - pad * 2));
        const int dateH = 22;
        m_dateLabel->setGeometry(w - pad - dateW, qMax(16, qRound(h * 0.045)), dateW, dateH);
    }

    // ⑤ 底部提示：整行居中
    if (m_hintLabel) m_hintLabel->setGeometry(pad, hintY, qMax(80, w - pad * 2), hintH);
}

// ── 绘制 ────────────────────────────────────────────────────────────────────

void StandByOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal a = m_alpha;
    const bool dark = ThemeManager::instance().isDark();

    // ① 整层深底。🔴 浅色主题也压深（只是略偏蓝一档）——「熄灯」语义，铺白底
    //    会像程序崩了。alpha 248 / 244 而非 255：底下界面留一丝微光透上来，
    //    看着像「玻璃罩住」而不是「贴了一张黑图」。
    //    🔴 别退回到 232/224 那种「半透」：实测 232 时主屏图标在暗底上仍然
    //       清清楚楚，待机层读起来像「蒙了层灰」而不是「熄灯」，大时钟反而
    //       抢不过背景（2026-10-30 截图对比）。
    const QColor base = dark ? QColor(8, 10, 14) : QColor(10, 14, 20);
    const int baseAlpha = dark ? 248 : 244;
    p.fillRect(rect(), QColor(base.red(), base.green(), base.blue(), qRound(baseAlpha * a)));

    // ② 极淡的径向光晕：中心偏上，白 8%。让大块深色有「光从上面打下来」的层次。
    const QPointF center(width() * 0.5, height() * 0.34);
    const qreal radius = qMax(width(), height()) * 0.62;
    QRadialGradient glow(center, radius);
    glow.setColorAt(0.0, QColor(255, 255, 255, qRound(20 * a)));    // 8% ≈ 20/255
    glow.setColorAt(0.55, QColor(255, 255, 255, qRound(6 * a)));
    glow.setColorAt(1.0, QColor(255, 255, 255, 0));
    p.fillRect(rect(), glow);

    // ③ 顶部 1px 高光：中间亮、两端收 —— 一道硬切的白线会像边框，渐变才像高光。
    if (width() > 0) {
        QLinearGradient top(0, 0, width(), 0);
        top.setColorAt(0.0, QColor(255, 255, 255, 0));
        top.setColorAt(0.5, QColor(255, 255, 255, qRound(46 * a)));
        top.setColorAt(1.0, QColor(255, 255, 255, 0));
        p.fillRect(QRect(0, 0, width(), 1), top);
    }
}

// ── 事件 ────────────────────────────────────────────────────────────────────

void StandByOverlay::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
}

bool StandByOverlay::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) {
        if (QWidget* host = parentWidget()) setGeometry(host->rect());
    }
    return QWidget::eventFilter(watched, event);
}

void StandByOverlay::mousePressEvent(QMouseEvent* event)
{
    // 整层就是一颗大按钮：任何按键都退出。子控件全部 WA_TransparentForMouseEvents，
    // 所以点在任何位置都会落到这里。
    Q_UNUSED(event);
    hideOverlay();
    emit dismissed();
}

void StandByOverlay::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        hideOverlay();
        emit dismissed();
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

} // namespace kc::ui
