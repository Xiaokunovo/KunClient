using System;
using System.Threading.Tasks;
using System.Windows;
using KunFlashTool.App.Dialogs;
using KunFlashTool.App.Services;

namespace KunFlashTool.App;

public partial class StartupWindow : Window
{
    private string? _username;
    private string? _token;
    private string? _vipExpire;
    private string? _vipLevel;

    public StartupWindow()
    {
        InitializeComponent();
        Title = $"KunClient Ver:{UpdateService.CurrentVersion} by 小坤ovo";
        Loaded += StartupWindow_Loaded;
    }

    private void StartupWindow_Loaded(object sender, RoutedEventArgs e)
    {
        try
        {
            var loginDialog = new LoginDialog();
            loginDialog.WindowStartupLocation = WindowStartupLocation.CenterScreen;
            var result = loginDialog.ShowDialog();
            if (result != true)
            {
                Environment.Exit(0);
                return;
            }

            _username = loginDialog.LoggedInUsername;
            _token = loginDialog.AuthToken;
            // 会员到期时间必须一起带过去：只传用户名的话主窗口拿不到会员身份，
            // 会员用户也会被 MembershipService 当成免费用户（刷写每天限 2 次）。
            _vipExpire = loginDialog.LoggedInVipExpire;
            _vipLevel = loginDialog.LoggedInVipLevel;

            var downloadDialog = new DownloadDialog();
            downloadDialog.WindowStartupLocation = WindowStartupLocation.CenterScreen;
            downloadDialog.ShowDialog();

            var mainWindow = new MainWindow();
            mainWindow.Show();
            mainWindow.SetUserInfo(_username!, vipExpire: _vipExpire ?? "", vipLevel: _vipLevel ?? "");

            this.Close();
        }
        catch (Exception ex)
        {
            System.IO.File.WriteAllText(
                System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "KunClient", "startup_error.log"),
                ex.ToString());
            Environment.Exit(1);
        }
    }
}
