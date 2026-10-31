#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FlashHistoryDialog —— 「刷机记录」查看 / 导出窗
//
//  为什么需要它：`kc::diag::FlashHistory` 每次刷写/擦除/读回都会落一条记录到
//  %LOCALAPPDATA%\KunClientState\flash-history.jsonl，但那些记录如果只能靠记事本
//  打开，等于没有 —— 用户想知道「上周刷过哪台机器、哪个包、成功没有」时，
//  需要一个能按列看、能一键导出给售后/群友的界面。
//
//  设计取舍：
//  · 基类用 GlassDialog（与擦除确认窗同族），不再新造一套皮肤；
//  · 表格列只放**能一眼判断成败**的字段（时间/操作/设备/来源包/分区/结果/耗时），
//    完整原始消息塞进每行的 tooltip，不占列宽；
//  · 导出两种格式：CSV 给 Excel，TXT 给聊天窗直接粘贴（等宽对齐）。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/dialogs/GlassDialog.h"

class QLabel;
class QTableWidget;

namespace kc::ui {

class FlashHistoryDialog : public GlassDialog
{
    Q_OBJECT

public:
    explicit FlashHistoryDialog(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    /// 重新从 FlashHistory 读一遍并重填表格（打开时 / 清空后都走这里）。
    void reload();

    QTableWidget* m_table = nullptr;
    QLabel* m_summary = nullptr;
    QLabel* m_empty = nullptr;
};

} // namespace kc::ui
