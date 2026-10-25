#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  EraseConfirmDialog —— 擦除分区前的二次确认弹窗。
//
//  为什么不用 ThemedMessageBox：分区一多（截图 69 个）把列表直接拼进 QLabel
//  会撑出几屏高的巨长弹窗。这里把分区名单放进固定高度的滚动列表，弹窗尺寸可控。
//
//  2026-10-24 迁移到 GlassDialog（液态玻璃底 + 自建布局）：原来的 `#msg*`
//  实色卡片皮肤不再参与，改由玻璃基类提供材质。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/dialogs/GlassDialog.h"

#include <QStringList>

class QLabel;
class QListWidget;
class QPushButton;

namespace kc::ui {

class EraseConfirmDialog : public GlassDialog {
    Q_OBJECT

public:
    /// summary 示例："即将擦除 69 个分区，共 1118.90 GB。"
    EraseConfirmDialog(QWidget* parent, const QString& summary,
                       const QStringList& items);

    bool confirmed() const { return m_confirmed; }

private:
    bool m_confirmed = false;
};

} // namespace kc::ui
