#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  AppStatePaths —— 「长期状态」目录 %LOCALAPPDATA%\KunClientState
//  移植自 KunFlashTool.Core.Configuration.AppStatePaths（C#）。
//
//  为什么单独一个目录：退出清理会把 %LOCALAPPDATA%\KunClient 整个删掉，
//  而登录态 / 合规同意 / 服务器地址 / 日志必须跨会话存活，所以挪到这里。
//  迁移是「只读兼容」：新位置没有时仍会去旧目录读一次。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>

namespace kc {

class AppStatePaths
{
public:
    // 状态目录名。注意与缓存目录名 KunClient 区分开——后者退出即删。
    static const QString DirName;
    // 被退出清理删除的旧缓存目录名，仅用于迁移读取。
    static const QString LegacyDirName;
    // 更早的历史目录名（曾命名 KunFlashTool），仅用于迁移读取。
    static const QString LegacyDirName2;

    // %LOCALAPPDATA%\KunClientState（返回前确保已存在）
    static QString Root();

    // 状态目录下的文件路径
    static QString Combine(const QString& fileName);

    // 可复用大件缓存根目录：%LOCALAPPDATA%\KunClientState\cache
    static QString CacheRoot();
    // 大件缓存里的某个子目录（返回前确保已存在）
    static QString Cache(const QString& name);
    // 老位置（退出即被清理）下的同名缓存目录，仅用于迁移搬运
    static QStringList LegacyCaches(const QString& name);
    // 解析大件缓存目录，必要时从老位置搬过来（只搬一次）
    static QString ResolveCache(const QString& name, const QString& legacyName,
                                const QString& probeFile);

    // 旧目录（会被退出清理删除）下同名文件路径，用于迁移读取
    static QString Legacy(const QString& fileName);
    // 更早历史目录下的同名文件路径，用于迁移读取
    static QString Legacy2(const QString& fileName);

    // 按「新位置 → 旧位置 → 更早位置」的顺序找第一个存在的文件，找不到返回空串
    static QString FindExisting(const QString& fileName);
};

} // namespace kc
