using System;
using System.IO;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using KunFlashTool.App.Services;
using Microsoft.Win32;

namespace KunFlashTool.App.Pages
{
    public partial class SettingsPage : BasePage
    {
        private bool _isInitializing = true;
        private readonly AppSettings _settings;

        public SettingsPage()
        {
            InitializeComponent();
            _settings = AppSettings.Current;
            LoadBackgroundSettings();
            _isInitializing = false;
            UpdateLegalState();
            Loaded += async (_, _) => await LoadProfileAsync();
        }

        private void LoadBackgroundSettings()
        {
            // 配置里的路径可能因文件被改名/移动而失效；ResolveBackgroundPath 会尝试自愈，
            // 自愈失败时必须在界面上说清楚，否则用户只看到"背景莫名其妙没了"。
            var bgPath = _settings.ResolveBackgroundPath();

            if (!string.IsNullOrEmpty(bgPath) && File.Exists(bgPath))
            {
                try
                {
                    if (IsVideoFile(bgPath))
                    {
                        PreviewImage.Source = null;
                        PreviewHint.Visibility = Visibility.Collapsed;
                    }
                    else
                    {
                        var bitmap = new BitmapImage();
                        bitmap.BeginInit();
                        bitmap.UriSource = new Uri(bgPath, UriKind.Absolute);
                        bitmap.CacheOption = BitmapCacheOption.OnLoad;
                        bitmap.EndInit();
                        bitmap.Freeze();
                        PreviewImage.Source = bitmap;
                        PreviewHint.Visibility = Visibility.Collapsed;
                    }
                    TxtImagePath.Text = Path.GetFileName(bgPath);
                    TxtImagePath.Foreground = (Brush)FindResource("Theme.TextSecondary");

                    // 兜底提醒：如果**当前正在用的**背景本身是 WE 官方预览素材（低清），
                    // 就把原因写在预览框里。用户看到的「背景糊/比例不对」十有八九是它 ——
                    // 光靠"能加载出来"是发现不了的（它能加载，只是只有 256~512px 宽）。
                    var resolvedItem = WallpaperEngineService.ResolveOne(bgPath);
                    if (resolvedItem?.FromPreview == true)
                    {
                        PreviewHint.Text = "低清预览素材（WE 官方预览图，约 256~512px）\n"
                                         + "铺满窗口会发虚 —— 建议换一张视频类壁纸";
                        PreviewHint.Visibility = Visibility.Visible;
                    }
                }
                catch
                {
                    TxtImagePath.Text = "文件无法加载";
                }
            }
            else if (_settings.BackgroundMissing)
            {
                PreviewImage.Source = null;
                PreviewHint.Text = "背景文件已丢失";
                PreviewHint.Visibility = Visibility.Visible;
                TxtImagePath.Text = "背景文件已被移动或重命名，请重新选择";
                TxtImagePath.Foreground = (Brush)FindResource("Theme.Danger");
            }
            else
            {
                PreviewImage.Source = null;
                PreviewHint.Text = "未选择背景图片";
                PreviewHint.Visibility = Visibility.Visible;
                TxtImagePath.Text = "未选择";
                TxtImagePath.Foreground = (Brush)FindResource("Theme.TextSecondary");
            }

            // 预览透明度跟随设置值（旧实现把 Opacity 写死在 XAML 里为 0.35，
            // 拖动"不透明度"滑杆时预览毫无变化，看起来就是"透明度不能自定义"）
            PreviewImage.Opacity = Math.Clamp(_settings.BackgroundOpacity, 0.0, 1.0);

            foreach (ComboBoxItem item in CmbAnimation.Items)
            {
                if (item.Tag?.ToString() == _settings.BackgroundAnimation)
                {
                    CmbAnimation.SelectedItem = item;
                    break;
                }
            }

            foreach (ComboBoxItem item in CmbBackgroundFit.Items)
            {
                if (item.Tag?.ToString() == _settings.BackgroundFit)
                {
                    CmbBackgroundFit.SelectedItem = item;
                    break;
                }
            }
            // 预览图与主窗口背景用同一种显示方式
            PreviewImage.Stretch = ParseStretch(_settings.BackgroundFit);

            SliderOpacity.Value = _settings.BackgroundOpacity;
            TxtOpacityValue.Text = $"{(int)(_settings.BackgroundOpacity * 100)}%";

            SliderBlur.Value = Math.Clamp(_settings.BackgroundBlur, 0.0, 48.0);
            TxtBlurValue.Text = $"{(int)_settings.BackgroundBlur}px";
            ApplyPreviewBlur();

            SliderCardOpacity.Value = Math.Clamp(_settings.CardOpacity, 0.0, 1.0);
            TxtCardOpacityValue.Text = $"{(int)(_settings.CardOpacity * 100)}%";

            SliderUiTransparency.Value = Math.Clamp(_settings.UiTransparency, 0.0, 1.0);
            TxtUiTransparencyValue.Text = $"{(int)(_settings.UiTransparency * 100)}%";
        }

        /// <summary>视频容器（含 Wallpaper Engine 视频壁纸常用的 .webm）。</summary>
        private static bool IsVideoFile(string path)
        {
            var ext = Path.GetExtension(path).ToLowerInvariant();
            return ext is ".mp4" or ".webm" or ".avi" or ".mkv" or ".mov" or ".wmv";
        }

        private void BtnSelectImage_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog
            {
                Title = "选择背景文件",
                // .webp 故意不在列表里：WPF 的位图解码器不支持 webp，选了也只会得到一个坏背景。
                // .webm 必须留在列表里：Wallpaper Engine 的视频壁纸大量使用 webm。
                Filter = "媒体文件|*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.mp4;*.webm;*.avi;*.mkv;*.mov;*.wmv"
                       + "|图片文件|*.jpg;*.jpeg;*.png;*.bmp;*.gif"
                       + "|视频文件|*.mp4;*.webm;*.avi;*.mkv;*.mov;*.wmv"
                       + "|所有文件|*.*"
            };

            if (dialog.ShowDialog() == true)
                SetBackgroundFromFile(dialog.FileName);
        }

        /// <summary>
        /// 从 Wallpaper Engine 挑一张动态壁纸。只有「视频」类型的壁纸能被本工具直接播放，
        /// 详细说明见 <see cref="Services.WallpaperEngineService"/>。
        /// </summary>
        private void BtnPickWallpaperEngine_Click(object sender, RoutedEventArgs e)
        {
            var picked = Dialogs.WallpaperEnginePickerDialog.Pick(Window.GetWindow(this));
            if (string.IsNullOrEmpty(picked)) return;
            SetBackgroundFromFile(picked);
        }

        /// <summary>
        /// 从云端背景库挑一张：清单由用户在控制台自行上传，客户端只负责挑选。
        /// 下载到 %LOCALAPPDATA%\KunClient\wallpapers\ 后走和本地选图完全相同的 SetBackgroundFromFile，
        /// 所以预览 / 持久化 / 即时生效的行为一致；那份副本会随退出清理一起删掉。
        /// </summary>
        private void BtnPickCloudWallpaper_Click(object sender, RoutedEventArgs e)
        {
            var picked = Dialogs.CloudWallpaperPickerDialog.Pick(Window.GetWindow(this));
            if (string.IsNullOrEmpty(picked)) return;
            SetBackgroundFromFile(picked);
        }

        /// <summary>
        /// 把某个本地路径设为背景（文件选择器与 Wallpaper Engine 选择器共用这一条路径，
        /// 保证两边的预览/持久化/即时生效行为完全一致）。
        /// <para>
        /// 2026-09-23 重写：以前这里只按**扩展名**判断能不能用，于是
        /// ① 用户手选 WE 壁纸目录里的 <c>preview.gif / preview.jpg</c>（官方预览素材，
        ///    通常只有 256~512px 宽）→ 背景被放大到窗口，糊得发虚、比例还对不上；
        /// ② 用户手选**整个壁纸文件夹** → <c>File.Exists(目录)</c> 为 false，点了「确定」毫无反应。
        /// 现在统一走 <see cref="WallpaperEngineService.ResolveOne"/>：和「从 Wallpaper Engine 选择」
        /// 完全同一条读取口径（优先原始视频，其次官方预览素材），低清降级素材直接拒绝并说明原因。
        /// </para>
        /// </summary>
        private void SetBackgroundFromFile(string path)
        {
            if (string.IsNullOrWhiteSpace(path)) return;

            var ext = Path.GetExtension(path).ToLowerInvariant();

            // 目录也要能设（用户手选整张壁纸文件夹是常见操作）——所以这里不再要求 File.Exists。
            var item = WallpaperEngineService.ResolveOne(path);

            if (item == null || !item.Usable)
            {
                var reason = ext == ".webp"
                    ? "不支持 .webp，请换 png / jpg"
                    : Directory.Exists(path)
                        ? "这个文件夹里没有可用的背景素材"
                        : "这个文件用不了（格式不支持或已损坏）";
                TxtImagePath.Text = reason;
                PreviewHint.Text = reason;
                PreviewHint.Visibility = Visibility.Visible;
                PreviewImage.Source = null;
                return;     // 不写配置：宁可不变，也别留下一个加载不出来的背景
            }

            // 官方预览素材（scene / web 类壁纸的降级来源）只是"缩略预览"，不是真背景。
            // 「从 Wallpaper Engine 选择」里已经把这类整类隐藏了，私有背景这边同样拒绝，
            // 并明确告诉用户为什么 —— 这正是「私人背景模糊、大小都不对」的根因。
            if (item.FromPreview)
            {
                TxtImagePath.Text = "这是 WE 官方预览素材（低清）";
                PreviewHint.Text = $"{item.TypeText}：约 256~512px 的预览图，铺满窗口会发虚、比例也对不上。\n"
                                 + "请在 Wallpaper Engine 里换一张**视频类**壁纸，或用「从 Wallpaper Engine 选择」。";
                PreviewHint.Visibility = Visibility.Visible;
                PreviewImage.Source = null;
                return;
            }

            var resolved = item.SourcePath!;
            if (IsVideoFile(resolved))
            {
                // 视频背景没法在这里预览画面（要再起一个 MediaElement，成本高还会和主窗口抢解码器）。
                // 旧实现连提示一起隐藏了，预览框变成一块**空灰板**，看着就像"背景坏了"；
                // 现在明确告诉用户这是视频背景。
                PreviewImage.Source = null;
                PreviewHint.Text = "视频背景（此框不播放画面）";
                PreviewHint.Visibility = Visibility.Visible;
            }
            else
            {
                try
                {
                    var bitmap = new BitmapImage();
                    bitmap.BeginInit();
                    bitmap.UriSource = new Uri(resolved, UriKind.Absolute);
                    bitmap.CacheOption = BitmapCacheOption.OnLoad;
                    bitmap.EndInit();
                    bitmap.Freeze();
                    PreviewImage.Source = bitmap;
                    PreviewHint.Visibility = Visibility.Collapsed;
                    ApplyPreviewBlur();   // 换了预览图，模糊要重新挂到新 Source 上
                }
                catch
                {
                    TxtImagePath.Text = "文件无法加载";
                    PreviewHint.Text = "这个文件解不开，已保持原背景";
                    PreviewHint.Visibility = Visibility.Visible;
                    return;     // 不写配置：宁可不变，也别留下一个加载不出来的背景
                }
            }

            _settings.BackgroundImagePath = resolved;
            _settings.Save();
            TxtImagePath.Text = Path.GetFileName(resolved);
            ApplyBackground();
        }

        private void BtnClearImage_Click(object sender, RoutedEventArgs e)
        {
            _settings.BackgroundImagePath = "";
            _settings.Save();
            PreviewImage.Source = null;
            PreviewHint.Visibility = Visibility.Visible;
            TxtImagePath.Text = "未选择";
            ApplyBackground();
        }

        private void CmbAnimation_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_isInitializing) return;
            var item = CmbAnimation.SelectedItem as ComboBoxItem;
            if (item?.Tag != null)
            {
                _settings.BackgroundAnimation = item.Tag.ToString()!;
                _settings.Save();
                ApplyBackground();
            }
        }

        private void CmbBackgroundFit_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_isInitializing) return;
            var item = CmbBackgroundFit.SelectedItem as ComboBoxItem;
            if (item?.Tag != null)
            {
                _settings.BackgroundFit = item.Tag.ToString()!;
                _settings.Save();
                // 预览图与主窗口背景用同一种显示方式，所见即所得
                PreviewImage.Stretch = ParseStretch(_settings.BackgroundFit);
                ApplyBackground();
            }
        }

        private static Stretch ParseStretch(string? fit) => fit switch
        {
            "Uniform" => Stretch.Uniform,
            "Stretch" => Stretch.Fill,
            _ => Stretch.UniformToFill,
        };

        private void SliderOpacity_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (_isInitializing) return;
            _settings.BackgroundOpacity = SliderOpacity.Value;
            TxtOpacityValue.Text = $"{(int)(SliderOpacity.Value * 100)}%";
            PreviewImage.Opacity = SliderOpacity.Value;
            _settings.Save();
            ApplyBackground();
        }

        /// <summary>
        /// 背景模糊（玻璃的磨砂底）：半径写进设置，预览图和主窗口背景一起重挂效果。
        /// 主窗口走 ApplyDynamicBackground → DynamicBackground.ApplyAnimation → SyncBackdropBlur，
        /// 和换背景/改动画共用同一条同步路径，不会出现「只改了设置没生效」。
        /// </summary>
        private void SliderBlur_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (_isInitializing) return;
            _settings.BackgroundBlur = SliderBlur.Value;
            TxtBlurValue.Text = $"{(int)SliderBlur.Value}px";
            _settings.Save();
            ApplyPreviewBlur();
            ApplyBackground();
        }

        /// <summary>设置页预览图挂同一支模糊，拖滑条时当场看得见（0 = 摘掉）。</summary>
        private void ApplyPreviewBlur()
        {
            var radius = Math.Clamp(_settings.BackgroundBlur, 0.0, 48.0);
            PreviewImage.Effect = radius <= 0 || PreviewImage.Source == null
                ? null
                : new BlurEffect { Radius = radius };
        }

        private void ApplyBackground()
        {
            var mainWindow = Window.GetWindow(this) as MainWindow;
            mainWindow?.ApplyDynamicBackground();
        }

        private void SliderCardOpacity_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (_isInitializing) return;
            _settings.CardOpacity = SliderCardOpacity.Value;
            TxtCardOpacityValue.Text = $"{(int)(SliderCardOpacity.Value * 100)}%";
            _settings.Save();
            var mainWindow = Window.GetWindow(this) as MainWindow;
            mainWindow?.ApplyCardOpacity();
        }

        /// <summary>全局界面透明度：改的是所有页面根背景 Theme.BgTint 的 alpha，一次生效全站。</summary>
        private void SliderUiTransparency_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (_isInitializing) return;
            _settings.UiTransparency = SliderUiTransparency.Value;
            TxtUiTransparencyValue.Text = $"{(int)(SliderUiTransparency.Value * 100)}%";
            _settings.Save();
            var mainWindow = Window.GetWindow(this) as MainWindow;
            mainWindow?.ApplyUiTransparency();
        }

        private static readonly HttpClient _profileHttp = Services.ServerGate.CreateClient(TimeSpan.FromSeconds(15));
        private static string ProfileServerUrl => KunFlashTool.App.Services.AppConfig.ServerUrl;

        private async Task LoadProfileAsync()
        {
            try
            {
                var token = KunFlashTool.App.Services.AuthSession.ResolveToken();
                if (string.IsNullOrEmpty(token))
                {
                    TxtProfileStatus.Text = "未登录，登录后显示用户信息";
                    return;
                }

                var request = new HttpRequestMessage(HttpMethod.Get, $"{ProfileServerUrl}/api/v2/profile");
                request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
                using var response = await _profileHttp.SendAsync(request);
                var json = await response.Content.ReadAsStringAsync();
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (response.IsSuccessStatusCode && result.GetProperty("success").GetBoolean())
                {
                    var data = result.GetProperty("data");
                    TxtUsername.Text = data.GetProperty("username").GetString() ?? "-";
                    // 角色文案统一走 AccountText，与官网/控制台保持同一套叫法
                    TxtRole.Text = Services.AccountText.Role(
                        data.TryGetProperty("role", out var roleEl) ? roleEl.GetString() : null);
                    TxtRegisterDate.Text = data.GetProperty("created_at").GetString() ?? "-";
                    TxtLastLogin.Text = data.GetProperty("last_login").GetString() ?? "-";

                    // 会员等级 + 到期时间：与窗口标题共用 AccountText，两处口径必须一致。
                    var vipLevel = data.TryGetProperty("vipLevel", out var lvEl) && lvEl.ValueKind == JsonValueKind.String
                        ? lvEl.GetString() : null;
                    var vipExpire = data.TryGetProperty("vip_expire_at", out var veEl) && veEl.ValueKind == JsonValueKind.String
                        ? veEl.GetString() : null;
                    var vipPermanent = data.TryGetProperty("vipPermanent", out var vpEl) && vpEl.ValueKind == JsonValueKind.True;
                    var vipDaysLeft = data.TryGetProperty("vipDaysLeft", out var vdEl) && vdEl.ValueKind == JsonValueKind.Number
                        ? vdEl.GetInt32() : 0;

                    TxtVipLevel.Text = Services.AccountText.VipLevel(vipLevel);
                    TxtVipExpire.Text = Services.AccountText.VipExpireText(vipExpire, vipPermanent, vipDaysLeft);

                    // 个性签名（未设置时给出明确提示，不要留一个空的“-”让人以为是加载失败）
                    var sig = data.TryGetProperty("signature", out var sgEl) && sgEl.ValueKind == JsonValueKind.String
                        ? sgEl.GetString() : null;
                    TxtSignature.Text = string.IsNullOrWhiteSpace(sig) ? "未设置" : sig;

                    TxtProfileStatus.Visibility = Visibility.Collapsed;
                    ProfileGrid.Visibility = Visibility.Visible;
                }
                else
                {
                    TxtProfileStatus.Text = "用户信息加载失败";
                }
            }
            catch
            {
                TxtProfileStatus.Text = "用户信息加载失败";
            }
        }

        // ===================== 法律与合规 =====================

        /// <summary>「查看使用条款与隐私说明」：只读模式打开，关掉不影响使用、也不会退出程序。</summary>
        private void BtnShowLegal_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new KunFlashTool.App.Dialogs.DisclaimerDialog(enforce: false)
            {
                Owner = Window.GetWindow(this)
            };
            dialog.ShowDialog();
            UpdateLegalState();
        }

        /// <summary>在系统默认浏览器里打开完整条款页（以隐私政策为入口，页内可跳转另两份）。</summary>
        private void BtnOpenLegalSite_Click(object sender, RoutedEventArgs e)
        {
            var url = AppConfig.ServerUrl.TrimEnd('/') + "/privacy/";
            try
            {
                System.Diagnostics.Process.Start(
                    new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true });
            }
            catch (Exception ex)
            {
                MessageBox.Show($"无法打开浏览器：{ex.Message}\n\n可手动访问：{url}",
                    "提示", MessageBoxButton.OK, MessageBoxImage.Information);
            }
        }

        private void UpdateLegalState()
        {
            if (TxtLegalState == null) return;

            const int cur = KunFlashTool.App.Dialogs.DisclaimerDialog.LegalVersion;
            var accepted = _settings.LegalAcceptedVersion;
            TxtLegalState.Text = accepted >= cur
                ? $"已同意条款版本 v{accepted}（当前版本 v{cur}）"
                : $"尚未同意当前版本条款（当前版本 v{cur}），下次启动时会再次提示";
        }
    }
}
