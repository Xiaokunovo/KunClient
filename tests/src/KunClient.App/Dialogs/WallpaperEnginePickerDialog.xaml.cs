using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// Wallpaper Engine 动态壁纸选择器。
    /// <para>
    /// Wallpaper Engine 本质上是个播放器，它把「壁纸」放在 Steam 创意工坊的 431960 目录下，
    /// 每张壁纸一个文件夹，里面有 project.json 描述类型与资源文件。本工具的动态背景是
    /// MediaElement，所以 <c>type=video</c> 的壁纸可以直接取它的 mp4/webm 来播，
    /// 完全不需要 Wallpaper Engine 在后台跑。
    /// </para>
    /// <para>
    /// scene / web 两类是私有格式，无法还原其原始渲染；但这两类壁纸目录里 Wallpaper Engine
    /// 自己会写一份官方预览素材（preview.gif 动图，或 preview.jpg 静态图），本工具用它降级顶上
    /// ——画面对得上、动图还能动，比判"不可用"有用得多。详见 <see cref="WallpaperEngineService"/>。
    /// </para>
    /// </summary>
    public partial class WallpaperEnginePickerDialog : Window
    {
        private readonly ObservableCollection<WallpaperEngineService.WallpaperItem> _items = new();

        /// <summary>用户选中的素材路径（视频 / 动图 / 图片）；取消或选了不可用项时为 null。</summary>
        public string? SelectedPath { get; private set; }

        public WallpaperEnginePickerDialog(Window? owner = null)
        {
            InitializeComponent();
            if (owner != null) Owner = owner;
            DgItems.ItemsSource = _items;
            Loaded += async (_, __) => await RescanAsync(null);
        }

        /// <summary>弹出选择器，返回选中素材的绝对路径（视频 / 动图 / 图片；取消返回 null）。</summary>
        public static string? Pick(Window? owner)
        {
            var dialog = new WallpaperEnginePickerDialog(owner);
            return dialog.ShowDialog() == true ? dialog.SelectedPath : null;
        }

        // ───────────────────────── 扫描 ─────────────────────────

        private Task RescanAsync(string? extraFolder)
        {
            SetHint("正在扫描本机的 Wallpaper Engine 壁纸…");
            BtnConfirm.IsEnabled = false;
            _items.Clear();

            return Task.Run(() =>
            {
                var found = new List<WallpaperEngineService.WallpaperItem>();
                try
                {
                    found.AddRange(WallpaperEngineService.Discover());
                    if (!string.IsNullOrWhiteSpace(extraFolder))
                        found.AddRange(WallpaperEngineService.DiscoverIn(extraFolder!));
                }
                catch { }

                // 去重（Discover 与 DiscoverIn 可能命中同一张壁纸）
                var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
                var deduped = new List<WallpaperEngineService.WallpaperItem>();
                foreach (var it in found)
                {
                    var key = it.SourcePath ?? it.Folder;
                    if (!string.IsNullOrEmpty(key) && seen.Add(key)) deduped.Add(it);
                }
                deduped.Sort((a, b) =>
                {
                    var c = b.Usable.CompareTo(a.Usable);
                    return c != 0 ? c : string.Compare(a.Title, b.Title, StringComparison.CurrentCulture);
                });

                // 只保留**本工具能真正播放**的壁纸（视频类）。
                // 场景(scene)/网页(web) 类是 WE 私有格式，本体没有可播的视频文件，只能拿官方预览素材
                // （preview.gif/jpg，通常只有 256~512 px 宽）顶上：铺满窗口后明显发虚、比例也常对不上
                // —— 用户 2026-09-23 反馈的「私人背景模糊、大小都不对」就是它。
                // 按用户决定：与其给一个糊的假背景，不如根本不出现在列表里（会在提示里说明隐藏了多少张）。
                // 想恢复：删掉下面这个过滤循环、直接用 deduped 即可（但请记得预览素材是低清的）。
                var playable = new List<WallpaperEngineService.WallpaperItem>();
                var hidden = 0;
                foreach (var it in deduped)
                {
                    if (it.SourceKind == WallpaperSourceKind.Video) playable.Add(it);
                    else hidden++;
                }

                Dispatcher.Invoke(() => ShowResult(playable, extraFolder, hidden));
            });
        }

        private void ShowResult(List<WallpaperEngineService.WallpaperItem> list, string? extraFolder, int hidden = 0)
        {
            _items.Clear();
            foreach (var it in list) _items.Add(it);

            var hiddenNote = hidden > 0
                ? $"\n・另有 {hidden} 张场景/网页(scene/web)类壁纸已隐藏：它们是 WE 私有格式，本工具只能拿官方预览素材顶上，铺满窗口会发虚。"
                : "";

            if (_items.Count == 0)
            {
                TxtEmpty.Visibility = Visibility.Visible;
                TxtEmpty.Text = WallpaperEngineService.IsInstalled()
                    ? "扫描到 Wallpaper Engine，但这个目录里没有找到可用素材。\n\n"
                      + "・本工具只列**视频类**壁纸（mp4/webm 等，直接播放，清晰度最高）；\n"
                      + "・如果壁纸存在别的磁盘，可以点右上角「手动指定文件夹」直接选它的目录。"
                      + hiddenNote
                    : "没有在本机找到 Wallpaper Engine 的壁纸目录。\n\n"
                      + "・如果确实装了，请点右上角「手动指定文件夹」，\n"
                      + "  选择 Steam\\steamapps\\workshop\\content\\431960，\n"
                      + "  或 Wallpaper Engine\\projects\\myprojects；\n"
                      + "・也可以直接用左侧的「选择文件」挑一个本地图片/视频当背景。";
                SetHint("未找到可用壁纸");
                return;
            }

            TxtEmpty.Visibility = Visibility.Collapsed;
            var usable = 0;
            foreach (var it in _items) if (it.Usable) usable++;
            SetHint((extraFolder == null
                ? $"共 {_items.Count} 张可用壁纸（双击一行即可）"
                : $"已合并手动目录：共 {_items.Count} 张可用壁纸") + hiddenNote);
        }

        private void SetHint(string text) => TxtHint.Text = text;

        // ───────────────────────── 交互 ─────────────────────────

        private async void BtnRefresh_Click(object sender, RoutedEventArgs e) => await RescanAsync(null);

        private async void BtnPickFolder_Click(object sender, RoutedEventArgs e)
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog
            {
                Title = "选择 Wallpaper Engine 壁纸目录（创意工坊 431960，或 projects\\myprojects）",
            };
            if (dialog.ShowDialog(this) != true) return;
            await RescanAsync(dialog.FolderName);
        }

        private void DgItems_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            var item = DgItems.SelectedItem as WallpaperEngineService.WallpaperItem;
            BtnConfirm.IsEnabled = item?.Usable == true;
            if (item == null) { TxtHint.Text = "双击一行即可直接使用"; return; }
            TxtHint.Text = item.Usable
                ? $"将使用：{item.SourcePath}（{item.TypeText}）"
                : $"{item.TypeText} —— 该壁纸目录里没有可直接使用的素材，请在 Wallpaper Engine 中导出后再用「选择文件」载入";
        }

        private void DgItems_MouseDoubleClick(object sender, MouseButtonEventArgs e)
        {
            if (DgItems.SelectedItem is WallpaperEngineService.WallpaperItem it && it.Usable) Confirm(it);
        }

        private void BtnConfirm_Click(object sender, RoutedEventArgs e)
        {
            if (DgItems.SelectedItem is WallpaperEngineService.WallpaperItem it) Confirm(it);
        }

        private void Confirm(WallpaperEngineService.WallpaperItem item)
        {
            if (!item.Usable)
            {
                SetHint(item.TypeText + " —— 不能直接使用");
                return;
            }
            SelectedPath = item.SourcePath;
            DialogResult = true;
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e) => DialogResult = false;

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ButtonState == MouseButtonState.Pressed) DragMove();
        }
    }
}
