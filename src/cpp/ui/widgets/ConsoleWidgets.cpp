#include "ui/widgets/ConsoleWidgets.h"

#include "kc/diag/FlashHistory.h"
#include "ui/ShellStatus.h"
#include "ui/theme/GlassSettings.h"
#include "ui/theme/ThemeManager.h"
#include "ui/widgets/GlassSurface.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>

namespace kc::ui {
namespace {

// 卡高与间距。92 = 标题 14 + 值 26 + 副值 14 + 三段内边距 38，是「一眼读完
// 又不占掉主屏一整行图标」的折中值。
constexpr int kCardHeight = 92;
constexpr int kCardWidth = 208;
constexpr int kCardGap = 12;
constexpr int kCardPadX = 16;
constexpr int kCardPadY = 12;

/// 主色 / 次色：小组件压在壁纸上，必须自己保证对比度，不能依赖 QSS（这些标签
/// 在自绘玻璃里，QSS 的背景规则会打架）。深色主题用白系，浅色用近黑。
QString primaryColor()
{
    return ThemeManager::instance().isDark() ? QStringLiteral("#F2F6FC")
                                             : QStringLiteral("#12161C");
}

QString secondaryColor()
{
    return ThemeManager::instance().isDark() ? QStringLiteral("rgba(255,255,255,0.62)")
                                             : QStringLiteral("rgba(18,22,28,0.60)");
}

/// 「3 分钟前 / 2 小时前 / 3 天前」。刷机记录里时间跨度很大，绝对时间反而
/// 要心算；相对时间一眼就知道「是不是刚才那一次」。
QString relativeTime(const QDateTime& then)
{
    if (!then.isValid()) return QStringLiteral("—");
    const qint64 seconds = then.secsTo(QDateTime::currentDateTime());
    if (seconds < 60) return QStringLiteral("刚刚");
    if (seconds < 3600) return QStringLiteral("%1 分钟前").arg(seconds / 60);
    if (seconds < 86400) return QStringLiteral("%1 小时前").arg(seconds / 3600);
    return QStringLiteral("%1 天前").arg(seconds / 86400);
}

QString weekdayName(int dayOfWeek)
{
    // Qt 的 dayOfWeek 是 1=周一 … 7=周日。
    static const char* kNames[] = {"一", "二", "三", "四", "五", "六", "日"};
    if (dayOfWeek < 1 || dayOfWeek > 7) return QString();
    return QString::fromUtf8(kNames[dayOfWeek - 1]);
}

} // namespace

ConsoleWidgets::ConsoleWidgets(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("consoleWidgets"));
    // 自绘玻璃卡是子控件，本层只负责摆位 —— 背景必须透，否则壁纸被糊一层。
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFixedSize(preferredSize());

    // 三块卡：设备 / 最近刷机 / 时钟。makeCard 内部建 GlassSurface + 两行文字。
    auto* device = makeCard(QStringLiteral("设备"), &m_deviceValue, &m_deviceSub);
    auto* flash = makeCard(QStringLiteral("最近刷机"), &m_flashValue, &m_flashSub);
    auto* clock = makeCard(QStringLiteral("时间"), &m_clockValue, &m_clockSub);
    m_cards = {device, flash, clock};

    m_clockTimer = new QTimer(this);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout, this, [this] {
        if (!m_clockValue || !m_clockSub) return;
        const QDateTime now = QDateTime::currentDateTime();
        m_clockValue->setText(now.toString(QStringLiteral("HH:mm")));
        m_clockSub->setText(QStringLiteral("%1 · 星期%2")
                                .arg(now.toString(QStringLiteral("M 月 d 日")),
                                     weekdayName(now.date().dayOfWeek())));
    });

    relayout();
    refreshData();
}

QSize ConsoleWidgets::preferredSize()
{
    return QSize(kCardWidth * 3 + kCardGap * 2, kCardHeight);
}

GlassSurface* ConsoleWidgets::makeCard(const QString& caption, QLabel** value, QLabel** sub)
{
    auto* card = new GlassSurface(this);
    card->setRadius(22);
    card->setInteractive(false);   // 小组件是「看」的，按压果冻会误导成可点
    card->setFallbackBackdrop(true);
    card->setStyle(currentGlassStyle());

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(kCardPadX, kCardPadY, kCardPadX, kCardPadY);
    layout->setSpacing(2);

    auto* captionLabel = new QLabel(caption, card);
    captionLabel->setStyleSheet(
        QStringLiteral("background: transparent; color: %1; font-size: 11px; "
                       "font-weight: 600; letter-spacing: .02em;")
            .arg(secondaryColor()));
    layout->addWidget(captionLabel);

    auto* valueLabel = new QLabel(QStringLiteral("—"), card);
    valueLabel->setStyleSheet(
        QStringLiteral("background: transparent; color: %1; font-size: 17px; "
                       "font-weight: 700;")
            .arg(primaryColor()));
    valueLabel->setWordWrap(false);
    layout->addWidget(valueLabel);

    auto* subLabel = new QLabel(QString(), card);
    subLabel->setStyleSheet(
        QStringLiteral("background: transparent; color: %1; font-size: 11px;")
            .arg(secondaryColor()));
    subLabel->setWordWrap(false);
    layout->addWidget(subLabel);

    layout->addStretch(1);

    *value = valueLabel;
    *sub = subLabel;
    return card;
}

void ConsoleWidgets::relayout()
{
    // 绝对摆位（不用布局）：本控件尺寸固定，三块卡等宽等距，手摆比嵌两层
    // 布局更直观，也避免 GlassSurface 被布局反复 resize 触发重建背板。
    int x = 0;
    for (GlassSurface* card : std::as_const(m_cards)) {
        if (!card) continue;
        card->setGeometry(x, 0, kCardWidth, kCardHeight);
        x += kCardWidth + kCardGap;
    }
}

void ConsoleWidgets::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    relayout();
}

void ConsoleWidgets::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_clockWanted) m_clockTimer->start();
}

void ConsoleWidgets::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    m_clockTimer->stop();
}

void ConsoleWidgets::setClockRunning(bool running)
{
    m_clockWanted = running;
    if (running) m_clockTimer->start();
    else m_clockTimer->stop();
}

void ConsoleWidgets::setBackdrop(const QImage& backdrop)
{
    for (GlassSurface* card : std::as_const(m_cards)) {
        if (card) card->setRawBackdrop(backdrop);
    }
}

void ConsoleWidgets::refreshGlass()
{
    for (GlassSurface* card : std::as_const(m_cards)) {
        if (card) card->setStyle(currentGlassStyle());
    }
}

void ConsoleWidgets::refreshData()
{
    const ShellStatus::Snapshot snap = ShellStatus::instance().snapshot();

    // ① 设备：芯片 / 模式两行，链路状态进副行。
    const bool connected = !snap.device.isEmpty() && snap.device != QLatin1String("-");
    if (m_deviceValue) {
        m_deviceValue->setText(connected ? snap.device : QStringLiteral("未连接"));
        m_deviceValue->setStyleSheet(
            QStringLiteral("background: transparent; color: %1; font-size: 17px; "
                           "font-weight: 700;")
                .arg(connected ? primaryColor()
                               : (ThemeManager::instance().isDark()
                                      ? QStringLiteral("rgba(255,255,255,0.48)")
                                      : QStringLiteral("rgba(18,22,28,0.44)"))));
    }
    if (m_deviceSub) {
        const QString mode = snap.mode.isEmpty() || snap.mode == QLatin1String("-")
            ? QString() : snap.mode;
        m_deviceSub->setText(connected
                                 ? (mode.isEmpty() ? snap.link
                                                   : QStringLiteral("%1 · %2").arg(mode, snap.link))
                                 : QStringLiteral("连接设备后这里显示型号与链路"));
    }

    // ② 最近刷机：只取最新一条。没有记录时给一句「引导」，别让卡空着。
    const QVector<kc::diag::FlashRecord> records =
        kc::diag::FlashHistory::instance().records();
    if (records.isEmpty()) {
        if (m_flashValue) m_flashValue->setText(QStringLiteral("暂无记录"));
        if (m_flashSub) m_flashSub->setText(QStringLiteral("完成一次刷写后自动记录"));
    } else {
        const kc::diag::FlashRecord& latest = records.first();
        if (m_flashValue) {
            m_flashValue->setText(QStringLiteral("%1 · %2")
                                      .arg(latest.operation.isEmpty() ? QStringLiteral("操作")
                                                                      : latest.operation,
                                           latest.success ? QStringLiteral("成功")
                                                          : QStringLiteral("失败")));
            // 成败着色与刷机记录窗同口径（深色亮 / 浅色深）。
            const bool dark = ThemeManager::instance().isDark();
            m_flashValue->setStyleSheet(
                QStringLiteral("background: transparent; color: %1; font-size: 17px; "
                               "font-weight: 700;")
                    .arg(latest.success
                             ? (dark ? QStringLiteral("#3ddc97") : QStringLiteral("#138451"))
                             : (dark ? QStringLiteral("#ff7b72") : QStringLiteral("#c0392b"))));
        }
        if (m_flashSub) {
            const QString device = latest.device.isEmpty() ? QString() : latest.device;
            m_flashSub->setText(device.isEmpty()
                                    ? relativeTime(latest.time)
                                    : QStringLiteral("%1 · %2")
                                          .arg(relativeTime(latest.time), device));
        }
    }

    // ③ 时钟：立刻刷一次，不等下一个整秒。
    const QDateTime now = QDateTime::currentDateTime();
    if (m_clockValue) m_clockValue->setText(now.toString(QStringLiteral("HH:mm")));
    if (m_clockSub) {
        m_clockSub->setText(QStringLiteral("%1 · 星期%2")
                                .arg(now.toString(QStringLiteral("M 月 d 日")),
                                     weekdayName(now.date().dayOfWeek())));
    }
}

} // namespace kc::ui
