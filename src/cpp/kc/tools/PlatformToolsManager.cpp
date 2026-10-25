#include "kc/tools/PlatformToolsManager.h"

#include "kc/config/AppStatePaths.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
   // 顺序有讲究：winsock2.h 必须早于 windows.h（后者在非 LEAN 模式下拉旧版
   // winsock.h，会把 sockaddr 之类重定义）；iphlpapi.h 的 TCP 表结构依赖 winsock2。
#  include <winsock2.h>
#  include <windows.h>
#  include <iphlpapi.h>
#  include <tlhelp32.h>
#endif

#include <QMutex>

namespace kc {
namespace {

// adb / fastboot 启动时由 Windows 加载器从 **exe 同目录** 解析的 DLL：
//   · fastboot 只链 AdbWinApi.dll（USB 传输层）；
//   · adb 还链 AdbWinUsbApi.dll（USB 后端）。
// 缺任何一个 → 0xC0000135「找不到 AdbWinApi.dll」系统弹窗，进程根本没起来。
QStringList requiredDllsFor(const QString& exePath)
{
    const QString name = QFileInfo(exePath).fileName().toLower();
    QStringList dlls{ QStringLiteral("AdbWinApi.dll") };
    if (name.startsWith(QStringLiteral("adb"))) dlls << QStringLiteral("AdbWinUsbApi.dll");
    return dlls;
}

// 可能放着这两个 DLL 的目录（修复时按顺序找来源）。
QStringList dllSourceDirectories(const QString& exePath)
{
    QStringList dirs;
    const QFileInfo info(exePath);
    dirs << info.absolutePath();
    dirs << QCoreApplication::applicationDirPath();
    const QString systemRoot = QString::fromLocal8Bit(qgetenv("SystemRoot"));
    if (!systemRoot.isEmpty())
        dirs << QDir(systemRoot).filePath(QStringLiteral("System32"));
    const QString pathEnv = QString::fromLocal8Bit(qgetenv("PATH"));
    for (const QString& entry : pathEnv.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString trimmed = entry.trimmed();
        if (!trimmed.isEmpty()) dirs << QDir::fromNativeSeparators(trimmed);
    }
    dirs.removeDuplicates();
    return dirs;
}

#ifdef Q_OS_WIN
// 监听指定本地端口的进程 PID（0 = 无人监听）。
// 用 GetExtendedTcpTable 而不是解析 `netstat -ano`：后者输出随系统语言变化
// （中文「LISTENING」/「侦听」），按列切分一换机器就废。
DWORD listeningPidForPort(quint16 port)
{
    DWORD size = 0;
    if (::GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                              TCP_TABLE_OWNER_PID_LISTENER, 0)
        != ERROR_INSUFFICIENT_BUFFER) {
        return 0;
    }

    QByteArray buffer(static_cast<int>(size), '\0');
    auto* table = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    if (::GetExtendedTcpTable(table, &size, FALSE, AF_INET,
                              TCP_TABLE_OWNER_PID_LISTENER, 0)
        != NO_ERROR) {
        return 0;
    }

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_TCPROW_OWNER_PID& row = table->table[i];
        // dwLocalPort 是网络字节序的 DWORD，端口只在低 16 位。
        const quint16 localPort = ntohs(static_cast<quint16>(row.dwLocalPort));
        if (localPort == port && row.dwState == MIB_TCP_STATE_LISTEN) return row.dwOwningPid;
    }
    return 0;
}

// PID → 可执行文件全路径。读不到（权限不足 / 进程已退出）返回空串。
QString processImagePath(DWORD pid)
{
    if (pid == 0) return QString();
    HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!handle) return QString();

    wchar_t path[MAX_PATH] = { 0 };
    DWORD length = MAX_PATH;
    const bool ok = ::QueryFullProcessImageNameW(handle, 0, path, &length) != 0;
    ::CloseHandle(handle);
    return ok ? QDir::toNativeSeparators(QString::fromWCharArray(path)) : QString();
}
#endif

} // namespace

PlatformToolsManager::PlatformToolsManager(const std::function<void(const QString&)>& log)
    : m_log(log)
{
    m_toolsDirectory = AppStatePaths::ResolveCache(QStringLiteral("platform-tools"),
                                                   QStringLiteral("tools"),
                                                   QStringLiteral("adb.exe"));
    ensureToolsDirectory();
}

void PlatformToolsManager::log(const QString& message) const
{
    if (m_log) m_log(message);
}

QString PlatformToolsManager::adbPath() const
{
    return QDir(m_toolsDirectory).filePath(QStringLiteral("adb.exe"));
}

QString PlatformToolsManager::fastbootPath() const
{
    return QDir(m_toolsDirectory).filePath(QStringLiteral("fastboot.exe"));
}

void PlatformToolsManager::ensureToolsDirectory()
{
    if (!QDir(m_toolsDirectory).exists()) {
        QDir().mkpath(m_toolsDirectory);
        log(QStringLiteral("创建工具目录: %1").arg(m_toolsDirectory));
    }
}

bool PlatformToolsManager::checkToolsExist()
{
    const bool adbExists = QFile::exists(adbPath());
    const bool fastbootExists = QFile::exists(fastbootPath());
    log(QStringLiteral("ADB: %1, Fastboot: %2")
            .arg(adbExists ? QStringLiteral("true") : QStringLiteral("false"),
                 fastbootExists ? QStringLiteral("true") : QStringLiteral("false")));
    return adbExists && fastbootExists;
}

bool PlatformToolsManager::ensureToolsExist(const ProgressFn& onProgress)
{
    if (checkToolsExist()) return true;

    // ── 停更离线版（2.0.6）：不再从服务器下载，改为「随包自带」────────────
    //  作者退圈、服务器关停，/assets/tools.zip 已不可用。
    //  发布包里 adb.exe / fastboot.exe / AdbWinApi.dll / AdbWinUsbApi.dll
    //  直接放在 exe 同目录（构建时由 build-release.sh 从 Google 官方
    //  platform-tools 拷入），这里把它们复制到工具缓存目录后直接可用。
    //
    //  为什么仍要复制到缓存目录而不是直接用 exe 同目录：AdbGptReader 等调用方
    //  按 adbPath() 取路径，而 adbPath() 固定在 m_toolsDirectory 下 —— 保持单一
    //  路径口径，避免「有的地方用包内、有的地方用缓存」两套来源打架。
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList needed = { QStringLiteral("adb.exe"),
                                 QStringLiteral("fastboot.exe"),
                                 QStringLiteral("AdbWinApi.dll"),
                                 QStringLiteral("AdbWinUsbApi.dll") };

    log(QStringLiteral("辅助工具未找到，正在从程序目录部署…"));
    int copied = 0;
    for (const QString& name : needed) {
        const QString target = QDir(m_toolsDirectory).filePath(name);
        if (QFile::exists(target)) { ++copied; continue; }

        const QString source = QDir(appDir).filePath(name);
        if (!QFile::exists(source)) {
            // 缺文件不是致命错误：adb 与 fastboot 缺一仍可部分工作，
            // 最终由 checkToolsExist() 判定；这里只把缺失项记进日志。
            log(QStringLiteral("程序目录缺少 %1（该功能可能不可用）").arg(name));
            continue;
        }
        if (QFile::copy(source, target)) {
            ++copied;
        } else {
            log(QStringLiteral("复制 %1 失败").arg(name));
        }
    }

    if (onProgress) {
        // 部署是瞬间完成的，进度直接走满（调用方的进度条不会停在半路）。
        onProgress(qMax(1, copied), qMax(1, copied));
    }

    if (!checkToolsExist()) {
        log(QStringLiteral("辅助工具不可用：请把 adb.exe / fastboot.exe 及 "
                           "AdbWinApi.dll / AdbWinUsbApi.dll 放到程序同目录后重试"));
        return false;
    }
    log(QStringLiteral("辅助工具已就绪（来自程序目录）"));
    return true;
}

QString PlatformToolsManager::binaryReadinessError(const QString& exePath)
{
    if (exePath.trimmed().isEmpty()) return QStringLiteral("未指定工具路径");

    const QFileInfo info(exePath);
    if (!info.exists() || !info.isFile())
        return QStringLiteral("工具不存在: %1").arg(exePath);

    const QDir dir = info.absoluteDir();
    QStringList missing;
    for (const QString& dll : requiredDllsFor(exePath)) {
        if (!dir.exists(dll)) missing << dll;
    }
    if (missing.isEmpty()) return QString();

    return QStringLiteral("%1 组件不完整（同目录缺少 %2）")
        .arg(info.fileName(), missing.join(QStringLiteral(" / ")));
}

bool PlatformToolsManager::ensureBinaryReady(const QString& exePath, QString* error)
{
    QString reason = binaryReadinessError(exePath);
    if (reason.isEmpty()) {
        if (error) error->clear();
        return true;
    }

    const QFileInfo info(exePath);
    if (!info.exists()) {
        if (error) *error = reason;
        return false;
    }

    // 尽力修复一次：从 System32 / PATH / 程序目录把缺失的 DLL 拷到 exe 旁边。
    // （换机器上最典型的成因是杀软把私有目录里的 DLL 隔离了。）
    const QDir targetDir = info.absoluteDir();
    const QStringList sources = dllSourceDirectories(exePath);
    for (const QString& dll : requiredDllsFor(exePath)) {
        if (targetDir.exists(dll)) continue;
        for (const QString& sourceDir : sources) {
            const QString candidate = QDir(sourceDir).filePath(dll);
            if (!QFile::exists(candidate)) continue;
            if (QFile::copy(candidate, targetDir.filePath(dll))) break;
        }
    }

    reason = binaryReadinessError(exePath);
    if (!reason.isEmpty()) {
        reason += QStringLiteral("；已尝试从 System32 / PATH 自动补齐但失败，"
                                 "请重新下载平台工具或检查杀软隔离");
    }
    if (error) *error = reason;
    return reason.isEmpty();
}

bool PlatformToolsManager::platformToolsReady(QString* error)
{
    PlatformToolsManager tools;
    if (!tools.checkToolsExist()) {
        if (error) {
            *error = QStringLiteral("平台工具未就绪（adb.exe / fastboot.exe 缺失）: %1")
                         .arg(tools.toolsDirectory());
        }
        return false;
    }
    return ensureBinaryReady(tools.adbPath(), error)
           && ensureBinaryReady(tools.fastbootPath(), error);
}

PlatformToolsManager::ExecResult PlatformToolsManager::executeAdb(const QString& arguments,
                                                                  int timeoutMs,
                                                                  const QString& deviceSerial)
{
    const QString adb = adbPath();

    // 🔴 组件不齐绝不 spawn：adb.exe 缺同目录 AdbWinApi.dll 时，Windows 加载器会
    // 弹系统错误框（0xC0000135），而且设备轮询每 1.4s 一次 → 弹窗关了又冒。
    QString readyError;
    if (!ensureBinaryReady(adb, &readyError))
        return { false, QString(), readyError };

    log(QStringLiteral("执行ADB命令: adb %1").arg(arguments));

    QProcess process;
    process.setProgram(adb);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setWorkingDirectory(m_toolsDirectory);
    if (!deviceSerial.isEmpty())
        process.setNativeArguments(QStringLiteral("-s %1 %2").arg(deviceSerial, arguments));
    else
        process.setNativeArguments(arguments);

    process.start();
    if (!process.waitForStarted(10000))
        return { false, QString(), QStringLiteral("ADB启动失败") };

    const bool finished = process.waitForFinished(timeoutMs > 0 ? timeoutMs : -1);
    if (!finished) {
        process.kill();
        log(QStringLiteral("ADB命令超时（%1ms）: adb %2").arg(timeoutMs).arg(arguments));
        return { false, QString(), QStringLiteral("ADB命令执行超时（%1ms）").arg(timeoutMs) };
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString error = QString::fromLocal8Bit(process.readAllStandardError());
    const int code = process.exitCode();
    log(QStringLiteral("ADB 命令完成，退出码: %1").arg(code));
    return { code == 0, output, error };
}

PlatformToolsManager::ExecResult PlatformToolsManager::executeFastboot(const QString& arguments,
                                                                       int timeoutMs,
                                                                       const QString& deviceSerial)
{
    const QString fastboot = fastbootPath();

    QString readyError;
    if (!ensureBinaryReady(fastboot, &readyError))
        return { false, QString(), readyError };

    log(QStringLiteral("执行Fastboot命令: fastboot %1").arg(arguments));

    QProcess process;
    process.setProgram(fastboot);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setWorkingDirectory(m_toolsDirectory);
    if (!deviceSerial.isEmpty())
        process.setNativeArguments(QStringLiteral("-s %1 %2").arg(deviceSerial, arguments));
    else
        process.setNativeArguments(arguments);

    process.start();
    if (!process.waitForStarted(10000))
        return { false, QString(), QStringLiteral("Fastboot启动失败") };

    const bool finished = process.waitForFinished(timeoutMs > 0 ? timeoutMs : -1);
    if (!finished) {
        process.kill();
        log(QStringLiteral("Fastboot命令超时（%1ms）: fastboot %2").arg(timeoutMs).arg(arguments));
        return { false, QString(), QStringLiteral("Fastboot命令执行超时（%1ms）").arg(timeoutMs) };
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString error = QString::fromLocal8Bit(process.readAllStandardError());
    const int code = process.exitCode();
    log(QStringLiteral("Fastboot 命令完成，退出码: %1").arg(code));
    return { code == 0, output, error };
}

PlatformToolsManager::ScriptResult
PlatformToolsManager::executeScriptStreaming(const QString& scriptPath,
                                             const QString& workingDirectory,
                                             const LineFn& onLine,
                                             const CancelFn& ct,
                                             int timeoutMs)
{
    if (!QFile::exists(scriptPath)) return { -1, false };

    QProcess process;
    // 用 call 而不是直接引号包脚本路径：cmd /c 对带空格路径的引号裁剪规则很怪，call 语义最稳。
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setNativeArguments(QStringLiteral("/c call \"%1\"").arg(scriptPath));
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setWorkingDirectory(workingDirectory.isEmpty() ? m_toolsDirectory : workingDirectory);

    // fastboot/adb 前置：脚本里裸调 fastboot 时优先解析到我们自带的版本。
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString path = env.value(QStringLiteral("PATH"));
    env.insert(QStringLiteral("PATH"), m_toolsDirectory + QStringLiteral(";") + path);
    process.setProcessEnvironment(env);

    log(QStringLiteral("执行脚本: %1").arg(scriptPath));

    QString outBuf;
    QString errBuf;
    QObject::connect(&process, &QProcess::readyReadStandardOutput, [&]() {
        outBuf += QString::fromLocal8Bit(process.readAllStandardOutput());
        int idx;
        while ((idx = outBuf.indexOf(QLatin1Char('\n'))) >= 0) {
            const QString line = outBuf.left(idx).trimmed();
            outBuf.remove(0, idx + 1);
            if (!line.isEmpty() && onLine) onLine(line);
        }
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, [&]() {
        errBuf += QString::fromLocal8Bit(process.readAllStandardError());
        int idx;
        while ((idx = errBuf.indexOf(QLatin1Char('\n'))) >= 0) {
            const QString line = errBuf.left(idx).trimmed();
            errBuf.remove(0, idx + 1);
            if (!line.isEmpty() && onLine) onLine(line);
        }
    });

    process.start();
    if (!process.waitForStarted(10000)) {
        log(QStringLiteral("脚本启动失败"));
        return { -1, false };
    }

    bool cancelled = false;
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (process.state() != QProcess::NotRunning) {
        if (ct && ct()) {
            cancelled = true;
            // 终止整棵进程树（cmd → fastboot 子进程一并杀掉）。
#ifdef Q_OS_WIN
            const qint64 pid = process.processId();
            if (pid > 0) {
                QProcess taskkill;
                taskkill.start(QStringLiteral("taskkill"),
                               { QStringLiteral("/F"), QStringLiteral("/T"),
                                 QStringLiteral("/PID"), QString::number(pid) });
                taskkill.waitForFinished(5000);
            }
#endif
            process.kill();
            process.waitForFinished(3000);
            break;
        }
        if (timeoutMs > 0 && QDateTime::currentMSecsSinceEpoch() - startMs > timeoutMs) {
            cancelled = true;
            process.kill();
            process.waitForFinished(3000);
            break;
        }
        // 让出事件循环一小段，避免忙等 —— QProcess 的信号需要事件循环才能派发。
        QThread::msleep(30);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        process.waitForReadyRead(10);
    }

    // 收尾：把残留缓冲里最后一行也吐出去。
    outBuf += QString::fromLocal8Bit(process.readAllStandardOutput());
    errBuf += QString::fromLocal8Bit(process.readAllStandardError());
    if (!outBuf.trimmed().isEmpty() && onLine) onLine(outBuf.trimmed());
    if (!errBuf.trimmed().isEmpty() && onLine) onLine(errBuf.trimmed());

    int exitCode = -1;
    if (process.exitStatus() == QProcess::NormalExit) exitCode = process.exitCode();
    if (process.state() != QProcess::NotRunning) {
        process.waitForFinished(3000);
        if (process.exitStatus() == QProcess::NormalExit) exitCode = process.exitCode();
    }

    log(QStringLiteral("脚本执行结束，退出码: %1%2")
            .arg(exitCode)
            .arg(cancelled ? QStringLiteral("（已取消）") : QString()));
    return { exitCode, cancelled };
}

PlatformToolsManager::TarResult
PlatformToolsManager::extractTarGz(const QString& archivePath, const QString& destDir,
                                   const LineFn& onLine, const CancelFn& ct, int timeoutMs)
{
#ifdef Q_OS_WIN
    const QString sysRoot = QString::fromLocal8Bit(qgetenv("SystemRoot"));
    const QString tar = QDir(sysRoot.isEmpty() ? QStringLiteral("C:/Windows") : sysRoot)
                            .filePath(QStringLiteral("System32/tar.exe"));
#else
    const QString tar = QStringLiteral("tar");
#endif
    if (!QFile::exists(tar))
        return { false, QStringLiteral("未找到系统 tar.exe（需要 Windows 10 1803+），"
                                       "请手动解压后选择目录") };
    if (!QFile::exists(archivePath))
        return { false, QStringLiteral("压缩包不存在: %1").arg(archivePath) };

    if (!QDir().mkpath(destDir))
        return { false, QStringLiteral("创建解压目录失败: %1").arg(destDir) };

    QProcess process;
    process.setProgram(tar);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.setWorkingDirectory(destDir);
    process.setNativeArguments(QStringLiteral("-xzf \"%1\" -C \"%2\"").arg(archivePath, destDir));

    log(QStringLiteral("解压线刷包: %1").arg(archivePath));

    QString outBuf;
    QString errBuf;
    auto flush = [&](QString& buf) {
        int idx;
        while ((idx = buf.indexOf(QLatin1Char('\n'))) >= 0) {
            const QString line = buf.left(idx).trimmed();
            buf.remove(0, idx + 1);
            if (!line.isEmpty() && onLine) onLine(line);
        }
    };
    QObject::connect(&process, &QProcess::readyReadStandardOutput, [&]() {
        outBuf += QString::fromLocal8Bit(process.readAllStandardOutput());
        flush(outBuf);
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, [&]() {
        errBuf += QString::fromLocal8Bit(process.readAllStandardError());
        flush(errBuf);
    });

    process.start();
    if (!process.waitForStarted(10000))
        return { false, QStringLiteral("tar 启动失败") };

    bool cancelled = false;
    const qint64 startMs = QDateTime::currentMSecsSinceEpoch();
    while (process.state() != QProcess::NotRunning) {
        if (ct && ct()) { cancelled = true; process.kill(); process.waitForFinished(3000); break; }
        if (timeoutMs > 0 && QDateTime::currentMSecsSinceEpoch() - startMs > timeoutMs) {
            cancelled = true; process.kill(); process.waitForFinished(3000); break;
        }
        QThread::msleep(30);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        process.waitForReadyRead(10);
    }
    if (process.state() != QProcess::NotRunning) process.waitForFinished(3000);

    if (cancelled) return { false, QStringLiteral("解压已取消") };
    const int code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
    return code == 0 ? TarResult{ true, QString() }
                     : TarResult{ false, QStringLiteral("tar 退出码 %1").arg(code) };
}

void PlatformToolsManager::killAdbServer()
{
    try {
        const QString adb = adbPath();
        if (QFile::exists(adb)) {
            QProcess process;
            process.setProgram(adb);
            process.setProcessChannelMode(QProcess::SeparateChannels);
            process.setNativeArguments(QStringLiteral("kill-server"));
            process.start();
            if (!process.waitForFinished(4000)) process.kill();
        }
    } catch (...) {
        log(QStringLiteral("关闭 ADB 服务失败"));
    }

#ifdef Q_OS_WIN
    // 按「可执行文件完整路径 == 我们的 adb」精确杀掉随工具启动的 adb.exe。
    const QString ourAdb = QDir::toNativeSeparators(adbPath());
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (QString::fromWCharArray(pe.szExeFile).compare(QStringLiteral("adb.exe"),
                                                              Qt::CaseInsensitive) != 0)
                continue;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                   FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t path[MAX_PATH] = { 0 };
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &len)) {
                if (QDir::toNativeSeparators(QString::fromWCharArray(path))
                        .compare(ourAdb, Qt::CaseInsensitive) == 0) {
                    TerminateProcess(h, 0);
                    log(QStringLiteral("已关闭随工具启动的 adb 服务进程。"));
                }
            }
            CloseHandle(h);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
#endif
}

PlatformToolsManager::AdbServerInfo PlatformToolsManager::inspectAdbServer()
{
    AdbServerInfo info;
#ifdef Q_OS_WIN
    const DWORD pid = listeningPidForPort(5037);
    if (pid == 0) {
        info.detail = QStringLiteral("5037 端口无人监听（adb 服务未启动）");
        return info;
    }

    info.reachable = true;
    info.pid = static_cast<qint64>(pid);
    info.exePath = processImagePath(pid);

    const QString ours = QDir::toNativeSeparators(adbPath());
    info.ownedByUs = !info.exePath.isEmpty()
                     && info.exePath.compare(ours, Qt::CaseInsensitive) == 0;

    if (info.ownedByUs) {
        info.detail = QStringLiteral("adb 服务由随包工具提供（PID %1）").arg(info.pid);
    } else {
        info.detail = QStringLiteral("5037 被其它程序占用：%1（PID %2）")
                          .arg(info.exePath.isEmpty() ? QStringLiteral("未知进程（权限不足，读不到路径）")
                                                      : info.exePath,
                               QString::number(info.pid));
    }
#else
    info.detail = QStringLiteral("端口接管目前只实现了 Windows 版本");
#endif
    return info;
}

PlatformToolsManager::TakeoverResult
PlatformToolsManager::takeOverAdbServer(int minIntervalMs)
{
    TakeoverResult result;

    const AdbServerInfo before = inspectAdbServer();
    result.before = before;

    // 常态：服务本来就归随包 adb —— 不重启、不消耗冷却，轮询里等于零开销。
    if (before.ownedByUs) {
        result.ok = true;
        result.after = before;
        result.detail = before.detail;
        return result;
    }

    // 冷却闸门：设备轮询每 1.5s 一轮，抢不到时不能每轮都去杀进程 / 起服务。
    {
        static QMutex cooldownMutex;
        static qint64 lastAttemptMs = 0;
        QMutexLocker locker(&cooldownMutex);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (lastAttemptMs > 0 && now - lastAttemptMs < minIntervalMs) {
            result.detail = QStringLiteral("%1（%2 秒内已尝试接管过，稍后自动重试）")
                                .arg(before.detail)
                                .arg((now - lastAttemptMs) / 1000);
            return result;
        }
        lastAttemptMs = now;
    }

    // 5037 被非 adb 程序占着 —— 不杀。那可能是用户正在用的正经程序，
    // 为了「刷机工具能连设备」而杀掉别人的业务进程，代价远大于收益。
    bool needKill = false;
    if (before.pid > 0) {
        const QString base = QFileInfo(before.exePath).fileName().toLower();
        if (base == QStringLiteral("adb.exe")) {
            needKill = true;   // 另一个版本的 adb server，杀了它会自己重启，无数据损失
        } else {
            result.detail = QStringLiteral(
                "5037 被非 adb 程序占用（%1，PID %2），已跳过接管；"
                "请关闭该程序，或在它的设置里释放 5037 端口")
                                .arg(before.exePath.isEmpty()
                                         ? QStringLiteral("未知进程（可能需要管理员权限才能查看）")
                                         : before.exePath,
                                     QString::number(before.pid));
            log(result.detail);
            return result;
        }
    }

    result.attempted = true;

    // ① 终止外来的 adb 服务（它只是个可重启的守护进程，杀掉不影响任何数据）。
    if (needKill) {
#ifdef Q_OS_WIN
        HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                      FALSE, static_cast<DWORD>(before.pid));
        if (!handle) {
            result.detail = QStringLiteral("无法终止外来 adb 服务（PID %1，错误码 %2），"
                                           "可能需要以管理员身份运行")
                                .arg(QString::number(before.pid))
                                .arg(::GetLastError());
            log(result.detail);
            return result;
        }

        const bool killed = ::TerminateProcess(handle, 0) != 0;
        const DWORD killError = killed ? 0 : ::GetLastError();
        ::CloseHandle(handle);

        if (!killed) {
            result.detail = QStringLiteral("终止外来 adb 服务失败（PID %1，错误码 %2），"
                                           "可能需要以管理员身份运行")
                                .arg(QString::number(before.pid))
                                .arg(killError);
            log(result.detail);
            return result;
        }

        result.killedForeign = true;
        result.killedPid = before.pid;
        result.killedExe = before.exePath;
        log(QStringLiteral("已终止外来的 adb 服务：%1（PID %2）")
                .arg(before.exePath, QString::number(before.pid)));

        // 等端口真正释放，别和正在退出的旧进程抢 5037。
        for (int i = 0; i < 20 && listeningPidForPort(5037) != 0; ++i) QThread::msleep(50);
#endif
    }

    // ② 用随包 adb 重新起服务。
    const ExecResult started = executeAdb(QStringLiteral("start-server"), 20000);
    if (!started.success) {
        const QString reason = started.error.trimmed().isEmpty() ? started.output.trimmed()
                                                                 : started.error.trimmed();
        result.detail = QStringLiteral("adb start-server 失败：%1")
                            .arg(reason.isEmpty() ? QStringLiteral("未知原因") : reason);
        log(result.detail);
        return result;
    }

    // ③ 复核：5037 现在必须由随包 adb 提供，否则等于没抢到。
    const AdbServerInfo after = inspectAdbServer();
    result.after = after;
    if (!after.ownedByUs) {
        result.detail = QStringLiteral("接管后 5037 仍不归随包 adb：%1").arg(after.detail);
        log(result.detail);
        return result;
    }

    result.ok = true;
    result.detail = result.killedForeign
                        ? QStringLiteral("已从 %1 抢回 adb 服务（PID %2 → %3）")
                              .arg(QFileInfo(before.exePath).fileName(),
                                   QString::number(before.pid),
                                   QString::number(after.pid))
                        : QStringLiteral("已启动随包 adb 服务（PID %1）").arg(after.pid);
    log(result.detail);
    return result;
}

bool PlatformToolsManager::checkAdbDevice()
{
    const ExecResult r = executeAdb(QStringLiteral("devices"));
    if (r.success && r.output.contains(QStringLiteral("device"))) {
        log(QStringLiteral("检测到ADB设备连接"));
        return true;
    }
    log(QStringLiteral("未检测到ADB设备连接"));
    return false;
}

bool PlatformToolsManager::checkFastbootDevice()
{
    const ExecResult r = executeFastboot(QStringLiteral("devices"));
    if (r.success && r.output.contains(QStringLiteral("fastboot"))) {
        log(QStringLiteral("检测到Fastboot设备连接"));
        return true;
    }
    log(QStringLiteral("未检测到Fastboot设备连接"));
    return false;
}

QStringList PlatformToolsManager::getAdbDevices()
{
    const ExecResult r = executeAdb(QStringLiteral("devices"));
    QStringList devices;
    if (r.success) {
        const QStringList lines = r.output.split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            if (line.contains(QStringLiteral("device")) && !line.contains(QStringLiteral("List")))
                devices.append(line.trimmed());
        }
    }
    return devices;
}

QStringList PlatformToolsManager::getFastbootDevices()
{
    const ExecResult r = executeFastboot(QStringLiteral("devices"));
    QStringList devices;
    if (r.success) {
        const QStringList lines = r.output.split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            if (line.contains(QStringLiteral("fastboot")) && !line.contains(QStringLiteral("List")))
                devices.append(line.trimmed());
        }
    }
    return devices;
}

} // namespace kc
