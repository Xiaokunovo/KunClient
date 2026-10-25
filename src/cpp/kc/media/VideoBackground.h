#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  VideoBackground —— 动态壁纸的视频解码器（Windows Media Foundation）
//
//  为什么不用 Qt6::Multimedia：
//    · mingw64（动态包）那套工具链**根本没装** qt6-multimedia；
//    · ucrt64（静态单文件包用的）虽然有 Qt6Multimedia 的 cmake 配置，却**没有
//      FFmpeg 后端**（qt6-static 里既无 ffmpeg 库也无 multimedia 插件目录）——
//      走 Qt 的话，「我就要一个文件」那个静态包会直接失去视频背景。
//  MF 是 Windows 自带组件（Win7 起），两套构建都能用，且**不增加任何分发体积**：
//  只是多链 mfplat / mfreadwrite / mfuuid 三个系统导入库。
//
//  解码跑在**独立工作线程**里（该线程进 MTA）：1080p 一帧的色彩转换是毫秒级
//  开销，放主线程上做会和 UI 抢帧 —— 那正是用户抱怨的「切页卡顿」的成因。
//
//  支持范围取决于系统解码器：H.264（mp4/mov 最常见）Windows 自带；HEVC/H.265
//  需要装「HEVC 视频扩展」；VP9/AV1 的 webm 多数机器解不了。探测失败时
//  open() 返回 false 并给出中文原因，调用方据此回落到静态预览图。
// ─────────────────────────────────────────────────────────────────────────────

#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

class QThread;

namespace kc::media {

class VideoWorker;

class VideoBackground : public QObject {
    Q_OBJECT

public:
    explicit VideoBackground(QObject* parent = nullptr);
    ~VideoBackground() override;

    /// 同步打开并探测（拿到分辨率才算成功），随后在工作线程上循环解码。
    /// 失败返回 false；reason 非空时写入中文失败原因。
    bool open(const QString& path, QString* reason = nullptr);
    /// 停止解码并回收线程。可重复调用。
    void close();

    bool isOpen() const { return m_open; }
    QSize frameSize() const { return m_frameSize; }

    /// 解码输出的长边上限（默认 1280）：壁纸是铺满窗口再模糊的，喂全分辨率
    /// 只是白白多搬内存。设成窗口尺寸附近最省。
    void setMaxDimension(int pixels);
    int maxDimension() const { return m_maxDimension; }

    /// 按扩展名粗筛「这看起来是不是视频」。真正能不能解仍要看 open()。
    static bool looksLikeVideo(const QString& path);
    /// 常见视频扩展名的文件对话框过滤器串。
    static QString dialogFilter();

signals:
    /// 解出一帧（已按 maxDimension 等比缩放）。跨线程队列投递。
    void frameReady(const QImage& frame);

private:
    QThread* m_thread = nullptr;
    VideoWorker* m_worker = nullptr;
    bool m_open = false;
    QSize m_frameSize;
    int m_maxDimension = 1280;
};

} // namespace kc::media
