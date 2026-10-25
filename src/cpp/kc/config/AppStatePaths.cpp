#include "kc/config/AppStatePaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace kc {
namespace {

// %LOCALAPPDATA% 的口径。Qt6 没有 LocalAppDataLocation（AppLocalDataLocation 会再拼
// 组织/应用名，与 C# 的 Environment.SpecialFolder.LocalApplicationData 不一致），
// 所以直接读环境变量；取不到再退回 GenericDataLocation，避免返回空串导致路径拼到根上。
QString localAppData()
{
    const QString env = QProcessEnvironment::systemEnvironment().value(QStringLiteral("LOCALAPPDATA"));
    if (!env.isEmpty()) return QDir::fromNativeSeparators(env);

    const QString p = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return p.isEmpty() ? QDir::homePath() : p;
}

// 递归把 source 目录整个复制到 target（覆盖已有文件）。
// 对应 C# 的 CopyDirectory：先建全部子目录，再复制全部文件。
void copyDirectory(const QString& source, const QString& target)
{
    QDir srcDir(source);
    if (!srcDir.exists()) return;

    QDir().mkpath(target);
    const QDir tgtDir(target);

    const QFileInfoList dirs = srcDir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& d : dirs) {
        const QString rel = srcDir.relativeFilePath(d.absoluteFilePath());
        const QString dst = tgtDir.absoluteFilePath(rel);
        QDir().mkpath(dst);
        copyDirectory(d.absoluteFilePath(), dst);
    }

    const QFileInfoList files = srcDir.entryInfoList(QDir::Files, QDir::Name);
    for (const QFileInfo& f : files) {
        const QString rel = srcDir.relativeFilePath(f.absoluteFilePath());
        const QString dst = tgtDir.absoluteFilePath(rel);
        // 目标已存在时强行覆盖（对应 File.Copy overwrite: true）。
        if (QFile::exists(dst)) QFile::remove(dst);
        QFile::copy(f.absoluteFilePath(), dst);
    }
}

} // namespace

const QString AppStatePaths::DirName = QStringLiteral("KunClientState");
const QString AppStatePaths::LegacyDirName = QStringLiteral("KunClient");
const QString AppStatePaths::LegacyDirName2 = QStringLiteral("KunFlashTool");

QString AppStatePaths::Root()
{
    const QString dir = QDir(localAppData()).filePath(DirName);
    // 建目录失败静默忽略：与 C# 的 try/catch 一致，调用方后续操作各自容错。
    QDir().mkpath(dir);
    return dir;
}

QString AppStatePaths::Combine(const QString& fileName)
{
    return QDir(Root()).filePath(fileName);
}

QString AppStatePaths::CacheRoot()
{
    const QString dir = QDir(Root()).filePath(QStringLiteral("cache"));
    QDir().mkpath(dir);
    return dir;
}

QString AppStatePaths::Cache(const QString& name)
{
    const QString dir = QDir(CacheRoot()).filePath(name);
    QDir().mkpath(dir);
    return dir;
}

QStringList AppStatePaths::LegacyCaches(const QString& name)
{
    const QString tmp = QDir::tempPath();
    const QString lad = localAppData();
    return {
        QDir(tmp).filePath(LegacyDirName + "/" + name),
        QDir(lad).filePath(LegacyDirName + "/" + name),
        QDir(tmp).filePath(LegacyDirName2 + "/" + name),
        QDir(lad).filePath(LegacyDirName2 + "/" + name),
    };
}

QString AppStatePaths::ResolveCache(const QString& name, const QString& legacyName,
                                    const QString& probeFile)
{
    const QString target = Cache(name);
    if (!probeFile.isEmpty() && QFile::exists(QDir(target).filePath(probeFile)))
        return target;

    const QStringList legacies = LegacyCaches(legacyName);
    for (const QString& legacy : legacies) {
        if (!QFile::exists(QDir(legacy).filePath(probeFile))) continue;
        copyDirectory(legacy, target);
        return target;
    }
    return target;
}

QString AppStatePaths::Legacy(const QString& fileName)
{
    return QDir(localAppData()).filePath(LegacyDirName + "/" + fileName);
}

QString AppStatePaths::Legacy2(const QString& fileName)
{
    return QDir(localAppData()).filePath(LegacyDirName2 + "/" + fileName);
}

QString AppStatePaths::FindExisting(const QString& fileName)
{
    const QStringList candidates = { Combine(fileName), Legacy(fileName), Legacy2(fileName) };
    for (const QString& p : candidates) {
        if (QFile::exists(p)) return p;
    }
    return {};
}

} // namespace kc
