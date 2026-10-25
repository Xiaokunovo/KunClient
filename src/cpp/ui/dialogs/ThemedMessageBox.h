#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ThemedMessageBox —— 全项目统一的消息 / 确认 / 报错弹窗（kc::ui）。
//
//  为什么替换 QMessageBox：原生 QMessageBox 是系统风格，深浅两主题下都和
//  本项目长得不像一家人，而且**没有任何动画**——啪地出现、啪地消失。
//  本类用 AnimatedDialog 做退场动画，用 QSS #msg* 段做双主题皮肤，
//  静态接口与 QMessageBox 一一对应，所以老调用点只需要把类名换掉：
//
//      QMessageBox::warning(this, "标题", text)      →  ThemedMessageBox::warning(...)
//      QMessageBox::question(...) != QMessageBox::Yes →  ThemedMessageBox::question(...) != QMessageBox::Yes
//
//  返回值仍然是 QMessageBox::StandardButton，比较代码一个字都不用改。
//
//  四个严重程度（variant 属性驱动配色，代码里不写死颜色）：
//    Info / Warning / Critical / Question —— 徽标分别是 i / ! / ! / ?
//   （刻意不用 emoji：客户端要过「无 emoji」检查，且矢量字形在两种主题下都清晰）
//
//  🔴 无边框 + WA_TranslucentBackground 的组合下，**QSS 不会画在顶层 widget 上**
//     （MainShell/LoginDialog 都踩过），所以卡片背景由内层 QFrame#msgRoot 提供。
//  🔴 长文本：默认不出现滚动条（用户明确要求「不要滑动模块」），只有当文案高度
//     超过屏幕可用高度的 62% 时才退化成滚动，宁可比屏幕矮一点也不常驻滚动条。
// ─────────────────────────────────────────────────────────────────────────────

#include <QDialog>
#include <QMessageBox>   // 只为复用 StandardButton / StandardButtons 枚举

#include "ui/dialogs/AnimatedDialog.h"

class QLabel;
class QPushButton;

namespace kc::ui {

class ThemedMessageBox : public AnimatedDialog {
    Q_OBJECT

public:
    enum class Severity { Info, Warning, Critical, Question };

    explicit ThemedMessageBox(Severity severity, const QString& title, const QString& text,
                              QMessageBox::StandardButtons buttons = QMessageBox::Ok,
                              QMessageBox::StandardButton defaultButton = QMessageBox::NoButton,
                              QWidget* parent = nullptr);

    /// 自定义按钮版（封禁提示的「去申诉 / 退出」就靠它）；返回按钮下标，关窗返回 -1。
    static int custom(QWidget* parent, const QString& title, const QString& text,
                      const QStringList& buttonTexts, int defaultIndex = 0,
                      Severity severity = Severity::Info);

    // ── 与 QMessageBox 同形的四个静态入口 ─────────────────────────────────
    static QMessageBox::StandardButton information(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

    static QMessageBox::StandardButton warning(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

    static QMessageBox::StandardButton critical(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

    static QMessageBox::StandardButton question(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

    /// 封禁 / 强制下线专用：主按钮做「去申诉」，次按钮退出或关闭。
    /// 返回 true = 用户点了申诉按钮（调用方接着弹 AppealDialog）。
    static bool noticeWithAction(QWidget* parent, const QString& title, const QString& text,
                                 const QString& actionText, const QString& dismissText,
                                 Severity severity = Severity::Critical);

private:
    /// 自定义按钮版的私有构造（custom() 用；公开入口只给标准按钮那套）。
    ThemedMessageBox(Severity severity, const QString& title, const QString& text,
                     const QStringList& buttonTexts, int defaultIndex, QWidget* parent);

    void buildUi(Severity severity, const QString& title, const QString& text,
                 const QVector<QPair<QMessageBox::StandardButton, QString>>& buttons,
                 QMessageBox::StandardButton defaultButton);
    void buildCustomUi(const QString& title, const QString& text,
                       const QStringList& buttonTexts, int defaultIndex, Severity severity);

    QPushButton* m_actionButton = nullptr;
    QPushButton* m_dismissButton = nullptr;
};

} // namespace kc::ui
