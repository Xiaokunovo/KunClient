// ============================================================================
// AppealDialog —— 被封禁账号的**免登录**申诉入口（2026-09-30）
//
// 🔴 为什么要免登录：被封的人根本登不进去，要求先登录的申诉入口等于不存在。
//    代价是必须自己防刷（服务端已做：pending 限 1 条 / 7 天限 1 次 / 同 IP 1 小时 5 条）。
// 🔴 绝不自动解封：这里只把「我不服」+ 证据流水送到管理员面前，由**人**来定。
// ============================================================================
using System;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Dialogs
{
    public partial class AppealDialog : Window
    {
        private static readonly HttpClient httpClient = Services.ServerGate.CreateClient(TimeSpan.FromSeconds(15));
        private static string serverUrl => AppConfig.ServerUrl;

        public AppealDialog(string username)
        {
            InitializeComponent();
            if (!string.IsNullOrEmpty(username)) TxtUsername.Text = username;
        }

        private void Cancel_Click(object sender, RoutedEventArgs e) => Close();

        private void ShowStatus(string message, bool isError)
        {
            StatusBorder.Visibility = Visibility.Visible;
            // 🔴 底色用**半透明**语义色，让主题背景透出来：写死浅粉/浅绿在深色主题下
            //    会变成一块刺眼的亮斑。文字/图标用不透明的语义色，两种主题都够对比。
            var accent = (Color)ColorConverter.ConvertFromString(isError ? "#DC2626" : "#16A34A");
            StatusInner.Background = new SolidColorBrush(Color.FromArgb(28, accent.R, accent.G, accent.B));
            var fg = new SolidColorBrush(accent);
            StatusIcon.Text = isError ? "\u2717" : "\u2713";
            StatusIcon.Foreground = fg;
            StatusText.Text = message;
            StatusText.Foreground = fg;
        }
        private void HideStatus() => StatusBorder.Visibility = Visibility.Collapsed;

        private async void Submit_Click(object sender, RoutedEventArgs e)
        {
            var username = TxtUsername.Text.Trim();
            var content = TxtContent.Text.Trim();
            var contact = TxtContact.Text.Trim();

            if (string.IsNullOrEmpty(username)) { ShowStatus("请填写账号名", true); return; }
            if (content.Length < 10) { ShowStatus("请说明具体情况（至少 10 个字）", true); return; }
            if (content.Length > 1000) { ShowStatus("申诉内容最多 1000 字", true); return; }

            BtnSubmit.IsEnabled = false;
            try
            {
                // 🔴 免登录：不带 Authorization 头，直接打 /api/appeal
                var payload = JsonBody.Of(("username", username), ("content", content), ("contact", contact));
                using var req = new StringContent(payload, Encoding.UTF8, "application/json");
                using var response = await httpClient.PostAsync($"{serverUrl}/api/v1/appeal", req);
                var json = await response.Content.ReadAsStringAsync();
                var result = JsonSerializer.Deserialize<JsonElement>(json);
                var msg = result.TryGetProperty("message", out var m) && m.ValueKind == JsonValueKind.String
                    ? m.GetString() : "提交失败";

                if (response.IsSuccessStatusCode
                    && result.TryGetProperty("success", out var s) && s.ValueKind == JsonValueKind.True)
                {
                    ShowStatus(msg ?? "申诉已提交，我们会人工复核", false);
                    // 已提交则禁用按钮，避免重复提交（服务端也会拦 pending）
                    BtnSubmit.IsEnabled = false;
                }
                else
                {
                    ShowStatus(msg ?? "提交失败", true);
                    BtnSubmit.IsEnabled = true;
                }
            }
            catch (HttpRequestException) { ShowStatus("无法连接服务器，请检查网络", true); BtnSubmit.IsEnabled = true; }
            catch (Exception ex) { ShowStatus($"提交失败: {ex.Message}", true); BtnSubmit.IsEnabled = true; }
        }
    }
}
