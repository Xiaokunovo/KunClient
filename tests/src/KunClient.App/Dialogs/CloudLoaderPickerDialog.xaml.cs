using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Net.Http;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// 云端引导库选择器：从服务器 <c>/api/loaders/firehose/index</c> 拉索引，
    /// 按品牌/机型挑一个 firehose 引导程序。数据源就是上传到服务端的
    /// <c>assets/loaders/firehose/&lt;品牌&gt;/&lt;机型&gt;</c> 那一整库。
    /// </summary>
    public partial class CloudLoaderPickerDialog : Window
    {
        private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(20) };
        private static string ServerUrl => KunFlashTool.App.Services.AppConfig.ServerUrl;

        private readonly ObservableCollection<CloudLoaderItem> _all = new();
        private readonly ObservableCollection<CloudLoaderItem> _view = new();
        private bool _filtering;

        public sealed class CloudLoaderItem
        {
            public string Brand { get; init; } = "";
            public string Model { get; init; } = "";
            /// <summary>形如 <c>mi/865</c>，下载接口的 name 参数直接用它。</summary>
            public string Name { get; init; } = "";
            public long Size { get; init; }
            public string SizeText => LoaderPickerDialog.FormatSize(Size);

            /// <summary>
            /// OPLUS digest 签名表的云端文件名（形如 <c>oppo/450.digest</c>），没有则为空。
            /// 与 <see cref="Signature"/> 同属一条索引 —— 选中机型点一次「下载引导」，
            /// programmer + digest + signature 整套一起下，不必再手选签名表。
            /// </summary>
            public string Digest { get; init; } = "";

            /// <summary>OPLUS signature 签名表的云端文件名（形如 <c>oppo/450.sig</c>），没有则为空。</summary>
            public string Signature { get; init; } = "";

            /// <summary>是否 OPLUS 签名通道整套（云端配了 digest/signature）。</summary>
            public bool HasAuthSet => Digest.Length > 0 || Signature.Length > 0;

            /// <summary>列表里的「通道」列：签名 / 通用。</summary>
            public string ChannelText => HasAuthSet ? "签名" : "通用";

            /// <summary>
            /// 解析 <c>/api/loaders/firehose/index</c> 的返回。
            /// 单独抽出来是为了让「整套字段有没有被吃掉」这类问题能被单测钉住 ——
            /// 解析嵌在 UI 事件里的话，改坏了一路绿到真机才炸。
            /// </summary>
            public static (bool Success, List<CloudLoaderItem> Items) ParseIndex(string json)
            {
                var items = new List<CloudLoaderItem>();
                using var doc = JsonDocument.Parse(json);
                var root = doc.RootElement;

                if (!root.TryGetProperty("success", out var ok) || ok.ValueKind != JsonValueKind.True)
                    return (false, items);

                if (root.TryGetProperty("data", out var arr) && arr.ValueKind == JsonValueKind.Array)
                {
                    foreach (var el in arr.EnumerateArray())
                    {
                        var name = el.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "";
                        if (string.IsNullOrEmpty(name)) continue;
                        items.Add(new CloudLoaderItem
                        {
                            Brand = el.TryGetProperty("brand", out var b) ? b.GetString() ?? "" : "",
                            Model = el.TryGetProperty("model", out var m) ? m.GetString() ?? "" : "",
                            Name = name,
                            Size = el.TryGetProperty("size", out var s) && s.TryGetInt64(out var sz) ? sz : 0,
                            Digest = el.TryGetProperty("digest", out var d) ? d.GetString() ?? "" : "",
                            Signature = el.TryGetProperty("signature", out var g) ? g.GetString() ?? "" : "",
                        });
                    }
                }

                return (true, items);
            }
        }

        /// <summary>用户选中的条目；取消时为 null。</summary>
        public CloudLoaderItem? Selected { get; private set; }

        public CloudLoaderPickerDialog(Window? owner = null)
        {
            InitializeComponent();
            if (owner != null) Owner = owner;

            TxtSearch.Text = Placeholder;
            TxtSearch.Foreground = (System.Windows.Media.Brush)FindResource("Theme.TextTertiary");
            DgLoaders.ItemsSource = _view;

            Loaded += async (_, __) => await LoadIndexAsync();
        }

        private const string Placeholder = "按机型筛选，例如 865、y700、redmagic";

        public static CloudLoaderItem? Pick(Window? owner)
        {
            var dialog = new CloudLoaderPickerDialog(owner);
            return dialog.ShowDialog() == true ? dialog.Selected : null;
        }

        // ───────────────────────── 数据 ─────────────────────────

        private async Task LoadIndexAsync()
        {
            SetEmpty("正在从服务器获取引导库…");
            BtnConfirm.IsEnabled = false;
            _all.Clear();
            _view.Clear();

            try
            {
                var json = await Http.GetStringAsync($"{ServerUrl}/api/v2/loaders/firehose/index");
                var (ok, items) = CloudLoaderItem.ParseIndex(json);
                if (!ok)
                {
                    SetEmpty("服务器返回失败，请稍后重试");
                    return;
                }

                foreach (var item in items)
                    _all.Add(item);

                if (_all.Count == 0)
                {
                    SetEmpty("引导库还是空的。\n服务端 assets/loaders/firehose/ 下还没有文件。");
                    return;
                }

                BuildBrandFilter();
                ApplyFilter();
            }
            catch (Exception ex)
            {
                SetEmpty($"无法连接服务器（{ServerUrl}）\n{ex.Message}");
            }
        }

        private void BuildBrandFilter()
        {
            var current = (CmbBrand.SelectedItem as ComboBoxItem)?.Tag?.ToString();
            CmbBrand.Items.Clear();

            var all = new ComboBoxItem { Content = $"全部品牌（{_all.Count}）", Tag = "" };
            CmbBrand.Items.Add(all);

            foreach (var brand in _all.Select(x => x.Brand).Where(x => x.Length > 0)
                                     .Distinct(StringComparer.OrdinalIgnoreCase)
                                     .OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
            {
                var count = _all.Count(x => string.Equals(x.Brand, brand, StringComparison.OrdinalIgnoreCase));
                CmbBrand.Items.Add(new ComboBoxItem { Content = $"{brand}（{count}）", Tag = brand });
            }

            CmbBrand.SelectedIndex =
                string.IsNullOrEmpty(current)
                    ? 0
                    : Math.Max(0, CmbBrand.Items.OfType<ComboBoxItem>().ToList()
                                        .FindIndex(i => (i.Tag?.ToString() ?? "") == current));
        }

        private void SetEmpty(string text)
        {
            TxtEmpty.Text = text;
            TxtEmpty.Visibility = Visibility.Visible;
        }

        private void ApplyFilter()
        {
            _filtering = true;
            try
            {
                var brand = (CmbBrand.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "";
                var keyword = TxtSearch.Text == Placeholder ? "" : (TxtSearch.Text ?? "").Trim();

                // 🔴 填表期间先摘开 ItemsSource：_view 是 ObservableCollection，绑定状态下每 Add 一格
                //    DataGrid 就插一行容器并当场重排（机型多时逐键过滤是 O(n²)）；挂回去只发一次 Reset。
                DgLoaders.ItemsSource = null;
                _view.Clear();
                foreach (var item in _all)
                {
                    if (brand.Length > 0 && !string.Equals(item.Brand, brand, StringComparison.OrdinalIgnoreCase))
                        continue;
                    if (keyword.Length > 0 &&
                        item.Name.IndexOf(keyword, StringComparison.OrdinalIgnoreCase) < 0)
                        continue;
                    _view.Add(item);
                }
                DgLoaders.ItemsSource = _view;

                TxtEmpty.Text = _view.Count == 0 ? "没有匹配的机型，换个关键词试试" : "";
                TxtEmpty.Visibility = _view.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
                TxtSubtitle.Text = $"共 {_all.Count} 款 · 当前 {_view.Count} 款";
                BtnConfirm.IsEnabled = _view.Count > 0;
            }
            finally { _filtering = false; }
        }

        // ───────────────────────── 事件 ─────────────────────────

        private void CmbBrand_SelectionChanged(object sender, SelectionChangedEventArgs e)
        {
            if (_filtering || !IsLoaded) return;
            ApplyFilter();
        }

        private void TxtSearch_TextChanged(object sender, TextChangedEventArgs e)
        {
            if (_filtering || !IsLoaded) return;
            ApplyFilter();
        }

        private async void BtnRefresh_Click(object sender, RoutedEventArgs e) => await LoadIndexAsync();

        private void DgLoaders_MouseDoubleClick(object sender, MouseButtonEventArgs e) => Confirm();

        private void BtnConfirm_Click(object sender, RoutedEventArgs e) => Confirm();

        private void Confirm()
        {
            if (DgLoaders.SelectedItem is not CloudLoaderItem item)
            {
                TxtHint.Text = "请先选中一款机型";
                return;
            }
            Selected = item;
            DialogResult = true;
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e) => DialogResult = false;

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ButtonState == MouseButtonState.Pressed) DragMove();
        }
    }
}
