#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ToolProgressBar — 通栏进度条 + 居中百分比 + 速率读数
//
//  移植自 tests/src/KunClient.App/Controls/ToolProgressBar.{xaml,xaml.cs}，
//  按任务要求扩展为「分段 + 速率读数」形态：
//   · 语义：value() 是 0-100 的百分比，0 时百分比压浅灰轨道 → 次级色；
//     填充过半 → 压蓝底 → 白字（写死白字会让 0% 的百分比整段隐形）；
//   · setRate() 在条右侧显示速率读数（如 "12.3 MB/s"）；
//   · setSegments() 把条分成 N 段（每段一段填充带小间隙），用于多分区刷写进度。
//  自绘（不依赖 QProgressBar 模板），因为要画分段与叠加文字。
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>

namespace kc::ui {

class ToolProgressBar : public QWidget {
    Q_OBJECT
    Q_PROPERTY(double value READ value WRITE setValue)
    Q_PROPERTY(QString text READ text WRITE setText)

public:
    explicit ToolProgressBar(QWidget* parent = nullptr);

    /// 进度百分比（0-100）。
    double value() const { return m_value; }
    void setValue(double value);

    /// 显示文本；留空则按 format() 自动生成（默认两位小数，与参考稿 100.00% 一致）。
    QString text() const { return m_text; }
    void setText(const QString& text);

    /// 自动文本的数字格式（默认 "0.00" → 100.00%）。
    QString format() const { return m_format; }
    void setFormat(const QString& format);

    bool showText() const { return m_showText; }
    void setShowText(bool show);

    /// 速率读数（如 "12.3 MB/s"）；空则右侧不占位。
    QString rate() const { return m_rate; }
    void setRate(const QString& rate);

    /// 分段数（>=1）。1 = 通栏单条；>1 = 等分若干段，段间留 2px 间隙。
    int segments() const { return m_segments; }
    void setSegments(int segments);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    /// 0-100 之间：判断文字该用白字还是次级色（填充是否已过半）。
    bool filledPast(double x) const;

    double m_value = 0.0;
    QString m_text;
    QString m_format = QStringLiteral("0.00");
    QString m_rate;
    bool m_showText = true;
    int m_segments = 1;
    int m_barHeight = 12;
};

} // namespace kc::ui
