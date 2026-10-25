#include "ui/widgets/ToolProgressBar.h"

#include "ui/theme/ThemeManager.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>

#include <algorithm>

namespace kc::ui {

ToolProgressBar::ToolProgressBar(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("toolProgressBar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // 主题切换后重算文字颜色（否则深色主题下留着浅色主题算出来的那一支）。
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            [this](bool) { update(); });
}

void ToolProgressBar::setValue(double value) {
    const double clamped = std::clamp(value, 0.0, 100.0);
    if (qFuzzyCompare(clamped + 1.0, m_value + 1.0)) return;
    m_value = clamped;
    update();
}

void ToolProgressBar::setText(const QString& text) {
    if (m_text == text) return;
    m_text = text;
    update();
}

void ToolProgressBar::setFormat(const QString& format) {
    if (m_format == format) return;
    m_format = format;
    update();
}

void ToolProgressBar::setShowText(bool show) {
    if (m_showText == show) return;
    m_showText = show;
    update();
}

void ToolProgressBar::setRate(const QString& rate) {
    if (m_rate == rate) return;
    m_rate = rate;
    update();
}

void ToolProgressBar::setSegments(int segments) {
    const int s = std::max(1, segments);
    if (m_segments == s) return;
    m_segments = s;
    update();
}

QSize ToolProgressBar::sizeHint() const {
    return QSize(320, m_barHeight);
}

QSize ToolProgressBar::minimumSizeHint() const {
    return QSize(80, m_barHeight);
}

bool ToolProgressBar::filledPast(double x) const { return x >= 50.0; }

void ToolProgressBar::paintEvent(QPaintEvent* /*event*/) {
    const ThemeManager& tm = ThemeManager::instance();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int gap = 2;
    const int totalW = width();
    const int barTop = (height() - m_barHeight) / 2;
    // 圆角夹到条高的一半：档位升到 8px 后，12px 高的条用 8px 圆角会超出半高，
    // addRoundedRect 不钳制，画出来是畸角。进度条要的是药丸不是畸角。
    const int radius = qMin(tm.radius(RadiusToken::Small), m_barHeight / 2);

    // 段宽（含间隙的等分）。
    const int segW = std::max(1, (totalW - gap * (m_segments - 1)) / m_segments);

    const QColor track = tm.color(ColorToken::ProgTrack);
    const QColor fill = tm.color(ColorToken::Accent);

    // 每段应填充的比例（按全局百分比平摊）。
    const double overall = m_value / 100.0;

    for (int i = 0; i < m_segments; ++i) {
        const int x = i * (segW + gap);
        QRect segRect(x, barTop, segW, m_barHeight);
        if (i == m_segments - 1) segRect.setRight(totalW); // 末段吃满余量

        QPainterPath trackPath;
        trackPath.addRoundedRect(segRect, radius, radius);
        p.fillPath(trackPath, track);

        // 该段的填充宽度：整条进度平摊到各段，保证末段在 100% 时刚好填满。
        const double segStart = static_cast<double>(i) / m_segments;
        const double segEnd = static_cast<double>(i + 1) / m_segments;
        double localFill = 0.0;
        if (overall >= segEnd) {
            localFill = 1.0;
        } else if (overall > segStart) {
            localFill = (overall - segStart) / (segEnd - segStart);
        }
        if (localFill > 0.0) {
            QRect fillRect(segRect.left(), segRect.top(),
                           static_cast<int>(segRect.width() * localFill), segRect.height());
            if (fillRect.width() > 0) {
                QPainterPath fillPath;
                fillPath.addRoundedRect(fillRect, radius, radius);
                p.save();
                p.setClipPath(trackPath); // 填充不越出轨道圆角
                p.fillPath(fillPath, fill);
                p.restore();
            }
        }
    }

    const QString label = m_showText
                              ? (m_text.isEmpty() ? QString::number(m_value, 'f', 2) + "%" : m_text)
                              : QString();

    const QColor textColor = filledPast(m_value) ? tm.color(ColorToken::ProgressText)
                                                 : tm.color(ColorToken::TextMuted);

    // 中央百分比
    if (!label.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(std::max(9.0, tm.fontSize(FontToken::Body) - 0.5));
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setPen(textColor);
        p.drawText(rect(), Qt::AlignCenter, label);
    }

    // 右侧速率读数（不遮中央百分比）。
    if (!m_rate.isEmpty()) {
        QFont f = font();
        f.setPointSizeF(std::max(8.0, tm.fontSize(FontToken::Caption) - 0.5));
        p.setFont(f);
        p.setPen(tm.color(ColorToken::TextFaint));
        const QFontMetrics fm(f);
        const int w = fm.horizontalAdvance(m_rate) + 4;
        p.drawText(QRect(totalW - w, 0, w, height()), Qt::AlignRight | Qt::AlignVCenter, m_rate);
    }
}

void ToolProgressBar::changeEvent(QEvent* event) {
    if (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange) {
        update();
    }
    QWidget::changeEvent(event);
}

} // namespace kc::ui
