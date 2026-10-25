#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  IosGroupedList —— iOS「内嵌分组列表」
//
//  一张圆角卡片，里面若干行；行之间有 16px 内缩的分隔线；右侧可选值 / 箭头 /
//  开关。设置页那种「一组一组」的观感就是它。
//
//      auto* g = new IosGroupedList(host);
//      g->setHeader(QStringLiteral("通用"));
//      g->addRow(QStringLiteral("版本"), QString(), QStringLiteral("1.0.0"));
//      g->addRow(QStringLiteral("检查更新"))->onActivated = [] { ... };
//      g->addToggleRow(QStringLiteral("自动更新"), true);
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QVBoxLayout;

namespace kc::ui {

class IosSwitch;

class IosGroupedList : public QWidget {
    Q_OBJECT

public:
    /// 一行。点整行触发 onActivated（可选）。
    class Row : public QWidget {
    public:
        Row(const QString& title, const QString& subtitle, bool chevron, QWidget* parent);

        void setValue(const QString& value);       ///< 右侧灰字
        void setDetail(const QString& subtitle);   ///< 第二行小字
        void setRowEnabled(bool on);
        void setFirst(bool first);
        /// 在右侧放任意控件（下拉框 / 分段器 / 滑杆容器等）。
        void setControl(QWidget* control);
        /// 整行可点时才显示手型光标（纯信息行保持箭头）。
        void setClickable(bool on)
        {
            setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }

        std::function<void()> onActivated;

    protected:
        void paintEvent(QPaintEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void enterEvent(QEnterEvent* event) override;
        void leaveEvent(QEvent* event) override;

    private:
        void relayout();

        QLabel* m_title = nullptr;
        QLabel* m_detail = nullptr;
        QLabel* m_value = nullptr;
        bool m_chevron = false;
        bool m_hover = false;
        bool m_first = false;
    };

    explicit IosGroupedList(QWidget* parent = nullptr);

    void setHeader(const QString& text);
    void setFooter(const QString& text);

    Row* addRow(const QString& title, const QString& subtitle = QString(),
                const QString& value = QString(), bool chevron = false);
    /// 标题在左、任意控件在右的一行（下拉框 / 分段器 / 滑杆容器…）。
    Row* addCustomRow(const QString& title, QWidget* control,
                      const QString& subtitle = QString());
    /// 返回开关指针（需要读值时用）。
    IosSwitch* addToggleRow(const QString& title, bool on, Row** rowOut = nullptr);
    void addSeparator();
    void clearRows();

private:
    QWidget* m_group = nullptr;      ///< 画圆角底的容器
    QVBoxLayout* m_groupLayout = nullptr;
    QLabel* m_header = nullptr;
    QLabel* m_footer = nullptr;
    QVector<QWidget*> m_rows;
};

} // namespace kc::ui
