using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Controls
{
    public partial class DynamicBackground : UserControl
    {
        private Storyboard? _currentStoryboard;
        private bool _isVideoMode;

        /// <summary>
        /// 背景的「真实模糊」效果（玻璃的磨砂底）。整控件只挂这一支，
        /// 挂/摘由 <see cref="SyncBackdropBlur"/> 按媒体类型与 GPU 能力统一决定。
        /// </summary>
        private readonly BlurEffect _backdropBlur = new BlurEffect { Radius = 24 };

        /// <summary>
        /// 动图（GIF）模式。WPF 的 <see cref="Image"/> **不会**自己播放 GIF（只显示第一帧），
        /// 这里用 <see cref="GifBitmapDecoder"/> 把帧全解出来，再用 DispatcherTimer 按各帧自己的
        /// 延时轮播 —— 不引入第三方依赖（如 WpfAnimatedGif），也便于和「显示方式/动画效果」共存。
        /// Wallpaper Engine 的 scene / web 类壁纸正是靠官方 <c>preview.gif</c> 走这条路径。
        /// </summary>
        private bool _isGifMode;
        private DispatcherTimer? _gifTimer;
        private readonly List<BitmapSource> _gifFrames = new();
        private readonly List<int> _gifDelaysMs = new();
        private int _gifIndex;

        /// <summary>
        /// 换帧的最小间隔（毫秒）= 25fps 上限。
        /// GIF 的帧延时可以低到 10ms（=100fps），而每换一帧都是**全窗口重绘**——
        /// 一个快 GIF 就能把 CPU 吃满。低于这个间隔的帧直接丢掉（不放慢整体速度，见 GifTimer_Tick）。
        /// </summary>
        private const int MinGifFrameMs = 40;

        /// <summary>播放起点（TickCount64）。按真实时间定位该显示第几帧，帧密时自动丢帧。</summary>
        private long _gifStartMs;
        /// <summary>暂停时刻；恢复时把起点顺延同样的时长，否则一恢复就跳帧。</summary>
        private long _gifPausedAtMs;

        /// <summary>当前已下发的媒体路径；用于避免对同一个文件重复设置 Source。</summary>
        private string? _currentMediaPath;
        /// <summary>媒体是否已成功打开（视频看 MediaOpened，图片看 Image.Source 是否就绪）。</summary>
        private bool _mediaReady;
        /// <summary>视频打开失败后的重试次数（只重试一次，避免异常文件导致死循环）。</summary>
        private int _videoRetry;
        /// <summary>OnLoaded 只做一次初始化，防止后续可见性变化触发重复加载。</summary>
        private bool _initialized;

        /// <summary>视频应显示的透明度（跟随设置里的「不透明度」，不再在 MediaOpened 里写死 1）。</summary>
        private double _videoOpacity = 1.0;

        /// <summary>
        /// 视频看门狗。WPF 的 MediaElement 在 LoadedBehavior=Manual 下**不会自己打开媒体**，
        /// 必须由我们主动 Play；而原实现把 Play 放在 MediaOpened 里，MediaOpened 又要等媒体打开
        /// 之后才触发 —— 两边互相等待，视频永远不播（实测画面渲染非黑占比 0.0%）。
        /// 这里定时核对播放位置，一旦发现停滞就再催一次 Play，兜住慢开/卡顿/被中断的情况。
        /// </summary>
        private readonly DispatcherTimer _videoWatchdog;
        private TimeSpan _lastVideoPosition = TimeSpan.MinValue;
        private int _stalledTicks;

        public DynamicBackground()
        {
            InitializeComponent();
            Loaded += OnLoaded;

            _videoWatchdog = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _videoWatchdog.Tick += VideoWatchdog_Tick;

            _gifTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            _gifTimer.Tick += GifTimer_Tick;
        }

        private void OnLoaded(object sender, RoutedEventArgs e)
        {
            if (_initialized) return;
            _initialized = true;

            // 这里是唯一的"首次加载"入口。MainWindow 构造阶段只负责设置可见性，
            // 把真正的媒体加载推迟到控件进入可视树之后（见 MainWindow.ApplyDynamicBackground）。
            var settings = AppSettings.Current;
            var path = settings.ResolveBackgroundPath();
            if (string.IsNullOrEmpty(path) || !File.Exists(path)) return;

            Visibility = Visibility.Visible;
            LoadMedia(path);
            ApplyAnimation(settings.BackgroundAnimation, settings.BackgroundOpacity);
        }

        private static bool IsVideoFile(string path)
        {
            var ext = Path.GetExtension(path).ToLowerInvariant();
            // .webm 是 Wallpaper Engine 视频壁纸的常用容器，必须一起认
            return ext is ".mp4" or ".webm" or ".avi" or ".mkv" or ".mov" or ".wmv";
        }

        /// <summary>动图（逐帧播放）。Wallpaper Engine 的场景/网页壁纸预览就是 .gif。</summary>
        private static bool IsGifFile(string path)
            => Path.GetExtension(path).ToLowerInvariant() is ".gif";

        /// <summary>
        /// 加载背景媒体。对同一路径是幂等的：重复调用不会再次设置 MediaElement.Source，
        /// 否则 WPF 会在媒体尚未打开时重启一次加载管线，偶发 MediaFailed 导致背景整块消失
        /// （这正是"自定义背景重启后不见"的诱因之一）。
        /// </summary>
        public void LoadMedia(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                ApplyDefaultGradient();
                return;
            }

            if (_mediaReady && string.Equals(_currentMediaPath, path, StringComparison.OrdinalIgnoreCase))
                return;

            _currentMediaPath = path;
            _mediaReady = false;

            try
            {
                if (IsVideoFile(path)) LoadVideo(path);
                else if (IsGifFile(path)) LoadAnimatedGif(path);
                else LoadImage(path);
            }
            catch
            {
                ApplyDefaultGradient();
            }
        }

        public void LoadVideo(string path)
        {
            try
            {
                _isVideoMode = true;
                _isGifMode = false;
                StopGif();
                BgImage.Opacity = 0;
                _videoOpacity = AppSettings.Current.BackgroundOpacity;
                _stalledTicks = 0;
                _lastVideoPosition = TimeSpan.MinValue;

                BgVideo.Source = new Uri(path, UriKind.Absolute);

                // ★ 关键：设完 Source 立刻 Play，不要等 MediaOpened。
                //   见 _videoWatchdog 注释：等 MediaOpened 才 Play 是死锁，视频永远不动。
                BgVideo.Position = TimeSpan.Zero;
                BgVideo.Opacity = _videoOpacity;
                BgVideo.Play();
                StartVideoWatchdog();
            }
            catch
            {
                ApplyDefaultGradient();
            }
            // 视频模式不模糊（见 SyncBackdropBlur），但要确保旧图片上的效果被摘掉
            SyncScalingMode();
        }

        private void StartVideoWatchdog()
        {
            _lastVideoPosition = TimeSpan.MinValue;
            _stalledTicks = 0;
            if (!_videoWatchdog.IsEnabled) _videoWatchdog.Start();
        }

        private void StopVideoWatchdog()
        {
            if (_videoWatchdog.IsEnabled) _videoWatchdog.Stop();
            _lastVideoPosition = TimeSpan.MinValue;
            _stalledTicks = 0;
        }

        private void VideoWatchdog_Tick(object? sender, EventArgs e)
        {
            if (!_isVideoMode || BgVideo.Source == null)
            {
                StopVideoWatchdog();
                return;
            }

            var pos = BgVideo.Position;
            if (pos != _lastVideoPosition)
            {
                // 位置在走 = 正常播放，清零停滞计数
                _lastVideoPosition = pos;
                _stalledTicks = 0;
                return;
            }

            // 位置没动：连续两次没动才催，避免刚 seek 完就误判
            if (++_stalledTicks < 2) return;
            _stalledTicks = 0;
            try
            {
                if (BgVideo.Opacity <= 0.001) BgVideo.Opacity = _videoOpacity;
                BgVideo.Play();
            }
            catch { }
        }

        public void LoadImage(string path)
        {
            try
            {
                _isVideoMode = false;
                _isGifMode = false;
                StopGif();
                StopVideoWatchdog();
                BgVideo.Stop();
                BgVideo.Opacity = 0;

                var bitmap = new BitmapImage();
                bitmap.BeginInit();
                bitmap.UriSource = new Uri(path, UriKind.Absolute);
                bitmap.CacheOption = BitmapCacheOption.OnLoad;
                bitmap.EndInit();
                bitmap.Freeze();
                BgImage.Source = bitmap;
                _mediaReady = true;
            }
            catch
            {
                ApplyDefaultGradient();
            }
            // 图片模式是唯一会挂「真实模糊」的路径，落定后同步一次
            // （ApplyAnimation 随后还会再同步，这里保证只调 LoadMedia 时也不错位）
            SyncScalingMode();
        }

        /// <summary>
        /// 加载动图（GIF）并逐帧播放。
        /// <para>
        /// WPF 的 <see cref="Image"/> 只显示 GIF 第一帧，所以这里用
        /// <see cref="GifBitmapDecoder"/> 把所有帧解出来（<c>BitmapCacheOption.OnLoad</c> 让文件可以
        /// 立即释放、不被锁住），再按每帧自己的 <c>/grctlext/Delay</c>（单位 1/100 秒）驱动
        /// DispatcherTimer 轮播。单帧 GIF 直接当静态图处理。
        /// </para>
        /// </summary>
        public void LoadAnimatedGif(string path)
        {
            try
            {
                _isVideoMode = false;
                StopVideoWatchdog();
                BgVideo.Stop();
                BgVideo.Opacity = 0;
                StopGif();
                _gifFrames.Clear();
                _gifDelaysMs.Clear();

                GifBitmapDecoder decoder;
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    decoder = new GifBitmapDecoder(fs, BitmapCreateOptions.PreservePixelFormat,
                                                   BitmapCacheOption.OnLoad);
                    foreach (var frame in decoder.Frames)
                    {
                        var frozen = BitmapFrame.Create(frame);
                        frozen.Freeze();
                        _gifFrames.Add(frozen);
                        _gifDelaysMs.Add(ReadGifDelayMs(frame));
                    }
                }

                if (_gifFrames.Count <= 1)
                {
                    // 单帧 / 解码不出帧：退回静态图，别让计时器空转
                    LoadImage(path);
                    return;
                }

                _isGifMode = true;
                _gifIndex = 0;
                BgImage.Source = _gifFrames[0];
                _mediaReady = true;
                _gifStartMs = Environment.TickCount64;
                _gifTimer!.Interval = TimeSpan.FromMilliseconds(Math.Max(_gifDelaysMs[0], MinGifFrameMs));
                _gifTimer.Start();
                SyncScalingMode();
            }
            catch
            {
                ApplyDefaultGradient();
            }
        }

        /// <summary>取 GIF 帧延时（毫秒）。查不到或为 0 时按 100ms 兜底（多数 GIF 的默认值）。</summary>
        private static int ReadGifDelayMs(BitmapFrame frame)
        {
            try
            {
                if (frame.Metadata is BitmapMetadata meta)
                {
                    var raw = meta.GetQuery("/grctlext/Delay");
                    var cs = raw switch
                    {
                        ushort u => (int)u,
                        int i => i,
                        short s => (int)s,
                        byte b => (int)b,
                        _ => 0,
                    };
                    if (cs > 0) return cs * 10;   // 1/100 秒 → 毫秒
                }
            }
            catch { }
            return 100;
        }

        private void GifTimer_Tick(object? sender, EventArgs e)
        {
            if (!_isGifMode || _gifFrames.Count == 0)
            {
                StopGif();
                return;
            }

            // 按 GIF 自带的帧间隔算出「此刻应该显示第几帧」：
            // 时间轴用的是**原始**延时，所以动画速度与 GIF 一致；间隔太密的帧在 25fps 的
            // 醒来节奏下会自然被跳过 = 丢帧，既不放慢也不再逐帧全窗重绘。
            var total = 0L;
            for (var i = 0; i < _gifDelaysMs.Count; i++) total += _gifDelaysMs[i];
            if (total <= 0)
            {
                StopGif();
                return;
            }

            var elapsed = Environment.TickCount64 - _gifStartMs;
            if (elapsed < 0) elapsed = 0;
            var t = elapsed % total;

            var acc = 0L;
            var idx = 0;
            for (var i = 0; i < _gifDelaysMs.Count; i++)
            {
                acc += _gifDelaysMs[i];
                if (t < acc)
                {
                    idx = i;
                    break;
                }
            }

            if (idx != _gifIndex)
            {
                _gifIndex = idx;
                BgImage.Source = _gifFrames[idx];   // 只有真的换帧才触碰 Source
            }

            // 下次醒来 = 到下一帧的到期时刻；下限 40ms（丢帧节奏），上限 1s（防呆）
            var nextIn = acc - t;
            if (nextIn < MinGifFrameMs) nextIn = MinGifFrameMs;
            if (nextIn > 1000) nextIn = 1000;
            _gifTimer!.Interval = TimeSpan.FromMilliseconds(nextIn);
        }

        private void StopGif()
        {
            _gifTimer?.Stop();
            _gifIndex = 0;
            _gifStartMs = 0;
            _gifPausedAtMs = 0;
            SyncScalingMode();
        }

        private bool _paused;

        /// <summary>
        /// 暂停 / 恢复背景播放。**窗口最小化时必须停。**
        /// <para>
        /// GIF 背景是全窗口逐帧换图，而 <see cref="DispatcherTimer"/> 跑在 UI 线程上 ——
        /// 窗口最小化后它仍按 GIF 自带的帧间隔（常见 100ms，约 10fps）继续重绘整窗，
        /// 纯烧 CPU 且让最小化后的机器发烫。视频背景同理（MediaElement 也会继续解码）。
        /// </para>
        /// <para>
        /// 幂等：重复传同一个状态直接返回，避免把用户手动暂停的状态又打开。
        /// </para>
        /// </summary>
        public void SetPlaying(bool playing)
        {
            if (_paused == !playing) return;
            _paused = !playing;

            if (_paused)
            {
                _gifPausedAtMs = Environment.TickCount64;   // 恢复时把起点顺延这段时长
                _gifTimer?.Stop();
                if (_mediaReady) BgVideo.Pause();
                SyncScalingMode();
                return;
            }

            if (_gifFrames.Count > 0)
            {
                if (_gifPausedAtMs != 0)
                {
                    _gifStartMs += Environment.TickCount64 - _gifPausedAtMs;
                    _gifPausedAtMs = 0;
                }
                _gifTimer?.Start();
            }
            else if (_mediaReady) BgVideo.Play();
            SyncScalingMode();
        }

        private void BgVideo_MediaOpened(object sender, RoutedEventArgs e)
        {
            _mediaReady = true;
            _videoRetry = 0;
            // 透明度取设置值，不再写死 1（否则用户把「不透明度」调到很低也会被强行拉满）
            BgVideo.Opacity = _videoOpacity;
            // 这里**不要**再 Position=0：媒体已经由 LoadVideo 主动 Play 起来了，
            // MediaOpened 又是延迟到达的，此时 seek 回 0 会把正在播放的视频打断，
            // 实测表现为「时钟跳到 0 之后再也不动」。
            BgVideo.Play();
            StartVideoWatchdog();
        }

        private void BgVideo_MediaEnded(object sender, RoutedEventArgs e)
        {
            // 循环播放：回到开头重播，并把看门狗重置，避免 seek 后 Play 没生效就卡死
            try
            {
                BgVideo.Position = TimeSpan.Zero;
                BgVideo.Play();
            }
            catch { }
            StartVideoWatchdog();
        }

        private void BgVideo_MediaFailed(object sender, ExceptionRoutedEventArgs e)
        {
            _mediaReady = false;
            BgVideo.Opacity = 0;
            StopVideoWatchdog();

            // 首次失败通常是加载管线竞争导致的瞬时问题：清空 Source 后重投一次；
            // 仍旧失败才退回默认渐变，避免在异常文件上反复重试。
            var path = _currentMediaPath;
            if (_videoRetry++ < 1 && !string.IsNullOrEmpty(path) && File.Exists(path))
            {
                Dispatcher.BeginInvoke(new Action(() =>
                {
                    try
                    {
                        BgVideo.Source = null;
                        BgVideo.Source = new Uri(path!, UriKind.Absolute);
                        // 重投之后同样要主动 Play，不能等 MediaOpened（见看门狗注释）
                        BgVideo.Position = TimeSpan.Zero;
                        BgVideo.Opacity = _videoOpacity;
                        BgVideo.Play();
                        StartVideoWatchdog();
                    }
                    catch
                    {
                        ApplyDefaultGradient();
                    }
                }), System.Windows.Threading.DispatcherPriority.Background);
                return;
            }

            ApplyDefaultGradient();
        }

        public void ApplyAnimation(string animationType, double opacity)
        {
            _currentStoryboard?.Stop(this);
            _currentStoryboard = null;

            // 收尾统一同步一次缩放滤镜（见 SyncScalingMode）：跑没跑补间在方法末尾是确定的，
            // 不必在视频 / 无背景 / 非裁剪铺满那几个早退分支里各写一遍。
            try
            {
                ApplyAnimationCore(animationType, opacity);
            }
            finally
            {
                SyncScalingMode();
            }
        }

        private void ApplyAnimationCore(string animationType, double opacity)
        {
            // 「显示方式」要在任何早退分支之前同步，否则视频模式/无背景时会漏掉
            ApplyFitStretch();
            bool allowTransformAnim = BgImage.Stretch == Stretch.UniformToFill;

            if (_isVideoMode)
            {
                _videoOpacity = opacity;
                BgVideo.Opacity = opacity;
                BgImage.Opacity = 0;
                // 视频模式下保证播放管线是活的（切主题/改设置都会重走这里）
                if (BgVideo.Source != null)
                {
                    try { BgVideo.Play(); } catch { }
                    StartVideoWatchdog();
                }
                return;
            }

            BgImage.Opacity = opacity;
            BgScale.ScaleX = 1.0;
            BgScale.ScaleY = 1.0;
            BgTranslate.X = 0;
            BgTranslate.Y = 0;

            if (BgImage.Source == null)
            {
                // 没有壁纸 ≠ 不要衬底：这里必须回落到环境色晕。
                // 原来写 Brushes.Transparent 会把 ApplyDefaultGradient 刚铺好的环境底色**擦掉**
                // （MainWindow 无壁纸分支正是 StopAnimation + ApplyAnimation("None",0) 这条路径），
                // 于是界面永远是一块纯白 —— 玻璃看起来「没实现」的另一半原因。
                TintLayer.Background = AmbientBloom;
                return;
            }

            // 非「裁剪铺满」时画面是按窗口完整摆放的，再叠加缩放/平移会重新裁掉画面
            // 或者露出一圈空底，所以这几种方式下不跑任何 RenderTransform 动画
            if (!allowTransformAnim)
            {
                TintLayer.Background = Brushes.Transparent;
                return;
            }

            switch (animationType)
            {
                case "Zoom":
                    ApplyZoomAnimation();
                    break;
                case "Pan":
                    ApplyPanAnimation();
                    break;
                case "KenBurns":
                default:
                    ApplyKenBurnsAnimation();
                    break;
            }
        }

        /// <summary>
        /// 应用「背景显示方式」。必须同时作用于视频与图片两套元素，
        /// 否则换背景（图↔视频）后设置会被 XAML 里的初始值覆盖回去。
        /// </summary>
        private void ApplyFitStretch()
        {
            BgVideo.Stretch = ParseStretch(AppSettings.Current.BackgroundFit);
            BgImage.Stretch = BgVideo.Stretch;
        }

        private static Stretch ParseStretch(string? fit) => fit switch
        {
            "Uniform" => Stretch.Uniform,   // 完整显示（可能留空）
            "Stretch" => Stretch.Fill,      // 拉伸填满（可能变形）
            _ => Stretch.UniformToFill,     // 裁剪铺满（默认）
        };

        public void SetOpacity(double opacity)
        {
            if (_isVideoMode)
            {
                _videoOpacity = opacity;
                BgVideo.Opacity = opacity;
            }
            else
            {
                BgImage.Opacity = opacity;
            }
        }

        private void ApplyKenBurnsAnimation()
        {
            var sb = new Storyboard { RepeatBehavior = RepeatBehavior.Forever, AutoReverse = true };

            var scaleAnim = new DoubleAnimation(1.0, 1.15, TimeSpan.FromSeconds(20))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(scaleAnim, BgImage);
            Storyboard.SetTargetProperty(scaleAnim, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[0].(ScaleTransform.ScaleX)"));
            sb.Children.Add(scaleAnim);

            var scaleAnimY = new DoubleAnimation(1.0, 1.15, TimeSpan.FromSeconds(20))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(scaleAnimY, BgImage);
            Storyboard.SetTargetProperty(scaleAnimY, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[0].(ScaleTransform.ScaleY)"));
            sb.Children.Add(scaleAnimY);

            var translateAnimX = new DoubleAnimation(0, 30, TimeSpan.FromSeconds(20))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(translateAnimX, BgImage);
            Storyboard.SetTargetProperty(translateAnimX, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[1].(TranslateTransform.X)"));
            sb.Children.Add(translateAnimX);

            var translateAnimY = new DoubleAnimation(0, 15, TimeSpan.FromSeconds(20))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(translateAnimY, BgImage);
            Storyboard.SetTargetProperty(translateAnimY, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[1].(TranslateTransform.Y)"));
            sb.Children.Add(translateAnimY);

            _currentStoryboard = sb;
            sb.Begin(this, true);
        }

        private void ApplyZoomAnimation()
        {
            var sb = new Storyboard { RepeatBehavior = RepeatBehavior.Forever, AutoReverse = true };

            var scaleX = new DoubleAnimation(1.0, 1.2, TimeSpan.FromSeconds(15))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(scaleX, BgImage);
            Storyboard.SetTargetProperty(scaleX, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[0].(ScaleTransform.ScaleX)"));
            sb.Children.Add(scaleX);

            var scaleY = new DoubleAnimation(1.0, 1.2, TimeSpan.FromSeconds(15))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(scaleY, BgImage);
            Storyboard.SetTargetProperty(scaleY, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[0].(ScaleTransform.ScaleY)"));
            sb.Children.Add(scaleY);

            _currentStoryboard = sb;
            sb.Begin(this, true);
        }

        private void ApplyPanAnimation()
        {
            var sb = new Storyboard { RepeatBehavior = RepeatBehavior.Forever, AutoReverse = true };

            var translateX = new DoubleAnimation(-20, 20, TimeSpan.FromSeconds(25))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(translateX, BgImage);
            Storyboard.SetTargetProperty(translateX, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[1].(TranslateTransform.X)"));
            sb.Children.Add(translateX);

            var translateY = new DoubleAnimation(-10, 10, TimeSpan.FromSeconds(25))
            {
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            };
            Storyboard.SetTarget(translateY, BgImage);
            Storyboard.SetTargetProperty(translateY, new PropertyPath("(UIElement.RenderTransform).(TransformGroup.Children)[1].(TranslateTransform.Y)"));
            sb.Children.Add(translateY);

            _currentStoryboard = sb;
            sb.Begin(this, true);
        }

        private void ApplyDefaultGradient()
        {
            StopVideoWatchdog();
            _isGifMode = false;
            StopGif();
            BgImage.Source = null;
            // 🔴 2026-10-08（iOS 液态玻璃）：这里原来是一支 **Opacity=0.08** 的对角双色线性渐变
            //    —— 实测肉眼几乎看不出来，等于没画。可它是**玻璃唯一的衬底**：
            //    没设壁纸时窗底是纯白，半透明面板叠在白底上，磨砂/镜面高光/外阴影三样全读不出来。
            //    这正是用户反复说「液态玻璃没实现」的最后一环（不是玻璃没做，是背后没东西）。
            //    现在换成 4 团柔和径向色晕 —— 像 iOS 壁纸那样给玻璃提供可透、可衬的内容。
            TintLayer.Background = AmbientBloom;
            // 没有壁纸 = 没有可模糊的东西，把可能还挂着的效果摘掉
            SyncScalingMode();
        }

        /// <summary>
        /// 无壁纸时的「环境底色」（iOS 液态玻璃衬底）。四团柔和径向色晕：
        /// 左上天空蓝为主（和强调色同源）+ 右上浅蓝 + 底部冷灰紫 + 左下一点青。
        /// <para>⚠️ 透明度的账是**链式**的：页面根用 Theme.BgTint（半透明白）压在这层上，
        /// 所以这里的色晕 alpha 不能太小 —— 被 BgTint 稀释后才是肉眼看到的那点颜色。
        /// 改这个值之前在预览探针（glass_probe preview）里看一眼再说。</para>
        /// <para>公开出来是为了让预览探针能用**同一支画刷**做观感预览，不要在别处另画一份。</para>
        /// </summary>
        public static Brush BuildAmbientBloom()
        {
            if (_ambientBloom != null) return _ambientBloom;

            var group = new DrawingGroup();
            void Bloom(double cx, double cy, double r, byte alpha, Color color)
            {
                var rg = new RadialGradientBrush
                {
                    MappingMode = BrushMappingMode.RelativeToBoundingBox,
                    Center = new Point(0.5, 0.5),
                    GradientOrigin = new Point(0.5, 0.5),
                    RadiusX = 0.5,
                    RadiusY = 0.5,
                };
                rg.GradientStops.Add(new GradientStop(Color.FromArgb(alpha, color.R, color.G, color.B), 0));
                rg.GradientStops.Add(new GradientStop(Color.FromArgb(0, color.R, color.G, color.B), 1));
                group.Children.Add(new GeometryDrawing(rg, null, new EllipseGeometry(new Point(cx, cy), r, r)));
            }

            Bloom(0.10, 0.00, 0.78, 0xE0, Color.FromRgb(0x7A, 0xB8, 0xF3));   // 天空蓝（左上，主色）
            Bloom(0.98, 0.14, 0.60, 0xA6, Color.FromRgb(0xA9, 0xD2, 0xF8));   // 浅蓝（右上）
            Bloom(0.62, 1.04, 0.90, 0x94, Color.FromRgb(0xB9, 0xC4, 0xE8));   // 冷灰紫（底部）
            Bloom(0.26, 0.78, 0.52, 0x7A, Color.FromRgb(0x8F, 0xD8, 0xD0));   // 一点青（左下）

            var brush = new DrawingBrush(group)
            {
                Stretch = Stretch.Fill,
                ViewboxUnits = BrushMappingMode.Absolute,
                Viewbox = new Rect(0, 0, 1, 1),
                ViewportUnits = BrushMappingMode.RelativeToBoundingBox,
                Viewport = new Rect(0, 0, 1, 1),
                TileMode = TileMode.None,
            };
            brush.Freeze();
            _ambientBloom = brush;
            return brush;
        }

        private static Brush? _ambientBloom;

        private static Brush AmbientBloom => BuildAmbientBloom();

        public void StopAnimation()
        {
            _currentStoryboard?.Stop(this);
            _currentStoryboard = null;
            StopVideoWatchdog();
            _isGifMode = false;
            StopGif();
            BgVideo.Stop();
            SyncScalingMode();
        }

        /// <summary>
        /// 按当前状态决定 BgImage 的缩放滤镜：**静止用 HighQuality（Fant），动起来用 Linear（双线性）**。
        ///
        /// XAML 上写死 HighQuality 是因为静态壁纸被拉伸到窗口尺寸时双线性会发虚（用户反馈的
        /// 「背景很模糊」）；但 KenBurns / Zoom / Pan 是每帧都在改 ScaleX/ScaleY、GIF 是每帧换 Source，
        /// 每一帧都要把整张图重新过一遍 Fant 滤镜 —— 4K 图 20 秒来回补间就是持续的高开销。
        /// 动的时候人眼分辨不出 Fant 和双线性的差别，停下来再切回 HighQuality 保证静态锐度。
        /// </summary>
        private void SyncScalingMode()
        {
            var moving = _currentStoryboard != null
                         || (_isGifMode && !_paused && _gifTimer is { IsEnabled: true });
            RenderOptions.SetBitmapScalingMode(BgImage,
                moving ? BitmapScalingMode.Linear : BitmapScalingMode.HighQuality);
            SyncBackdropBlur();
        }

        /// <summary>
        /// 挂/摘「真实背景模糊」—— 玻璃面板的磨砂底就来自它：
        /// 壁纸先被模糊，再透过半透明的面板（Theme.Surface / CardBg …）露出来。
        ///
        /// 三个自动降级条件（都不需要用户操作）：
        ///  ① 视频 / GIF 每帧都在换内容，逐帧重模糊是纯浪费 —— 退回不模糊的磨砂；
        ///  ② 软件渲染（无 GPU / Tier &lt; 2）不挂效果 —— WPF 会退到 CPU 逐像素卷积，
        ///     那正是刚清掉的那类掉帧来源；
        ///  ③ 半径 BackgroundBlur = 0 表示用户主动关掉。
        /// </summary>
        private void SyncBackdropBlur()
        {
            var radius = Math.Clamp(AppSettings.Current.BackgroundBlur, 0.0, 60.0);
            var enable = radius > 0
                         && !_isVideoMode
                         && !_isGifMode
                         && BgImage.Source != null
                         // ⚠️ RenderCapability.Tier 返回的是**左移 16 位**的原始值
                         // （本机实测 131072 = 0x20000 = 档位 2），直接拿它和 2 比大小
                         // 会把「有限硬件加速」的档位 1 也当成满血放行 —— 那档的像素着色器
                         // 可能落到 CPU 上，正是要避开的掉帧来源。必须右移后再比。
                         && (RenderCapability.Tier >> 16) >= 2;

            if (!enable)
            {
                BgImage.Effect = null;
                return;
            }

            if (Math.Abs(_backdropBlur.Radius - radius) > 0.5) _backdropBlur.Radius = radius;
            if (!ReferenceEquals(BgImage.Effect, _backdropBlur)) BgImage.Effect = _backdropBlur;
        }
    }
}
