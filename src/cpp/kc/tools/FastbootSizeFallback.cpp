#include "kc/tools/FastbootSizeFallback.h"

#include "kc/tools/PlatformToolsManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

namespace kc {
namespace {

struct RunOutcome {
    bool started = false;
    bool finished = false;
    int exitCode = -1;
    QString output;
};

RunOutcome runOnce(const QString& binary, const QString& arguments, int timeoutMs,
                   const QString& deviceSerial, bool insertForce)
{
    RunOutcome outcome;

    QProcess process;
    process.setProgram(binary);
    process.setWorkingDirectory(QFileInfo(binary).absolutePath());
    // fastboot 把错误写在 stderr，正文在 stdout；合并后按原文匹配错误特征。
    process.setProcessChannelMode(QProcess::MergedChannels);

    QString nativeArguments;
    if (!deviceSerial.isEmpty()) nativeArguments += QStringLiteral("-s %1 ").arg(deviceSerial);
    if (insertForce) nativeArguments += QStringLiteral("--force ");
    nativeArguments += arguments;
    process.setNativeArguments(nativeArguments);

    process.start();
    if (!process.waitForStarted(10000)) {
        outcome.output = QStringLiteral("进程启动失败");
        return outcome;
    }
    outcome.started = true;

    if (!process.waitForFinished(timeoutMs > 0 ? timeoutMs : -1)) {
        process.kill();
        process.waitForFinished(3000);
        outcome.output = QStringLiteral("命令超时（%1ms）").arg(timeoutMs);
        return outcome;
    }

    outcome.finished = true;
    outcome.exitCode = process.exitCode();
    outcome.output = QString::fromLocal8Bit(process.readAll());
    return outcome;
}

QString commandLineOf(const QString& binary, const QString& arguments, const QString& deviceSerial,
                      bool insertForce)
{
    QString line = QFileInfo(binary).fileName();
    if (!deviceSerial.isEmpty()) line += QStringLiteral(" -s %1").arg(deviceSerial);
    if (insertForce) line += QStringLiteral(" --force");
    line += QLatin1Char(' ');
    line += arguments;
    return line;
}

} // namespace

bool FastbootSizeFallback::isPartitionSizeUnavailable(const QString& text)
{
    // 参考实现口径：同时含 "cannot get" 与 "partition size"（大小写不敏感）。
    if (text.isEmpty()) return false;
    return text.contains(QStringLiteral("cannot get"), Qt::CaseInsensitive)
           && text.contains(QStringLiteral("partition size"), Qt::CaseInsensitive);
}

QStringList FastbootSizeFallback::candidates()
{
    QStringList raw;
    QString toolsDir;

    {
        // 只为拿随包路径；构造函数会确保工具目录存在，不做网络动作。
        const PlatformToolsManager tools;
        toolsDir = tools.toolsDirectory();
        raw << tools.fastbootPath();
    }

    if (!toolsDir.isEmpty()) {
        raw << QDir(toolsDir).filePath(QStringLiteral("fastboot33.exe"));
        raw << QDir(toolsDir).filePath(QStringLiteral("fastboot_legacy.exe"));
        raw << QDir(toolsDir).filePath(QStringLiteral("LegacyFb/fastboot33.exe"));
    }

    if (QCoreApplication::instance()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            raw << QDir(appDir).filePath(QStringLiteral("fastboot.exe"));
            raw << QDir(appDir).filePath(QStringLiteral("LegacyFb/fastboot33.exe"));
            raw << QDir(appDir).filePath(QStringLiteral("tools/fastboot33.exe"));
        }
    }

    const QString systemRoot = QString::fromLocal8Bit(qgetenv("SystemRoot"));
    if (!systemRoot.isEmpty())
        raw << QDir(systemRoot).filePath(QStringLiteral("System32/fastboot.exe"));

    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("fastboot"));
    if (!fromPath.isEmpty()) raw << fromPath;

    // 去重 + 只保留真实存在的文件（Windows 路径大小写不敏感）。
    QStringList unique;
    QSet<QString> seen;
    for (const QString& path : raw) {
        if (path.trimmed().isEmpty()) continue;
        const QFileInfo info(path);
        if (!info.exists() || !info.isFile()) continue;
        const QString key = info.absoluteFilePath().toLower();
        if (seen.contains(key)) continue;
        seen.insert(key);
        unique << info.absoluteFilePath();
    }
    return unique;
}

FastbootSizeFallback::Result FastbootSizeFallback::execute(
    const QString& arguments, int timeoutMs, const QString& deviceSerial,
    const std::function<void(const QString&)>& log)
{
    Result result;

    const QStringList chain = candidates();
    if (chain.isEmpty()) {
        result.error = QStringLiteral("未找到可用的 fastboot（随包工具、程序目录、System32、PATH 都没有）");
        if (log) log(QStringLiteral("[Fastboot] %1").arg(result.error));
        return result;
    }

    QSet<QString> tried;
    bool stop = false;

    auto attempt = [&](const QString& binary, bool insertForce) {
        if (stop) return;
        const QString key = binary.toLower() + (insertForce ? QStringLiteral("|force")
                                                           : QStringLiteral("|plain"));
        if (tried.contains(key)) return;
        tried.insert(key);

        const QString commandLine = commandLineOf(binary, arguments, deviceSerial, insertForce);
        if (log) log(QStringLiteral("[Fastboot] 尝试: %1").arg(commandLine));

        const RunOutcome outcome =
            runOnce(binary, arguments, timeoutMs, deviceSerial, insertForce);

        Attempt record;
        record.binary = binary;
        record.command = commandLine;
        record.usedForce = insertForce;
        record.success = outcome.finished && outcome.exitCode == 0;
        record.output = outcome.output;
        result.attempts.append(record);
        result.output = outcome.output;

        if (record.success) {
            result.success = true;
            result.binary = binary;
            result.error.clear();
            if (log) {
                log(QStringLiteral("[Fastboot] 成功: %1（退出码 0%2）")
                        .arg(commandLine, insertForce ? QStringLiteral("，带 --force")
                                                      : QString()));
            }
            stop = true;
            return;
        }

        result.error = outcome.output.isEmpty()
                           ? QStringLiteral("fastboot 退出码 %1").arg(outcome.exitCode)
                           : outcome.output;
        if (log) {
            log(QStringLiteral("[Fastboot] 失败（退出码 %1）: %2")
                    .arg(outcome.exitCode)
                    .arg(outcome.output.trimmed().left(400)));
        }

        // 只对「引导未上报分区大小」继续换二进制；其它失败原样返回，
        // 绝不能给所有失败都加 --force（那等于把尺寸保护全关掉）。
        if (!isPartitionSizeUnavailable(outcome.output)) {
            stop = true;
            return;
        }
    };

    // ① 先用当前随包 fastboot 原样跑一次（不改变正常刷写行为）。
    attempt(chain.first(), false);

    // ② 命中「分区大小未上报」才建补偿链：每个候选先带 --force 再不带。
    if (!result.success && !stop) {
        if (log) {
            log(QStringLiteral("[Fastboot] 引导未上报分区大小，依次试本机 %1 个 fastboot"
                               "（每个先 --force 再不带），共 %2 次…")
                    .arg(chain.size())
                    .arg(chain.size() * 2));
        }
        for (const QString& binary : chain) {
            attempt(binary, true);
            attempt(binary, false);
        }
        if (!result.success && log) {
            log(QStringLiteral("[Fastboot] 已试完 %1 个 fastboot × 2 种参数组合，均未成功")
                    .arg(chain.size()));
        }
    }

    return result;
}

} // namespace kc
