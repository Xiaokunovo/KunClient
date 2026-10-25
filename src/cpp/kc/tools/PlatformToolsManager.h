#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  PlatformToolsManager —— adb / fastboot 平台工具的下载、管理与执行
//  移植自 KunFlashTool.Core.PlatformTools.PlatformToolsManager.cs（587 行）。
//
//  职责：
//    · 确保 adb.exe / fastboot.exe 存在（服务器下载 → 解压）
//    · 执行 adb / fastboot 命令并捕获 stdout/stderr
//    · 流式执行 .bat 脚本（小米线刷：把工具目录前置进 PATH）
//    · 用系统自带 tar.exe 解压 .tgz / .tar.gz 线刷包
//    · 关闭随本工具启动的 adb 服务进程
//
//  端点顺序（与 C# 完全一致，逐级回落）：
//    ① {ServerUrl}/assets/tools.zip        （CDN 边缘缓存）
//    ② {ServerUrl}/api/packages/tools/file （二进制 API）
//    ③ {ServerUrl}/api/packages/tools      （base64 JSON 回落）
//
//  工具目录 = %LOCALAPPDATA%\KunClientState\cache\platform-tools（跨会话保留），
//  用 AppStatePaths::ResolveCache("platform-tools","tools","adb.exe") 迁移老位置。
//
//  ⚠️ 线程模型：本项目「一个 worker 线程跑一条流程」。下载用 kc::HttpClient /
//     ChunkedDownloader（内部 QEventLoop 阻塞当前线程），子进程执行用 QProcess 的
//     同步 waitForFinished —— 因此**不要**在 GUI 线程直接调用这些阻塞方法。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>

#include <functional>

namespace kc {

class PlatformToolsManager
{
public:
    struct ExecResult
    {
        bool success = false;
        QString output;
        QString error;
    };

    struct ScriptResult
    {
        int exitCode = -1;
        bool cancelled = false;
    };

    // 下载进度回调：(已下载字节, 总字节)；总长未知时为 -1。
    using ProgressFn = std::function<void(qint64 received, qint64 total)>;
    // 逐行输出回调（子进程 stdout/stderr 每行触发）。
    using LineFn = std::function<void(const QString& line)>;
    // 取消令牌：与 ChunkedDownloader::CancelToken 语义一致（原子标志）。
    using CancelFn = std::function<bool()>;

    // log 可为空；构造时解析并创建工具目录。
    explicit PlatformToolsManager(const std::function<void(const QString&)>& log = nullptr);

    // 工具目录（= ResolveCache("platform-tools","tools","adb.exe")）。
    QString toolsDirectory() const { return m_toolsDirectory; }
    QString adbPath() const;
    QString fastbootPath() const;

    // 确保工具目录存在。
    void ensureToolsDirectory();

    // adb.exe 与 fastboot.exe 是否都已就位。
    bool checkToolsExist();

    // 确保平台工具存在，必要时下载并解压。已存在 → 立即返回 true。
    bool ensureToolsExist(const ProgressFn& onProgress = nullptr);

    // 执行 adb 命令（同步阻塞）。deviceSerial 非空时前置 `-s <serial>`。
    // timeoutMs <= 0 表示不限时。
    ExecResult executeAdb(const QString& arguments, int timeoutMs = 30000,
                          const QString& deviceSerial = QString());

    // 执行 fastboot 命令（同步阻塞）。
    ExecResult executeFastboot(const QString& arguments, int timeoutMs = 30000,
                               const QString& deviceSerial = QString());

    // 流式执行一个 Windows 批处理脚本（cmd /c call），每行输出实时回调。
    // 工具目录前置进 PATH，脚本里的 fastboot/adb 才会命中我们自带的工具。
    // workingDirectory 为空时使用工具目录。ct 为可选的取消判定（返回 true 即取消）。
    // timeoutMs = 0 表示不限时。
    ScriptResult executeScriptStreaming(const QString& scriptPath,
                                        const QString& workingDirectory,
                                        const LineFn& onLine,
                                        const CancelFn& ct = nullptr,
                                        int timeoutMs = 0);

    // 用系统自带 tar.exe（Windows 10 1803+）解压 .tgz / .tar.gz 线刷包。
    struct TarResult
    {
        bool success = false;
        QString error;
    };
    TarResult extractTarGz(const QString& archivePath, const QString& destDir,
                           const LineFn& onLine, const CancelFn& ct = nullptr,
                           int timeoutMs = 15 * 60 * 1000);

    // 关闭随本工具启动的 adb 服务进程（adb kill-server + 按路径精确杀 adb.exe）。
    void killAdbServer();

    // ── adb 服务（127.0.0.1:5037）接管 ──────────────────────────────────────
    //  根因：adb 的 server 是**单例**，先起来的那个当服务端。机器上往往同时躺着
    //  好几个 adb.exe（System32 里被别的工具塞进去的旧版、Android Studio 的、
    //  其它刷机工具的），谁先占住 5037，后来者只能当客户端：
    //    · 版本不同 → 「adb server version (33) doesn't match this client (37)」，
    //      adb 会尝试 kill 旧 server，权限不足时直接失败；
    //    · 客户端连不上 → 设备明明插着，界面永远「未连接」。
    //  这里把服务端强制换回**随包 adb**：只杀 adb.exe，绝不碰其它占用端口的程序。
    struct AdbServerInfo
    {
        bool reachable = false;    // 5037 是否有监听者
        bool ownedByUs = false;    // 监听者是否就是随包 adb.exe
        qint64 pid = 0;            // 监听者 PID（0 = 无）
        QString exePath;           // 监听者可执行文件全路径（可能为空：权限不足读不到）
        QString detail;            // 可直接展示的说明
    };

    struct TakeoverResult
    {
        bool attempted = false;    // 是否真的动了手（服务已归我们 / 冷却中 / 被拒 → false）
        bool ok = false;           // 结束时 5037 是否归随包 adb
        bool killedForeign = false;// 是否终止了外来的 adb 服务
        qint64 killedPid = 0;
        QString killedExe;
        AdbServerInfo before;
        AdbServerInfo after;
        QString detail;            // 可直接展示的说明
    };

    // 查看 5037 当前由谁提供（只读，不改变任何状态）。
    AdbServerInfo inspectAdbServer();

    // 抢回 adb 服务：外来 adb 占用 5037 → 终止它 → 随包 adb start-server → 复核。
    // 非 adb 进程占用端口时**不杀**（可能是用户正经在用的程序），只返回原因。
    // minIntervalMs 为冷却窗口（默认 30s）：设备轮询每 1.5s 一轮，不能每轮都重启服务。
    TakeoverResult takeOverAdbServer(int minIntervalMs = 30000);

    // ── 起进程前的组件完整性闸门（移植自 YukiClient `Core/ToolReadinessGate.cs`）──
    //  根因：adb.exe / fastboot.exe 启动时，Windows 加载器只在 **exe 自己所在目录**
    //  找 AdbWinApi.dll；缺失 → 0xC0000135 系统弹窗「找不到 AdbWinApi.dll」。
    //  开发机上往往被 System32 里某个历史安装兜住，换台机器就每次 spawn 都弹一次。
    //  所以：组件不齐**绝不 spawn**，先尝试从别处补齐一次，仍不齐就返回可读失败。
    //
    //  返回空串 = 就绪；否则为可展示的原因。纯静态，可单测。
    static QString binaryReadinessError(const QString& exePath);
    // 检查 + 尽力修复（把缺失的 DLL 从 System32 / PATH / 程序目录拷到 exe 旁）。
    static bool ensureBinaryReady(const QString& exePath, QString* error = nullptr);
    // 平台工具三件套是否齐全（adb.exe + 两个 DLL）。
    static bool platformToolsReady(QString* error = nullptr);

    // 便捷探测。
    bool checkAdbDevice();
    bool checkFastbootDevice();
    QStringList getAdbDevices();
    QStringList getFastbootDevices();

private:
    void log(const QString& message) const;

    std::function<void(const QString&)> m_log;
    QString m_toolsDirectory;
};

} // namespace kc
