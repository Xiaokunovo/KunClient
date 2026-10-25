#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  LogCollector — 日志采集与上报载荷构建（2026-10-12）
//
//  与 LogUploadService 分开的原因：这一层是**纯函数**（不摸网络、不摸时钟、
//  不依赖登录态），可以进 core 单测；上传编排（HTTP/鉴权/重试语义）在上一层。
//
//  采集口径与客户端同意弹窗里写明的**逐字对应**：
//    · 最近几天的会话日志文件（按日切割，见 SessionLog）；
//    · 版本 / 系统环境信息（meta，由上传方填好传进来）；
//    · 账号名（在 meta.username 里，已登录才有）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace kc::diag {

struct LogFileItem {
    QString name;
    QByteArray content;
};

/// 收集最近日志：按文件名（= 日期）升序，从新往旧装，最多 maxFiles 个、
/// 累计不超过 maxTotalBytes（超预算就保新丢旧）。dir 为空用 SessionLog::logDir()。
QVector<LogFileItem> collectRecentLogs(const QString& dir = QString(),
                                       int maxFiles = 5,
                                       qint64 maxTotalBytes = 2 * 1024 * 1024);

/// 构建上报 JSON（纯函数，供单测钉结构）。
/// files 内容按 base64 编进 content 字段；note 截到 500 字。
QJsonObject buildPayload(const QVector<LogFileItem>& files, const QString& build,
                         const QJsonObject& meta, const QString& note);

} // namespace kc::diag
