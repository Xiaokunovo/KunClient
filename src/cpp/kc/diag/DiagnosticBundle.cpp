#include "kc/diag/DiagnosticBundle.h"

#include "kc/config/AppStatePaths.h"
#include "kc/diag/LogCollector.h"
#include "kc/diag/SessionLog.h"
#include "kc/tools/ZipWriter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSysInfo>
#include <QThread>
#include <QVector>

namespace kc::diag {
namespace {

// 与 LogCollector 的默认值同口径：诊断包不追求「全量」，追求「够定位问题且能发得出去」。
constexpr int kMaxLogFiles = 5;
constexpr qint64 kMaxLogBytes = 2 * 1024 * 1024;

QString safeValue(const QString& text)
{
    return text.isEmpty() ? QStringLiteral("—") : text;
}

} // namespace

QString suggestedBundleName(const QDateTime& now)
{
    return QStringLiteral("KunClient-诊断-%1.zip")
        .arg(now.toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

QString buildSystemInfoText(const QString& appVersion, const QJsonObject& extraMeta,
                            const QString& logDirectory)
{
    // 纯文本键值对，不用 JSON：用户/开发者直接双击打开就能读，不需要任何工具。
    QStringList lines;
    lines << QStringLiteral("KunClient 诊断包");
    lines << QStringLiteral("================");
    lines << QString();
    lines << QStringLiteral("[版本]");
    lines << QStringLiteral("客户端版本: %1").arg(safeValue(appVersion));
    lines << QStringLiteral("Qt: %1").arg(QString::fromLatin1(qVersion()));
    lines << QStringLiteral("程序目录: %1")
                 .arg(safeValue(QCoreApplication::applicationDirPath()));
    lines << QString();
    lines << QStringLiteral("[系统]");
    lines << QStringLiteral("系统: %1").arg(QSysInfo::prettyProductName());
    lines << QStringLiteral("内核: %1 %2")
                 .arg(QSysInfo::kernelType(), QSysInfo::kernelVersion());
    lines << QStringLiteral("CPU 架构: %1").arg(QSysInfo::currentCpuArchitecture());
    lines << QStringLiteral("字节序: %1")
                 .arg(QSysInfo::ByteOrder == QSysInfo::BigEndian
                          ? QStringLiteral("大端") : QStringLiteral("小端"));
    lines << QStringLiteral("进程位数: %1")
                 .arg(sizeof(void*) == 8 ? QStringLiteral("64 位")
                                         : QStringLiteral("32 位"));
    lines << QString();
    lines << QStringLiteral("[运行环境]");
    lines << QStringLiteral("日志目录: %1")
                 .arg(safeValue(logDirectory.isEmpty() ? SessionLog::logDir()
                                                       : logDirectory));
    lines << QStringLiteral("状态目录: %1").arg(AppStatePaths::Root());
    lines << QStringLiteral("导出时间: %1")
                 .arg(QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyy-MM-dd HH:mm:ss")));

    // extraMeta 里的键值按字典序追加：调用方（设置页）塞的是「当前主题 / 存储类型 /
    // 最近一次连接设备」这类现场信息，字段集合以后还会加，所以不做固定格式。
    if (!extraMeta.isEmpty()) {
        lines << QString();
        lines << QStringLiteral("[现场信息]");
        QStringList keys = extraMeta.keys();
        keys.sort();
        for (const QString& key : std::as_const(keys)) {
            const QJsonValue value = extraMeta.value(key);
            QString text;
            if (value.isString()) text = value.toString();
            else if (value.isBool()) text = value.toBool() ? QStringLiteral("是")
                                                           : QStringLiteral("否");
            else if (value.isDouble()) text = QString::number(value.toDouble());
            else if (!value.isNull() && !value.isUndefined()) text = QStringLiteral("(复杂值)");
            lines << QStringLiteral("%1: %2").arg(key, safeValue(text));
        }
    }

    lines << QString();
    lines << QStringLiteral("说明：本包含最近几天的会话日志（logs/ 子目录）与本文件。");
    lines << QStringLiteral("日志只记录本机操作过程，不含账号凭据。");
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

BundleResult writeDiagnosticBundle(const QString& targetZipPath,
                                   const QString& appVersion,
                                   const QJsonObject& extraMeta)
{
    BundleResult result;
    if (targetZipPath.trimmed().isEmpty()) {
        result.error = QStringLiteral("未指定保存路径");
        return result;
    }

    const QFileInfo info(targetZipPath);
    QDir parent = info.absoluteDir();
    if (!parent.exists() && !QDir().mkpath(parent.absolutePath())) {
        result.error = QStringLiteral("创建目录失败：%1").arg(parent.absolutePath());
        return result;
    }

    const QVector<LogFileItem> logs =
        collectRecentLogs(QString(), kMaxLogFiles, kMaxLogBytes);

    kc::tools::ZipWriter writer;
    // 日志放 logs/ 子目录：解压后一眼能分清「这是日志、那是说明」，也不会把
    // 一堆 session-*.log 撒在根目录。
    for (const LogFileItem& item : logs) {
        writer.addFile(QStringLiteral("logs/%1").arg(item.name), item.content);
    }
    writer.addFile(QStringLiteral("system-info.txt"),
                   buildSystemInfoText(appVersion, extraMeta).toUtf8());

    QString error;
    if (!writer.writeTo(info.absoluteFilePath(), &error)) {
        result.error = error.isEmpty() ? QStringLiteral("写入诊断包失败") : error;
        return result;
    }

    result.ok = true;
    result.path = info.absoluteFilePath();
    result.logFileCount = logs.size();
    result.totalBytes = static_cast<qint64>(writer.totalBytes());
    return result;
}

} // namespace kc::diag
