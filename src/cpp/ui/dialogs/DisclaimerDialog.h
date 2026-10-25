#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  DisclaimerDialog —— 使用条款 + 隐私说明的确认窗口（移植自 DisclaimerDialog.xaml.cs）。
//
//  两种用法（与 C# 一致）：
//    · enforce = true（首次启动）：点「不同意」或直接关窗 → reject → 调用方退出程序。
//    · enforce = false（设置页重新查看）：只做展示；隐藏「不同意」，按钮变「我已阅读」。
//
//  LegalVersion 与 C# DisclaimerDialog.LegalVersion、server.js 的 LEGAL_VERSION、
//  LoginDialog 注册上报的 legalVersion 是**同一语义的四份拷贝**，条款有实质变更四处一起 +1。
// ─────────────────────────────────────────────────────────────────────────────

#include <QDialog>
#include "ui/dialogs/AnimatedDialog.h"

class QLabel;

namespace kc::ui {

class DisclaimerDialog : public AnimatedDialog {
    Q_OBJECT

public:
    /// 当前条款文本版本。改动免责/协议/隐私内容就要 +1。
    static constexpr int LegalVersion = 6;

    explicit DisclaimerDialog(bool enforce = true, QWidget* parent = nullptr);

    /// 是否点了「同意」/「我已阅读」。
    bool isAgreed() const { return m_agreed; }

private:
    bool m_enforce = true;
    bool m_agreed = false;
};

} // namespace kc::ui
