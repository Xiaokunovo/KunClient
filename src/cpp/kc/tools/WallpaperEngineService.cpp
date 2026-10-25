#include "kc/tools/WallpaperEngineService.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

namespace kc {
namespace {

/// Wallpaper Engine 在 Steam 上的 AppID，创意工坊子目录名就是它。
constexpr const char* kWeAppId = "431960";

/// 播放器（Qt Multimedia / Media Foundation）能吃下的视频容器。
const char* kVideoExtensions[] = { ".mp4", ".webm", ".avi", ".mkv", ".mov", ".wmv" };

/// 能解码的图片格式（**不含 .webp**：与 C# 同一口径，硬塞会退回渐变底）。
const char* kImageExtensions[] = { ".gif", ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff" };

/// 预览素材的候选文件名（project.json 未给出 preview 字段时的兜底顺序）。
const char* kPreviewCandidates[] = { "preview.gif", "preview.jpg", "preview.jpeg",
                                     "preview.png" };

QString extOf(const QString& path)
{
    const QString ext = QFileInfo(path).suffix();
    return ext.isEmpty() ? QString() : QStringLiteral(".") + ext.toLower();
}

} // namespace

bool WallpaperEngineService::WallpaperItem::usable() const
{
    return sourceKind != WallpaperSourceKind::None && !sourcePath.isEmpty()
           && QFile::exists(sourcePath);
}

QString WallpaperEngineService::WallpaperItem::typeText() const
{
    switch (sourceKind) {
    case WallpaperSourceKind::Video:
        return QStringLiteral("视频 · 可直接使用");
    case WallpaperSourceKind::AnimatedGif:
        return fromPreview ? QStringLiteral("场景/网页 · 官方预览动图")
                           : QStringLiteral("动图");
    case WallpaperSourceKind::StaticImage:
        return fromPreview ? QStringLiteral("场景/网页 · 官方预览图（静态）")
                           : QStringLiteral("图片 · 静态");
    case WallpaperSourceKind::None:
        break;
    }
    if (type == QStringLiteral("scene")) return QStringLiteral("场景 · 未找到预览素材");
    if (type == QStringLiteral("web")) return QStringLiteral("网页 · 未找到预览素材");
    if (type == QStringLiteral("video")) return QStringLiteral("视频 · 文件缺失");
    return type.isEmpty() ? QStringLiteral("未知类型") : type;
}

QVector<WallpaperEngineService::WallpaperItem> WallpaperEngineService::discover()
{
    QVector<WallpaperItem> items;
    const QStringList roots = candidateProjectRoots();
    for (const QString& root : roots) scanRoot(root, &items);
    return dedup(std::move(items));
}

QVector<WallpaperEngineService::WallpaperItem> WallpaperEngineService::discoverIn(
    const QString& folder)
{
    QVector<WallpaperItem> items;
    if (QDir(folder).exists()) scanRoot(folder, &items);
    return dedup(std::move(items));
}

bool WallpaperEngineService::isInstalled()
{
    const QStringList roots = candidateProjectRoots();
    for (const QString& root : roots)
        if (QDir(root).exists()) return true;
    return false;
}

bool WallpaperEngineService::resolveOne(const QString& path, WallpaperItem* out)
{
    if (path.trimmed().isEmpty()) return false;

    if (QFileInfo(path).isDir()) {
        const QString folder = QDir(path).absolutePath();
        return parseFolder(folder, QFileInfo(folder).fileName(), folder, out);
    }
    if (!QFile::exists(path)) return false;

    const QString full = normalize(path);
    const QString dir = QFileInfo(full).absolutePath();

    // 文件在某个含 project.json 的目录里 → 这是 WE 壁纸的一部分（常见于用户手选
    // preview.gif）。先按整张壁纸解析：能给出原始视频就用原始视频。
    if (QFile::exists(dir + QStringLiteral("/project.json"))) {
        WallpaperItem byProject;
        if (parseFolder(dir, QFileInfo(dir).fileName(), dir, &byProject)
            && byProject.usable()) {
            if (out) *out = byProject;
            return true;
        }
    }

    WallpaperSourceKind kind = WallpaperSourceKind::None;
    const QString ext = extOf(full);
    if (isVideo(full)) kind = WallpaperSourceKind::Video;
    else if (ext == QStringLiteral(".gif")) kind = WallpaperSourceKind::AnimatedGif;
    else if (isImage(full)) kind = WallpaperSourceKind::StaticImage;
    if (kind == WallpaperSourceKind::None) return false;

    WallpaperItem item;
    item.title = QFileInfo(full).completeBaseName();
    item.type = QStringLiteral("file");
    item.sourcePath = full;
    item.sourceKind = kind;
    item.fromPreview = false;
    item.folder = dir;
    item.source = dir;
    if (out) *out = item;
    return true;
}

QString WallpaperEngineService::previewFor(const QString& wallpaperPath)
{
    if (wallpaperPath.trimmed().isEmpty()) return QString();

    // 目录 / 文件都接受：都归一到「壁纸所在目录」再找预览素材。
    const QFileInfo info(normalize(wallpaperPath));
    const QString dir = info.isDir() ? QDir(info.absoluteFilePath()).absolutePath()
                                     : info.absolutePath();
    if (dir.isEmpty() || !QDir(dir).exists()) return QString();

    QString previewName;
    QFile f(dir + QStringLiteral("/project.json"));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject r = QJsonDocument::fromJson(f.readAll()).object();
        previewName = r.value(QStringLiteral("preview")).toString();
    }
    // findPreview 内部只认可解码的图片后缀（preview.gif / preview.jpg / preview.png）。
    return findPreview(dir, previewName);
}

// ═══════════════════════════ 目录发现 ═══════════════════════════

QStringList WallpaperEngineService::steamRoots()
{
    QStringList list;
    auto add = [&list](const QString& p) {
        const QString norm = normalize(p);
        if (norm.isEmpty() || !QDir(norm).exists()) return;
        for (const QString& existing : list)
            if (existing.compare(norm, Qt::CaseInsensitive) == 0) return;
        list.append(norm);
    };

    // 注册表是最可靠的来源：Steam 可以装在任意盘/任意目录。
    add(QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                  QSettings::NativeFormat)
            .value(QStringLiteral("SteamPath"))
            .toString());
    add(QSettings(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam"),
                  QSettings::NativeFormat)
            .value(QStringLiteral("InstallPath"))
            .toString());
    add(QSettings(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam"),
                  QSettings::NativeFormat)
            .value(QStringLiteral("InstallPath"))
            .toString());
    // 兜底：两个默认安装位置（ProgramFiles(x86) 与 ProgramFiles）。
    const QString programFilesX86 = qEnvironmentVariable("ProgramFiles(x86)");
    if (!programFilesX86.isEmpty()) add(programFilesX86 + QStringLiteral("/Steam"));
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    if (!programFiles.isEmpty()) add(programFiles + QStringLiteral("/Steam"));
    return list;
}

QStringList WallpaperEngineService::libraryRoots(const QString& steamRoot)
{
    QStringList libs{ steamRoot };
    const QString vdf = steamRoot + QStringLiteral("/steamapps/libraryfolders.vdf");
    QFile f(vdf);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return libs;

    const QString text = QString::fromUtf8(f.readAll());
    QRegularExpressionMatchIterator matches =
        QRegularExpression(QStringLiteral("\"path\"\\s*\"([^\"]+)\""),
                           QRegularExpression::CaseInsensitiveOption)
            .globalMatch(text);
    bool any = false;
    while (matches.hasNext()) {
        const QString p = normalize(matches.next().captured(1)
                                        .replace(QStringLiteral("\\\\"),
                                                 QStringLiteral("\\")));
        if (p.isEmpty() || !QDir(p).exists()) continue;
        bool dup = false;
        for (const QString& existing : libs)
            if (existing.compare(p, Qt::CaseInsensitive) == 0) { dup = true; break; }
        if (!dup) libs.append(p);
        any = true;
    }

    // 旧版 VDF 没有 path 键，直接是 "序号" "盘符路径"。
    if (!any) {
        matches = QRegularExpression(QStringLiteral("\"\\d+\"\\s*\"([A-Za-z]:[^\"]+)\""))
                      .globalMatch(text);
        while (matches.hasNext()) {
            const QString p = normalize(matches.next().captured(1)
                                            .replace(QStringLiteral("\\\\"),
                                                     QStringLiteral("\\")));
            if (p.isEmpty() || !QDir(p).exists()) continue;
            bool dup = false;
            for (const QString& existing : libs)
                if (existing.compare(p, Qt::CaseInsensitive) == 0) { dup = true; break; }
            if (!dup) libs.append(p);
        }
    }
    return libs;
}

QStringList WallpaperEngineService::candidateProjectRoots()
{
    QStringList roots;
    for (const QString& steam : steamRoots()) {
        for (const QString& lib : libraryRoots(steam)) {
            // 创意工坊订阅的壁纸。
            roots.append(lib + QStringLiteral("/steamapps/workshop/content/")
                         + QString::fromLatin1(kWeAppId));
            // 用户自己在 Wallpaper Engine 里「创建壁纸」生成的工程。
            roots.append(lib + QStringLiteral("/steamapps/common/wallpaper_engine/"
                                              "projects/myprojects"));
            roots.append(lib + QStringLiteral("/steamapps/common/wallpaper_engine/"
                                              "projects/defaultprojects"));
        }
    }
    return roots;
}

QString WallpaperEngineService::normalize(const QString& path)
{
    if (path.trimmed().isEmpty()) return path;
    // 统一成反斜杠绝对路径：Steam 注册表里的 SteamPath 用正斜杠（e:/steam），
    // HKLM 的 InstallPath 与 VDF 里的库路径用反斜杠；不统一的话同一份安装会被
    // 当成两个扫描根，每张壁纸都会被列两遍。
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

// ═══════════════════════════ 壁纸解析 ═══════════════════════════

void WallpaperEngineService::scanRoot(const QString& root, QVector<WallpaperItem>* items)
{
    if (!QDir(root).exists()) return;

    // ① 用户直接选了「某一张壁纸」的文件夹：根目录自己就带 project.json 或素材。
    WallpaperItem self;
    if (parseFolder(QDir(root).absolutePath(), QFileInfo(root).fileName(), root, &self)) {
        items->append(self);
        return;
    }

    // ② 正常情况下根目录下是若干张壁纸的文件夹（创意工坊目录名是数字 ID）。
    const QFileInfoList dirs =
        QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& sub : dirs) {
        WallpaperItem item;
        if (parseFolder(sub.absoluteFilePath(), sub.fileName(), root, &item))
            items->append(item);
    }
}

bool WallpaperEngineService::parseFolder(const QString& folder,
                                         const QString& fallbackTitle,
                                         const QString& source, WallpaperItem* out)
{
    QString title = fallbackTitle;
    QString type;
    QString rawVideo;      // project.json 的 file 字段指向的视频（原始素材）
    QString previewName;   // project.json 的 preview 字段

    const QString projectJson = folder + QStringLiteral("/project.json");
    if (QFile::exists(projectJson)) {
        QFile f(projectJson);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject r =
                QJsonDocument::fromJson(f.readAll()).object();
            const QString t = r.value(QStringLiteral("title")).toString();
            if (!t.trimmed().isEmpty()) title = t;
            type = r.value(QStringLiteral("type")).toString().trimmed().toLower();
            previewName = r.value(QStringLiteral("preview")).toString();
            rawVideo = resolveRelative(folder,
                                       r.value(QStringLiteral("file")).toString());
        }
    }

    if (rawVideo.isEmpty() || !isVideo(rawVideo)) rawVideo = findFirstVideo(folder);

    // ── 决定最终可用的素材 ──
    QString path;
    WallpaperSourceKind kind = WallpaperSourceKind::None;
    bool fromPreview = false;

    if (!rawVideo.isEmpty()) {
        path = rawVideo;
        kind = WallpaperSourceKind::Video;
    } else {
        // scene / web（以及任何没有视频的壁纸）：退到官方的预览素材。
        const QString previewPath = findPreview(folder, previewName);
        if (!previewPath.isEmpty()) {
            path = previewPath;
            fromPreview = true;
            kind = extOf(previewPath) == QStringLiteral(".gif")
                       ? WallpaperSourceKind::AnimatedGif
                       : WallpaperSourceKind::StaticImage;
        }
    }

    // 既没有可识别类型、也找不到任何素材 → 不是一张壁纸目录，跳过。
    if (type.isEmpty() && path.isEmpty()) return false;

    WallpaperItem item;
    item.title = title.trimmed().isEmpty() ? QStringLiteral("(未命名壁纸)") : title;
    item.type = type;
    item.sourcePath = path;
    item.sourceKind = kind;
    item.fromPreview = fromPreview;
    item.folder = normalize(folder);
    item.source = source;
    if (out) *out = item;
    return true;
}

QString WallpaperEngineService::resolveRelative(const QString& folder,
                                                const QString& relative)
{
    if (relative.trimmed().isEmpty()) return QString();
    const QString abs = QDir::cleanPath(folder + QLatin1Char('/') + relative);
    return QFile::exists(abs) ? abs : QString();
}

QString WallpaperEngineService::findPreview(const QString& folder,
                                            const QString& previewName)
{
    // 优先 project.json 的 preview 字段，否则按固定候选名探测。只认可解码的格式。
    const QString byField = resolveRelative(folder, previewName);
    if (!byField.isEmpty() && isImage(byField)) return byField;

    for (const char* candidate : kPreviewCandidates) {
        const QString abs = folder + QStringLiteral("/") + QString::fromLatin1(candidate);
        if (QFile::exists(abs)) return abs;
    }
    return QString();
}

QString WallpaperEngineService::findFirstVideo(const QString& folder)
{
    const QFileInfoList files =
        QDir(folder).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : files)
        if (isVideo(fi.absoluteFilePath())) return fi.absoluteFilePath();
    return QString();
}

bool WallpaperEngineService::isVideo(const QString& path)
{
    const QString ext = extOf(path);
    for (const char* e : kVideoExtensions)
        if (ext == QString::fromLatin1(e)) return true;
    return false;
}

bool WallpaperEngineService::isImage(const QString& path)
{
    const QString ext = extOf(path);
    for (const char* e : kImageExtensions)
        if (ext == QString::fromLatin1(e)) return true;
    return false;
}

QVector<WallpaperEngineService::WallpaperItem> WallpaperEngineService::dedup(
    QVector<WallpaperItem> items)
{
    QVector<WallpaperItem> result;
    QStringList seen;
    for (WallpaperItem& it : items) {
        // 去重键必须规范化：Steam 注册表里的 SteamPath 是正斜杠、HKLM 是反斜杠，
        // 否则同一份安装会被当成两个扫描根，整份列表翻倍。
        const QString key = normalize(it.sourcePath.isEmpty() ? it.folder : it.sourcePath);
        if (!key.isEmpty() && !seen.contains(key, Qt::CaseInsensitive)) {
            seen.append(key);
            result.append(it);
        }
    }

    std::sort(result.begin(), result.end(),
              [](const WallpaperItem& a, const WallpaperItem& b) {
                  const int byUsable =
                      (b.usable() ? 1 : 0) - (a.usable() ? 1 : 0);
                  if (byUsable != 0) return byUsable > 0;
                  // 同为可用时，视频（原始素材）排在降级的预览素材之前。
                  if (a.sourceKind != b.sourceKind)
                      return static_cast<int>(a.sourceKind)
                             < static_cast<int>(b.sourceKind);
                  return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
              });
    return result;
}

} // namespace kc
