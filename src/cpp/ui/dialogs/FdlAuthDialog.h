#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FdlAuthDialog —— FDL Auth 弹窗（blob 只读 + sig 手工粘贴）
//
//  停更离线版（2.0.6）重建说明：
//    原版含「平台账号 + 一键计算签名」（FdlApiClient 走服务端 tool_calc），
//    那部分早在 2026-10-17 已按用户要求删除；2026-10-24 服务器关停后，
//    本弹窗只剩**纯本地**的一条路：把设备读到的 blob 显示出来供复制，
//    用户把别处算好的 sig 粘进来，点「写入」继续。
//
//  🔴 为什么这个弹窗必须留着：小米 FDL 流程里 `fastboot stage` 需要一个
//     **由小米签名服务器算出的 sig**，本工具不算签名（也不该算）—— 没有这个
//     粘贴框，整条 FDL 流程就走不下去。它是本地功能，不是联网功能。
//
//  用法（见 XiaomiEdlPage::requestSig）：
//      FdlAuthDialog dlg(blob, parent);
//      if (dlg.exec() == QDialog::Accepted) { ... dlg.sigText() ... }
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/dialogs/AnimatedDialog.h"

#include <QString>

class QLabel;
class QPlainTextEdit;
class QPushButton;

namespace kc::ui {

class FdlAuthDialog : public AnimatedDialog
{
    Q_OBJECT

public:
    explicit FdlAuthDialog(const QString& blob, QWidget* parent = nullptr);

    /// 用户粘贴的 sig 原文（已通过 kc::auth::tryParseBytes 校验）。
    QString sigText() const;

private:
    void buildUi();
    void updateHint();

    QString m_blob;
    QPlainTextEdit* m_blobEdit = nullptr;
    QPlainTextEdit* m_sigEdit = nullptr;
    QLabel* m_hint = nullptr;
    QPushButton* m_okButton = nullptr;
};

} // namespace kc::ui
