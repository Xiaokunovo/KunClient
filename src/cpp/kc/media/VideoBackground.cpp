#include "kc/media/VideoBackground.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSemaphore>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <objbase.h>
#  include <propidl.h>
#  include <mfapi.h>
#  include <mfidl.h>
#  include <mfreadwrite.h>
#endif

namespace kc::media {

namespace {

#ifdef Q_OS_WIN

/// 极简 COM 智能指针。不用 wrl/client.h —— MinGW 上它时有时无，不值得赌。
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    /// 取「用于接收新对象的地址」，会先释放旧对象。
    T** put()
    {
        reset();
        return &m_p;
    }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    void reset()
    {
        if (m_p) {
            m_p->Release();
            m_p = nullptr;
        }
    }

private:
    T* m_p = nullptr;
};

QString hresultText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

const char* const kVideoExts[] = { ".mp4", ".m4v", ".mov", ".webm", ".mkv",
                                   ".avi", ".wmv", ".mpg", ".mpeg", ".ts" };

/// 壁纸的目标帧率上限（原片 30fps，壁纸 24 肉眼分不出）。
constexpr UINT32 kTargetFps = 24;
/// 相邻两帧的最小间隔（毫秒）：壁纸 24fps 足够，跳掉的帧连 memcpy + 色彩转换
/// 一起省了（解码本身省不掉，ReadSample 一定要跑）。
constexpr qint64 kFrameIntervalMs = 1000 / static_cast<qint64>(kTargetFps);

#endif // Q_OS_WIN

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
//  VideoWorker —— 真正干活的那个对象，活在工作线程上
//
//  它**不在**事件循环里跑：run() 一口气解到底，靠 atomic stop 旗标退出。
//  原因是 MF 的同步 ReadSample 会阻塞，用队列投递的 quit() 根本送不进来。
// ─────────────────────────────────────────────────────────────────────────────
class VideoWorker : public QObject {
    Q_OBJECT

public:
    VideoWorker(QSize* sizeOut, QString* errorOut, QSemaphore* ready, int maxDim)
        : m_sizeOut(sizeOut), m_errorOut(errorOut), m_ready(ready), m_maxDim(maxDim) {}

    std::atomic<bool> stop{false};

public slots:
    void run(const QString& path);

signals:
    void frameReady(const QImage& frame);

private:
    QSize* m_sizeOut = nullptr;
    QString* m_errorOut = nullptr;
    QSemaphore* m_ready = nullptr;
    int m_maxDim = 1280;
};

#ifdef Q_OS_WIN

void VideoWorker::run(const QString& path)
{
    // 探测结果只报一次；之后的失败都靠「没有帧」体现（调用方已经拿到图或没拿到）。
    bool announced = false;
    // 供 announce 里的诊断打印用：请求的输出尺寸（在拿到原生类型后才知道）。
    int wantW = 0;
    int wantH = 0;
    const auto announce = [&](bool ok, const QSize& size, const QString& error) {
        if (announced) return;
        announced = true;
        if (ok) {
            if (m_sizeOut) *m_sizeOut = size;
        } else if (m_errorOut) {
            *m_errorOut = error;
        }
        // 同时打一份到 stderr：GUI 版的 qWarning 走 OutputDebugString，探针抓不到。
        const QString line =
            ok ? QStringLiteral("[video] opened %1x%2 (requested %3x%4, %5fps)")
                     .arg(size.width())
                     .arg(size.height())
                     .arg(wantW)
                     .arg(wantH)
                     .arg(kTargetFps)
               : QStringLiteral("[video] failed: %1").arg(error);
        std::fprintf(stderr, "%s\n", qPrintable(line));
        if (m_ready) m_ready->release();
    };

    // 这个线程要进 MTA —— MF 的同步对象跨线程使用时，STA 会被自动封送、
    // 每次调用都绕一圈，白白多出开销。
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(comHr);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        announce(false, {}, QStringLiteral("COM 初始化失败（%1）").arg(hresultText(comHr)));
        return;
    }

    // MFSTARTUP_LITE 就够了：只要解码器 + 色彩转换，不碰网络栈。
    const HRESULT startupHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(startupHr)) {
        announce(false, {}, QStringLiteral("Media Foundation 启动失败（%1）")
                               .arg(hresultText(startupHr)));
        if (comOwned) CoUninitialize();
        return;
    }

    const auto shutdown = [&] {
        MFShutdown();
        if (comOwned) CoUninitialize();
    };

    // MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING 必须**在创建 reader 之前**
    // 通过属性传进去：它让 MF 自动做色彩转换（YUV→RGB32），否则我们得自己接
    // MFVideoFormat_NV12 再写一遍转换。
    // 🔴 别加 MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING 想换缩放：
    //    实测（2026-10-17）只要这个属性在 bag 里，MFCreateSourceReaderFromURL 就
    //    直接返回 E_INVALIDARG（0x80070057），整个视频壁纸打不开。而**基础**视频
    //    处理不支持 scaling —— 设 MF_MT_FRAME_SIZE 会被无声忽略，仍吐原生尺寸。
    //    所以缩放只能在 Qt 侧做（见下面零拷贝 + scaled 的注释）。
    // 🔴 硬件解码（DXVA）2026-10-17 试过、撤了，结论记在这里免得后人再踩：
    //    路子是 D3D11CreateDevice(VIDEO_SUPPORT) + MFCreateDXGIDeviceManager +
    //    attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr)。三步都能过，
    //    但紧接着 MFCreateSourceReaderFromURL 恒返回 E_INVALIDARG(0x80070057)，
    //    MFSTARTUP_LITE 换完整 MFStartup 也一样。本机有 3 个虚拟显示适配器
    //    （ToDesk / GameViewer / AskLink），默认适配器很可能不支持视频解码 ——
    //    真要做，得用 IDXGIFactory 枚举、挑出能 QI 到 ID3D11VideoDevice 的适配器
    //    再建 device，并补上 ID3D10Multithread::SetMultithreadProtected
    //    （MinGW 下那个手写 vtable 调用会崩，得想别的办法拿接口）。
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(MFCreateAttributes(attrs.put(), 2))) {
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        // 🔴 别加 MF_LOW_LATENCY：CPU / 内存数字很漂亮（148%→85%、274MB→198MB），
        //    但实测两种异常 —— 一次进程 32s 后自己退出，一次跑到 2 分钟解码停滞
        //    （CPU 掉到 20%、壁纸定在最后一帧）。壁纸是装饰，不值得拿稳定性换。
    }

    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(
        reinterpret_cast<LPCWSTR>(path.utf16()), attrs.get(), reader.put());
    if (FAILED(hr)) {
        announce(false, {}, QStringLiteral("打不开这个文件（%1）").arg(hresultText(hr)));
        shutdown();
        return;
    }

    // 只要第一条视频流：不这么设，ReadSample 会先吐音频样本。
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // ── 输出类型：顺便让 MF 把帧缩到我们要的尺寸 ───────────────────────────
    //  原先是「全分辨率 RGB32 交出来，再在 Qt 里 scaled()」：1888×1078 一帧 8.1MB，
    //  逐行 memcpy + Qt 再缩一次 = 每帧搬两遍、白烧一个核（2026-10-17 实测
    //  30fps 下整窗 261% CPU）。MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING 打开后
    //  Video Processor MFT 本来就能缩放，直接把目标尺寸写进输出类型即可。
    QSize nativeSize;
    {
        ComPtr<IMFMediaType> native;
        if (SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                                 native.put()))) {
            UINT32 w = 0;
            UINT32 h = 0;
            if (SUCCEEDED(MFGetAttributeSize(native.get(), MF_MT_FRAME_SIZE, &w, &h)))
                nativeSize = QSize(static_cast<int>(w), static_cast<int>(h));
        }
    }
    // 目标尺寸：长边压到 m_maxDimension 以内，宽高都取偶数（RGB32 的 stride 对齐）。
    QSize wantSize = nativeSize;
    if (m_maxDim > 0 && !nativeSize.isEmpty()
        && (nativeSize.width() > m_maxDim || nativeSize.height() > m_maxDim)) {
        const double k = double(m_maxDim) / double(qMax(nativeSize.width(), nativeSize.height()));
        wantSize = QSize(qMax(2, (static_cast<int>(nativeSize.width() * k)) & ~1),
                         qMax(2, (static_cast<int>(nativeSize.height() * k)) & ~1));
    }
    wantW = wantSize.width();
    wantH = wantSize.height();

    const auto makeOutType = [](const QSize& size, ComPtr<IMFMediaType>& type) -> bool {
        if (FAILED(MFCreateMediaType(type.put()))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (!size.isEmpty())
            MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(size.width()),
                               static_cast<UINT32>(size.height()));
        // 🔴 **不要**在这里写 MF_MT_FRAME_RATE 做帧率转换：实测这个壁纸的
        //    Video Processor MFT 不吃（SetCurrentMediaType 直接 E_FAIL，症状是
        //    「系统里没有能解这个编码的解码器」）。帧率节流改在消费侧做 ——
        //    见 MainShell 的 frameReady：距上一帧不足 1/24s 就丢弃不重绘。
        return true;
    };

    ComPtr<IMFMediaType> outType;
    bool typeOk = makeOutType(wantSize, outType)
                  && SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                           nullptr, outType.get()));
    if (!typeOk) {
        // 有的解码器不吃指定尺寸（或 MFT 不支持这个比例），退回「原生尺寸」再试一次：
        // 这时下面 Qt 侧的 scaled() 兜底，功能不受影响，只是多一次搬运。
        typeOk = makeOutType(QSize(), outType)
                 && SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                          nullptr, outType.get()));
    }
    if (!typeOk) {
        announce(false, {}, QStringLiteral("系统里没有能解这个编码的解码器"
                                           "（HEVC/H.265 需装「HEVC 视频扩展」，"
                                           "VP9/AV1 通常不支持）"));
        shutdown();
        return;
    }

    UINT32 width = 0;
    UINT32 height = 0;
    LONG stride = 0;
    {
        ComPtr<IMFMediaType> current;
        if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                  current.put()))) {
            MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &width, &height);
            UINT32 raw = 0;
            // MF_MT_DEFAULT_STRIDE 是「有符号」的：负数表示图像自下而上存放，
            // 直接当无符号用会得到 40 亿这种离谱值。
            if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw)))
                stride = static_cast<LONG>(raw);
        }
    }
    if (width == 0 || height == 0) {
        announce(false, {}, QStringLiteral("读不到视频分辨率（文件可能损坏）"));
        shutdown();
        return;
    }

    announce(true, QSize(static_cast<int>(width), static_cast<int>(height)), {});
    if (m_maxDim > 0 && (static_cast<int>(width) > m_maxDim || static_cast<int>(height) > m_maxDim))
        m_sizeOut = nullptr;   // 已经报过原始尺寸，下面不再用

    // ── 解码循环 ─────────────────────────────────────────────────────────
    // 按样本时间戳节流：直接连着读会跑满一个核（解码永远快过播放）。
    QElapsedTimer clock;
    LONGLONG firstTs = 0;
    bool haveFirstTs = false;
    qint64 lastEmitMs = -1000;   // 上一帧投递时刻（相对 firstTs），跳帧节流用
    const int srcW = static_cast<int>(width);
    const int srcH = static_cast<int>(height);

    while (!stop.load(std::memory_order_relaxed)) {
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex,
                                &flags, &timestamp, sample.put());
        if (FAILED(hr)) {   // 文件损坏 / 设备丢失：静默停在这里，画面停在最后一帧
            std::fprintf(stderr, "[video] ReadSample failed: %s\n",
                         qPrintable(hresultText(hr)));
            break;
        }

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            // 循环播放：回到开头重来。壁纸不循环没有意义。
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = 0;
            reader->SetCurrentPosition(GUID_NULL, pos);
            PropVariantClear(&pos);
            haveFirstTs = false;
            continue;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            ComPtr<IMFMediaType> current;
            if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                      current.put()))) {
                UINT32 raw = 0;
                if (SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw)))
                    stride = static_cast<LONG>(raw);
            }
            continue;   // 分辨率变了：这一帧的缓冲布局对不上，丢掉重新来
        }
        if (!sample) continue;

        if (!haveFirstTs) {
            firstTs = timestamp;
            haveFirstTs = true;
            clock.restart();
        } else {
            // 分小步睡，好让 stop 能及时生效（一次睡 33ms 会让退出卡住）。
            const qint64 targetMs = (timestamp - firstTs) / 10000;
            while (!stop.load(std::memory_order_relaxed) && clock.elapsed() < targetMs)
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            // 跳帧到 kTargetFps：这一帧距上一帧投递不足 1/24s 就整个丢掉 ——
            // 省掉下面的逐行 memcpy + convertToFormat + 跨线程投递。
            // 解码本身（ReadSample）省不掉，但那是 MF 的事，我们不管。
            if (targetMs - lastEmitMs < kFrameIntervalMs) continue;
        }
        if (stop.load(std::memory_order_relaxed)) break;
        lastEmitMs = (timestamp - firstTs) / 10000;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) continue;

        BYTE* data = nullptr;
        DWORD maxLen = 0;
        DWORD curLen = 0;
        if (FAILED(buffer->Lock(&data, &maxLen, &curLen))) continue;

        QImage frame;
        const int absStride = stride != 0 ? std::abs(static_cast<int>(stride)) : srcW * 4;
        const bool needScale = m_maxDim > 0 && (srcW > m_maxDim || srcH > m_maxDim);
        if (stride >= 0 && absStride >= srcW * 4) {
            // ── 零拷贝路径 ─────────────────────────────────────────────────
            //  直接把 MF 的缓冲包成 QImage（不 memcpy），就地缩放。原来那条
            //  「逐行 memcpy 8MB → convertToFormat 再 8MB → scaled 出 2MB」的路，
            //  光搬运就三趟；现在只有「读一遍源 + 写一遍目标」。
            //  1888×1078 的壁纸每帧省 ~16MB 内存带宽，24fps 下就是 400MB/s。
            const QImage wrap(data, srcW, srcH, absStride, QImage::Format_RGB32);
            frame = needScale ? wrap.scaled(m_maxDim, m_maxDim, Qt::KeepAspectRatio,
                                            Qt::FastTransformation)
                              : wrap.copy();
        } else {
            // stride < 0（自下而上存放）或 stride 异常：老实逐行搬。
            QImage raw(srcW, srcH, QImage::Format_RGB32);
            const int dstStride = raw.bytesPerLine();
            const int rowBytes = qMin(absStride, dstStride);
            for (int y = 0; y < srcH; ++y) {
                // stride < 0 → 内存里的第一行是图像的**最后**一行。
                const BYTE* src = (stride < 0)
                                      ? data + static_cast<size_t>(srcH - 1 - y) * absStride
                                      : data + static_cast<size_t>(y) * absStride;
                std::memcpy(raw.scanLine(y), src, static_cast<size_t>(rowBytes));
            }
            frame = needScale ? raw.scaled(m_maxDim, m_maxDim, Qt::KeepAspectRatio,
                                           Qt::FastTransformation)
                              : raw;
        }
        buffer->Unlock();

        // 🔴 必须转成 ARGB32_Premultiplied，不能直接把 Format_RGB32 交出去。
        //   MF 的 RGB32 输出第 4 个字节（alpha）**通常是 0**；QImage 的 RGB32 语义上
        //   虽然「恒不透明」，但 Qt 的光栅引擎在 blit 时仍会读这个字节 —— 于是整帧
        //   被当成全透明画出去。症状极具欺骗性：解码日志、帧尺寸、pixelColor() 全都
        //   正常（RGB32 的 pixelColor 会强补 alpha=255），窗口上却什么都没有。
        //   2026-10-17 用「同一次 paintEvent 里 fillRect 有色块、drawImage 无内容」
        //   的对照实验定位到这一步。
        //   顺带这也是 QPainter 的最快绘制格式。
        frame = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        // 首帧打一行：区分「打开了但一帧都吐不出来」和「根本没打开」。
        static bool printedFirst = false;
        if (!printedFirst) {
            printedFirst = true;
            std::fprintf(stderr, "[video] first frame %dx%d (native %dx%d)\n", srcW, srcH,
                         static_cast<int>(width), static_cast<int>(height));
        }

        emit frameReady(frame);
    }

    reader.reset();
    shutdown();
}

#else // !Q_OS_WIN

void VideoWorker::run(const QString&)
{
    if (m_ready) m_ready->release();
}

#endif

// ─────────────────────────────────────────────────────────────────────────────
//  VideoBackground
// ─────────────────────────────────────────────────────────────────────────────

VideoBackground::VideoBackground(QObject* parent)
    : QObject(parent)
{
}

VideoBackground::~VideoBackground()
{
    close();
}

bool VideoBackground::looksLikeVideo(const QString& path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty()) return false;
    for (const char* ext : kVideoExts) {
        if (suffix == QLatin1String(ext + 1)) return true;   // +1 跳过前导点
    }
    return false;
}

QString VideoBackground::dialogFilter()
{
    return QStringLiteral("视频文件 (*.mp4 *.m4v *.mov *.webm *.mkv *.avi *.wmv);;"
                          "图片文件 (*.jpg *.jpeg *.png *.bmp *.gif *.webp);;"
                          "所有文件 (*)");
}

void VideoBackground::setMaxDimension(int pixels)
{
    m_maxDimension = qMax(0, pixels);
}

bool VideoBackground::open(const QString& path, QString* reason)
{
    close();
    if (reason) reason->clear();

    if (path.isEmpty() || !QFileInfo::exists(path)) {
        if (reason) *reason = QStringLiteral("文件不存在");
        return false;
    }

    QSize sizeOut;
    QString errorOut;
    QSemaphore ready;

    m_thread = new QThread(this);
    m_worker = new VideoWorker(&sizeOut, &errorOut, &ready, m_maxDimension);
    m_worker->moveToThread(m_thread);

    // 工作线程一启动就跑 run()。run() 会阻塞该线程的事件循环，直到 stop 置真。
    connect(m_thread, &QThread::started, m_worker,
            [this, path] { m_worker->run(path); });
    connect(m_worker, &VideoWorker::frameReady, this, &VideoBackground::frameReady,
            Qt::QueuedConnection);
    m_thread->start();

    // 等探测结果：拿不到分辨率就不算打开成功，调用方要能立刻决定回落到静态图。
    // 5 秒对本地文件绰绰有余；真超时说明解码器卡死了，早点放弃更好。
    if (!ready.tryAcquire(1, 5000)) {
        close();
        if (reason) *reason = QStringLiteral("打开超时");
        return false;
    }
    if (!errorOut.isEmpty()) {
        close();
        if (reason) *reason = errorOut;
        return false;
    }

    m_frameSize = sizeOut;
    m_open = true;
    return true;
}

void VideoBackground::close()
{
    if (m_worker) {
        m_worker->stop.store(true, std::memory_order_relaxed);
    }
    if (m_thread) {
        m_thread->quit();
        // run() 阻塞着事件循环，quit() 送不进去；真正的退出靠 stop 旗标，
        // wait() 等它自己跑完收尾（MFShutdown 也在那边）。
        if (!m_thread->wait(5000)) {
            m_thread->terminate();
            m_thread->wait(1000);
        }
    }
    delete m_worker;   // 线程已停，此刻删是安全的
    m_worker = nullptr;
    if (m_thread) {
        delete m_thread;
        m_thread = nullptr;
    }
    m_open = false;
    m_frameSize = QSize();
}

} // namespace kc::media

#include "VideoBackground.moc"
