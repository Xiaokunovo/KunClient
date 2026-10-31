#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  UpdateService —— 检查更新 → 下载 → 校验 → 提示重启
//
//  🔴 这个文件的安全前提写在 kc/update/UpdateVerifier.h：**先有校验，才有更新**。
//     SECURITY.md 把「更新包无签名校验」列为严重问题（旧代码只查文件头是不是 MZ）。
//     本类在下载完成后**必须**走 UpdateVerifier::verifyPackage()，
//     校验不过就删掉文件、绝不执行。
//
//  ── 流程 ────────────────────────────────────────────────────────────────
//    ① ApiClient 握手（拿签名会话）
//    ② GET /api/v2/app/version  —— 带签名
//       注意：服务端要求「已登录」才下发 downloadTicket。本项目登录界面已移除，
//       所以拿不到票据时会退化成「只检查更新、给下载链接」——
//       这是有意的降级，不是 bug：**不自动替换可执行文件**本来就比自动替换安全。
//    ③ 有新版本 → 下载到临时目录（带进度）
//    ④ 校验：SHA-256 完整性 + ECDSA P-256 真实性
//    ⑤ 校验通过 → 提示用户「新版本已下载，是否立即安装」
//       安装 = 启动一个替换脚本（重命名旧 exe → 移入新 exe → 重启），
//       因为 Windows 上**运行中的 exe 不能被覆盖**。
//
//  ── 为什么用「外部脚本替换」而不是自己替换 ──────────────────────────────
//    进程自己删不掉/覆盖不了正在运行的 exe。标准做法是起一个 cmd 脚本：
//    等本进程退出 → 移动旧文件 → 复制新文件 → 启动新进程 → 自删。
//    这是 Windows 上更新器的通用形态（Chrome / VS Code 都这么干）。
// ─────────────────────────────────────────────────────────────────────────────

#include <QObject>
#include <QString>

#include <functional>

namespace kc::update {

/// 检查更新的结果。
struct UpdateCheckResult {
    bool ok = false;              ///< 检查动作本身成功（不代表有新版本）
    bool hasUpdate = false;       ///< 服务端版本 > 当前版本
    bool updateEnabled = true;    ///< 服务端是否开放了自更新通道
    QString closedReason;         ///< 通道关闭时的原因（中文）
    QString latestVersion;
    QString currentVersion;
    QString changelog;
    QString downloadUrl;          ///< 可能是站内相对路径
    QString sha256;               ///< 服务器下发的包哈希（可能为空 → 无法校验）
    QString signatureBase64;      ///< 服务器下发的签名（可能为空）
    bool forceUpdate = false;
    QString error;                ///< ok=false 时的原因
};

/// 下载进度回调：(已收字节, 总字节)。总字节未知时 -1。
using ProgressFn = std::function<void(qint64, qint64)>;

class UpdateService : public QObject {
    Q_OBJECT

public:
    explicit UpdateService(QObject* parent = nullptr);
    ~UpdateService() override;

    /// 服务端根地址（如 https://kunawa.cn）。
    void setBaseUrl(const QString& base);

    /// 当前客户端版本（默认取 kc::kVersionString）。
    void setCurrentVersion(const QString& version);

    /// 检查更新（异步）。回调在 UI 线程执行。
    void check(std::function<void(const UpdateCheckResult&)> done);

    /// 下载并校验（异步）。
    /// 成功时 *savedPath 是**已通过校验**的临时文件路径。
    /// 校验失败会删除文件并返回 ok=false。
    void downloadAndVerify(const UpdateCheckResult& info,
                           ProgressFn progress,
                           std::function<void(bool ok, const QString& savedPath,
                                              const QString& error)> done);

    /// 应用更新：写一个替换脚本、启动它、然后让调用方退出程序。
    /// 🔴 只在 downloadAndVerify 成功之后调用。
    /// 返回 false = 无法创建脚本（error 说明原因），此时**不要**退出程序。
    static bool applyUpdate(const QString& verifiedPackagePath, QString* error);

    /// 临时下载目录（%LOCALAPPDATA%\KunClientState\update）
    static QString updateTempDir();

private:
    class Impl;
    Impl* d = nullptr;
};

} // namespace kc::update
