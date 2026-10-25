#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  ComplianceConfirmDialog —— 高风险操作的合规确认（移植自 ComplianceConfirmDialog.xaml.cs）。
//
//  涉及设备防盗机制（如 FRP 出厂重置保护）的操作不能一点就跑，必须先让用户显式
//  声明设备归属。两个确认项（设备归属 + 已知晓可能构成刑事犯罪）都勾上才放行。
// ─────────────────────────────────────────────────────────────────────────────

#include <QDialog>
#include "ui/dialogs/AnimatedDialog.h"
#include <QString>

class QCheckBox;
class QPushButton;

namespace kc::ui {

class ComplianceConfirmDialog : public AnimatedDialog {
    Q_OBJECT

public:
    ComplianceConfirmDialog(const QString& actionName, const QString& riskText,
                            QWidget* parent = nullptr);

    /// 便捷入口：exec + 返回是否点了「确认执行」。
    static bool confirm(QWidget* parent, const QString& actionName, const QString& riskText);

private:
    void updateOkEnabled();

    QCheckBox* m_chkOwn = nullptr;     // 设备归属声明
    QCheckBox* m_chkLegal = nullptr;   // 已知晓可能构成刑事犯罪（2026-09-24 合规加强）
    QPushButton* m_ok = nullptr;
};

} // namespace kc::ui
