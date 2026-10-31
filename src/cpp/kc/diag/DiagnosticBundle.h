#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  DiagnosticBundle —— 「一键导出诊断包」（2026-10-29）
//
//  停更离线版删掉了「上传日志给开发者」（服务器没了，没有接收端），但
//  `kc/diag/SessionLog` 仍然每天落盘、`LogCollector` 仍然能把最近几天的日志
//  收成一个数组 —— 只是**没有任何 UI 入口**把它们变成「用户能发出去的东西」。
//  用户报障时还是得自己去 %LOCALAPPDATA%\KunClientState\logs 里挑文件、再手动
//  压缩，十有八九漏掉环境信息。
//
//  这一层就是补那个缺口：把最近几天的会话日志 + 一份 system-info.txt 打成一个
//  zip，用户点一下就能把整包发给开发者。
//
//  为什么自己打 zip：项目不引 zlib / QuaZip（单文件产物体积是硬指标），诊断包
//  只有几 MB 纯文本，用 `kc::tools::ZipWriter` 的「只存储不压缩」完全够用。
// ─────────────────────────────────────────────────────────────────────────────

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace kc::diag {

/// 诊断包构建结果。失败时 path 为空、error 是可直接展示给用户的中文原因。
struct BundleResult
{
    bool ok = false;
    QString path;
    QString error;
    int logFileCount = 0;
    qint64 totalBytes = 0;
};

/// 默认文件名：KunClient-诊断-YYYYMMDD-HHmmss.zip（不含目录）。
QString suggestedBundleName(const QDateTime& now);

/// 把诊断包写到 targetZipPath。
///   · logs/            最近 5 个会话日志（保新丢旧，累计 ≤ 2 MiB，与 LogCollector 同口径）
///   · system-info.txt  版本 / 系统 / 运行环境 / 日志目录（纯文本，键值对）
/// 目录不存在时自动创建；失败用 QSaveFile 的原子性保证不留半成品。
BundleResult writeDiagnosticBundle(const QString& targetZipPath,
                                   const QString& appVersion,
                                   const QJsonObject& extraMeta = {});

/// 仅 system-info.txt 的正文（纯函数，单测直接断言，不碰磁盘）。
QString buildSystemInfoText(const QString& appVersion, const QJsonObject& extraMeta = {},
                            const QString& logDirectory = QString());

} // namespace kc::diag
