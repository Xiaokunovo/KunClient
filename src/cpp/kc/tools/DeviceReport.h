#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  DeviceReport —— 设备信息导出（给维修现场 / 求助用）
//
//  场景：机器刷坏了要在群里问人，或者发给售后。现在得一项项抄：机型、序列号、
//  分区表、GPT 状态……抄漏一个对方就得反问一轮。
//
//  这里做两个出口：给人看的等宽文本（直接粘聊天窗）+ 结构化 JSON（给脚本 / 归档）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QMap>
#include <QPair>
#include <QString>
#include <QVector>

namespace kc::tools {

/// 生成报告需要的输入。
struct DeviceReportInput {
    QString deviceSummary;      ///< "COM7 · 机型 · 序列号" 这种摘要
    QString generatedAt;        ///< ISO 时间；空 = 取当前时间
    QString appVersion;         ///< 客户端版本

    /// 分区表（名字 + 字节数）。空 = 不输出分区段。
    QVector<QPair<QString, qint64>> partitions;

    /// 附加键值（协议 / 存储类型 / GPT 状态 / 电量 ……），按插入顺序输出。
    QVector<QPair<QString, QString>> extraInfo;
};

/// 等宽对齐的纯文本（可直接粘进聊天窗）。
QString buildTextReport(const DeviceReportInput& input);

/// 结构化 JSON（缩进格式，UTF-8）。
QByteArray buildJsonReport(const DeviceReportInput& input);

/// 建议的文件名：KunClient-设备报告-<时间戳>.txt
QString suggestedReportFileName(const QString& suffix);

} // namespace kc::tools
