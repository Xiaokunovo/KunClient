#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  WallpaperEngineService — Wallpaper Engine 动态壁纸发现服务（kc::tools）。
//
//  移植自 tests/src/KunClient.App/Services/WallpaperEngineService.cs。
//
//  Wallpaper Engine 的壁纸按 project.json 里的 type 分三类：
//   · video —— 视频壁纸（*.mp4 / *.webm）。本工具的动态背景本身就是视频播放器，
//     这一类可以**直接**拿来当背景，画质最好。
//   · scene —— 私有打包格式 scene.pkg，第三方无法还原渲染。
//   · web   —— HTML 壁纸，依赖 Wallpaper Engine 内置浏览器内核。
//  scene / web 的可行兜底：官方预览素材 preview.gif（动图，优先）/ preview.jpg。
//
//  这里负责：定位 Steam 库（注册表 + libraryfolders.vdf）→ 找到创意工坊 AppID
//  431960 目录 → 解析每张壁纸的 project.json → 抽出真正可用的素材路径。
//  纯文件/注册表扫描，无 Qt UI 依赖；扫描跑在调用方的 worker 线程上。
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>
#include <QVector>

namespace kc {

/// 一张壁纸最终能拿来当背景的素材类型。
enum class WallpaperSourceKind {
    None,          ///< 没有可用素材。
    Video,         ///< 视频（mp4/webm…），播放器直接播。
    AnimatedGif,   ///< 动图（GIF），由动态背景逐帧播放。
    StaticImage,   ///< 静态图片。
};

class WallpaperEngineService
{
public:
    /// 一张壁纸的扫描结果。
    struct WallpaperItem {
        QString title;
        /// project.json 里的 type 原值：video / scene / web / 空。
        QString type;
        /// 可直接当背景的素材绝对路径（视频/动图/静态图）；不可用时为空。
        QString sourcePath;
        WallpaperSourceKind sourceKind = WallpaperSourceKind::None;
        /// true = 用的是官方预览素材，而非壁纸的原始素材（scene / web 的降级路径）。
        bool fromPreview = false;
        QString folder;
        /// 从哪个扫描根目录发现的（用于提示文件位置）。
        QString source;

        bool usable() const;
        QString typeText() const;
    };

    /// 扫描本机所有已知位置；可用的排在前面，其余按名称排序。
    static QVector<WallpaperItem> discover();
    /// 只扫描用户手动指定的目录（应对非 Steam 默认安装位置）。
    static QVector<WallpaperItem> discoverIn(const QString& folder);
    /// 本机是否装了 Wallpaper Engine（用于界面给出更贴切的提示）。
    static bool isInstalled();
    /// 把「用户手选的一个路径」解析成真正能用的背景素材（目录/任意文件均可）。
    static bool resolveOne(const QString& path, WallpaperItem* out);

    /// 取「同一张壁纸的官方预览素材」（preview.gif / preview.jpg / preview.png，
    /// 优先 project.json 的 preview 字段）。
    ///
    /// 用途：视频壁纸（mp4/webm）本客户端还放不了 —— 直接 QImage::load() 会静默失败，
    /// 用户看到的就是「自定义背景失效」。这里给出降级素材，让背景至少出得来。
    /// 传入目录或该壁纸内的任意文件都可以；找不到返回空串。
    static QString previewFor(const QString& wallpaperPath);

private:
    static void scanRoot(const QString& root, QVector<WallpaperItem>* items);
    static bool parseFolder(const QString& folder, const QString& fallbackTitle,
                            const QString& source, WallpaperItem* out);
    static QVector<WallpaperItem> dedup(QVector<WallpaperItem> items);

    static QStringList steamRoots();
    static QStringList libraryRoots(const QString& steamRoot);
    static QStringList candidateProjectRoots();

    static QString normalize(const QString& path);
    static QString resolveRelative(const QString& folder, const QString& relative);
    static QString findPreview(const QString& folder, const QString& previewName);
    static QString findFirstVideo(const QString& folder);
    static bool isVideo(const QString& path);
    static bool isImage(const QString& path);
};

} // namespace kc
