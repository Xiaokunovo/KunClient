#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  GlassSettings —— 液态玻璃预设表 + AppSettings ⇄ glass::Style 互转
//
//  为什么单独一层：glass::Style 是 UI 层的绘制参数，AppSettings 是 core 层的
//  持久化模型，两边不该互相 include。所有「预设 id → 具体数值」「配置字段 →
//  Style」的映射都收在这里，设置页、预览卡、灵动岛、将来的其它玻璃面板共用
//  同一份口径 —— 否则改一处忘一处，用户看到的就是「设置里调了、别处没变」。
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/widgets/LiquidGlass.h"

#include <QString>
#include <QVector>

namespace kc::ui {

/// 一档预设。
struct GlassPreset {
    QString id;      ///< 持久化用的键（crystal / standard / frosted / minimal）
    QString name;    ///< 下拉框显示的中文名
    QString hint;    ///< 一句话说明（显示在预设下方，帮用户选）
    glass::Style style;
};

/// 全部预设（顺序 = 下拉框顺序）。不含 custom —— custom 是「用户手调过」的状态。
const QVector<GlassPreset>& glassPresets();

/// 按 id 找预设；找不到返回 nullptr。
const GlassPreset* findGlassPreset(const QString& id);

/// 从 AppSettings 读出当前 Style。参数越界会自动钳到合法区间。
glass::Style currentGlassStyle();

/// 把 Style 写回 AppSettings（**只改内存，不落盘**）。
/// @param presetId 传 "custom" 表示这是手调值；传预设 id 则同时记录该预设。
void storeGlassStyle(const glass::Style& style, const QString& presetId);

/// 应用一档预设：把预设数值写进 AppSettings（内存）。返回该预设。
const GlassPreset& applyGlassPreset(const QString& id);

/// 当前 Style 与哪一档预设完全一致（用于下拉框回显）。都不一致返回 "custom"。
QString matchGlassPreset(const glass::Style& style);

} // namespace kc::ui
