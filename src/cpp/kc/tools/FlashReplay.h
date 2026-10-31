#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  FlashReplay —— 一键「恢复上次现场」（2026-10-31）
//
//  刷机历史里躺着一堆记录，但用户真正想干的是「刚才那套再来一遍」——尤其是
//  同一台机器反复试包的时候。这个模块把一条 FlashRecord 翻译成一份**可确认的
//  重放计划**，UI 拿 describeReplay() 弹个确认框，用户点「开始」再交给现有的
//  刷机入口去执行。
//
//  为什么只做「计划」不做「执行」：
//    · 执行要碰设备、碰 worker、碰 Qt 事件循环，一旦混进来这个文件就没法脱离
//      UI 单测了。这里只回答三个问题：这条记录能不能重放、要拿什么参数去重放、
//      有什么风险要提醒用户。
//    · 重放是**危险动作**（会覆盖设备上的数据），必须由用户在确认框里看过
//      describeReplay() 的文案后显式点确认。计划层不做任何隐式决定。
//
//  🔴 三条硬规则（都是「宁可拒绝，也不能刷错」）：
//    ① 只有「写入 / XML 刷机 / 擦除 / 回读」可重放。GPT 写入 / 整盘擦除 /
//       恢复出厂 / 基带备份这些要么是「改分区表」要么是「清整机」，误触代价
//       太大，重放入口一律不提供（用户想做可以走原页面，那里有完整确认）。
//    ② 设备序列号与当前设备不一致 → 直接拒绝。**绝不静默换设备刷**：批量
//       场景下 COM 口会被重新枚举，上一次的 COM7 这次可能已经是另一台机器，
//       按端口号匹配等于把 A 机的包刷进 B 机。
//    ③ 来源包不存在 → 拒绝。路径变了 / U 盘拔了 / 临时目录被清理都会让「重放」
//       变成「刷一个不存在的文件」，与其在执行层报错不如在这里说清楚。
//
//  本文件只依赖 QtCore（QFileInfo 判存在性），可以脱离 UI 直接单测。
// ─────────────────────────────────────────────────────────────────────────────

#include "kc/diag/FlashHistory.h"

#include <QString>
#include <QStringList>

namespace kc::tools {

/// 重放计划。ok=false 时 reason 一定非空（直接拿去当提示框正文）。
struct ReplayPlan {
    bool ok = false;
    QString reason;         ///< 不可重放的原因；ok=true 时为空
    QString operation;      ///< 操作类型，原样来自记录（"写入" / "XML 刷机" / …）
    QString packagePath;    ///< 来源包路径；无包的操作为空
    QStringList partitions; ///< 从分区摘要解析出的分区名；解析不出时为空
    QStringList notes;      ///< 给人看的提醒（设备一致 / 包已校验 / 无分区信息 …）
};

/// 该操作类型是否允许重放。纯判断，不看设备也不看磁盘。
bool isReplayableOperation(const QString& operation);

/// 从 FlashRecord 构建重放计划。
/// currentDevice 传「当前连接设备的摘要」（格式与 FlashRecord::device 一致，
/// 即 MainWindow 的 m_deviceSummary："COM7 · 机型 · 序列号"）。
/// 当前没有设备时传空串 → 视为不匹配并拒绝（重放必须对着具体设备）。
ReplayPlan buildReplayPlan(const kc::diag::FlashRecord& record, const QString& currentDevice);

/// 从设备摘要里取出**身份部分**（序列号 / hwid），用于跨次比较。
/// 摘要形如 "COM7 · 小米 23127PN0CC · 0x1234abcd"，最后一段是 identity。
/// 为什么不能整串比较：COM 口号会随插拔变化（COM7 → COM9），端口一致不代表
/// 设备一致；反之端口变了也不代表换了机器。所以比对必须落在 identity 上。
QString deviceIdentity(const QString& deviceSummary);

/// 两份设备摘要是否指向同一台机器（按 deviceIdentity 比）。
/// 任一方拿不到 identity 时返回 false —— 无法确认就当不一致，宁可拒绝。
bool isSameDevice(const QString& summaryA, const QString& summaryB);

/// 从分区摘要里解析出分区名（记录里存的是 "boot, system, vendor 等 3 个" 这种
/// 给人看的字符串，不是结构化数据 —— 这里尽量还原，还原不了就返回空）。
/// totalCount 非空时写入「共几个」（解析不出填 -1）；只列出前几个时它是总数。
QStringList parsePartitionSummary(const QString& summary, int* totalCount = nullptr);

/// 给人看的多行文案（确认弹窗正文）。ok=false 时只有原因与建议。
QString describeReplay(const ReplayPlan& plan);

} // namespace kc::tools
