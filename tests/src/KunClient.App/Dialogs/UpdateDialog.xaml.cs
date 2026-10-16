using System;
using System.Threading.Tasks;
using System.Windows;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Dialogs
{
    public partial class UpdateDialog : Window
    {
        private UpdateInfo? _updateInfo;

        /// <summary>
        /// 本次更新是否已成功应用。调用方（MainWindow 强制更新分支）据此判断
        /// 是否需要关闭旧版本界面：更新已应用时 UpdateService 内部已完成重启流程，
        /// 若再 Shutdown 会与重启竞争导致新版本被立即关闭。
        /// </summary>
        public bool UpdateApplied { get; private set; }

        public UpdateDialog()
        {
            InitializeComponent();
            Loaded += async (_, _) =>
            {
                await WindowAnimation.FadeInAsync(this);
                await CheckUpdateAsync();
            };
        }

        private async Task CheckUpdateAsync()
        {
            CheckingPanel.Visibility = Visibility.Visible;
            NoUpdatePanel.Visibility = Visibility.Collapsed;
            UpdatePanel.Visibility = Visibility.Collapsed;
            ErrorPanel.Visibility = Visibility.Collapsed;
            UpdateButton.Visibility = Visibility.Collapsed;

            try
            {
                var info = await UpdateService.CheckForUpdateAsync();
                if (info == null)
                {
                    CheckingPanel.Visibility = Visibility.Collapsed;
                    NoUpdatePanel.Visibility = Visibility.Visible;
                    // 版本号格式统一为 `Ver:1.0.9`。原来前缀写「当前版本: 」，再拼 `Ver:` 会
                    // 出现「当前版本: Ver:1.0.9」两个冒号，所以这里把标签的冒号去掉。
                    CurrentVersionText.Text = $"当前版本 Ver:{UpdateService.CurrentVersion}";
                }
                else
                {
                    _updateInfo = info;
                    CheckingPanel.Visibility = Visibility.Collapsed;
                    UpdatePanel.Visibility = Visibility.Visible;
                    NewVersionText.Text = $"Ver:{UpdateService.CurrentVersion} → Ver:{info.Version}";
                    BuildDateText.Text = info.BuildDate;
                    ChangelogText.Text = info.Changelog;
                    UpdateButton.Visibility = Visibility.Visible;
                }
            }
            catch (Exception ex)
            {
                CheckingPanel.Visibility = Visibility.Collapsed;
                ErrorPanel.Visibility = Visibility.Visible;
                ErrorText.Text = ex.Message;
            }
        }

        private async void CheckButton_Click(object sender, RoutedEventArgs e)
        {
            await CheckUpdateAsync();
        }

        private async void UpdateButton_Click(object sender, RoutedEventArgs e)
        {
            if (_updateInfo == null) return;

            UpdateButton.IsEnabled = false;
            UpdateButton.Content = "更新中...";
            ProgressBar.Visibility = Visibility.Visible;

            var progress = new Progress<string>(msg =>
            {
                StatusText.Text = msg;
                if (msg.Contains("%"))
                {
                    var parts = msg.Split(':');
                    if (parts.Length > 1 && int.TryParse(parts[1].Replace("%", "").Trim(), out var pct))
                        ProgressBar.Value = pct;
                }
            });

            var success = await UpdateService.DownloadAndApplyUpdateAsync(_updateInfo, progress);

            if (success)
            {
                UpdateApplied = true;
            }
            else
            {
                UpdateButton.IsEnabled = true;
                UpdateButton.Content = "立即更新";
                ProgressBar.Visibility = Visibility.Collapsed;
            }
        }

        private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

        private void Window_MouseLeftButtonDown(object sender, System.Windows.Input.MouseButtonEventArgs e)
        {
            if (e.ButtonState == System.Windows.Input.MouseButtonState.Pressed)
                DragMove();
        }
    }
}
