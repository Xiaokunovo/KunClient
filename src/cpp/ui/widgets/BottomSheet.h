#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  BottomSheet —— iOS 风格「底部上滑抽屉」
//
//  和 AnimatedDialog（居中淡入）不是一类东西：
//    · 全屏暗化遮罩（scrim）从 0 渐到 ~50%；
//    · 玻璃面板贴底，从屏幕下方滑上来，顶部一个抓取把手；
//    · 点遮罩 / Esc 收起，退场反向滑下去。
//
//  适合「次级操作 / 确认」——比居中弹窗轻，比 Toast 重（需要用户决策时用）。
//
//  用法：
//      BottomSheet::confirm("擦除分区", "确定要擦除 system 吗？",
//                           "擦除", [=]{ doErase(); });
// ─────────────────────────────────────────────────────────────────────────────

#include <QDialog>
#include <QString>
#include <QVector>

#include <functional>

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

namespace kc::ui {

class GlassSurface;

class BottomSheet : public QDialog {
    Q_OBJECT
    Q_PROPERTY(qreal scrimAlpha READ scrimAlpha WRITE setScrimAlpha)

public:
    explicit BottomSheet(QWidget* parent = nullptr);
    ~BottomSheet() override;

    /// 设置内容（接管所有权）。height <= 0 时按内容 sizeHint 自动定高。
    void setContent(QWidget* content, int height = 0);

    /// 滑入显示。
    void showSheet();
    /// 滑出收起（收起后自毁）。
    void closeSheet();

    qreal scrimAlpha() const { return m_scrimAlpha; }
    void setScrimAlpha(qreal alpha);

    /// 便捷确认框：标题 + 正文 + 取消/确认。确认后执行 onConfirm。
    static void confirm(const QString& title, const QString& message,
                        const QString& confirmText, std::function<void()> onConfirm,
                        const QString& cancelText = QStringLiteral("取消"));

    /// 阻塞式确认（内部跑嵌套事件循环），返回用户是否点了确认。
    /// 用来 1:1 替换 ThemedMessageBox::question 这类同步确认。
    static bool confirmSync(const QString& title, const QString& message,
                            const QString& confirmText,
                            const QString& cancelText = QStringLiteral("取消"));

    /// 面板最大宽度（0 = 不限制，铺满）。iOS 动作面板通常限制在 ~520px 居中。
    void setMaxPanelWidth(int px);

    /// 动作面板里的一项。
    struct Action {
        QString text;
        bool destructive = false;      ///< 危险动作（红字）
        std::function<void()> onPick;
    };

    /// iOS 动作面板：标题 + 一列动作 + 独立的取消块。
    static void showActions(const QString& title, const QVector<Action>& actions,
                            const QString& cancelText = QStringLiteral("取消"));

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void relayout();
    void refreshPanelBackdrop();
    void finishClose();

    GlassSurface* m_panel = nullptr;
    QWidget* m_contentHost = nullptr;
    QWidget* m_handle = nullptr;
    QWidget* m_content = nullptr;
    int m_wantedHeight = 0;
    int m_maxPanelW = 0;      ///< 0 = 铺满；>0 = 居中且不超过该宽度
    qreal m_scrimAlpha = 0.0;
    bool m_closing = false;
};

} // namespace kc::ui
