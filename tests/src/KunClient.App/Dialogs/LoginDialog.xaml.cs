using System;
using System.Net.Http;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using KunFlashTool.App.Services;
using KunFlashTool.Core.Services;

namespace KunFlashTool.App.Dialogs
{
    public partial class LoginDialog : Window
    {
        public bool IsLoggedIn { get; private set; }
        public string? AuthToken { get; private set; }
        public string? LoggedInUsername { get; private set; }
        /// <summary>登录后拿到的会员到期时间。启动流程要靠它把会员身份交给主窗口，
        /// 否则会员会被当成免费用户，刷写白白被限成每天 2 次。</summary>
        public string? LoggedInVipExpire { get; private set; }
        /// <summary>登录后拿到的会员等级（normal / vip / svip），用于窗口标题与官网/控制台统一叫法。</summary>
        public string? LoggedInVipLevel { get; private set; }

        private static readonly HttpClient httpClient = CreateHttpClient();

        /// <summary>
        /// 统一装请求头。原来这个 HttpClient **一个头都不带（UA 为空）**，
        /// 在网关/CF 眼里是强机器人信号 —— 配合 ServerGate 一起改掉。
        /// </summary>
        private static HttpClient CreateHttpClient()
        {
            var c = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
            Services.ServerGate.ApplyDefaultHeaders(c);
            return c;
        }
        // 软配置：服务器地址来自 AppConfig（KunClient.config.json / 环境变量 / 登录框"服务器设置"），不要在这里写死
        private static string serverUrl => AppConfig.ServerUrl;

        private enum LoginMode { Password, Key, Email, Register }
        private LoginMode _currentMode = LoginMode.Password;

        /// <summary>
        /// 法务条款版本，注册时随请求上报（服务端写进 users.legal_version 作为同意凭证）。
        /// 必须与 <see cref="DisclaimerDialog.LegalVersion"/>、server.js 的 LEGAL_VERSION、
        /// 控制台的 LEGAL_VERSION 保持一致 —— 这四个是**同一个语义的四份拷贝**，
        /// 条款有实质变更就四处一起 +1。
        /// </summary>
        private const int LegalVersion = 5;

        /// <summary>用系统默认浏览器打开官网的某一份法务文档（terms / privacy / disclaimer）。</summary>
        private void OpenLegalDoc(string doc)
        {
            try
            {
                var url = serverUrl.TrimEnd('/') + "/" + doc + "/";
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo
                { FileName = url, UseShellExecute = true });
            }
            catch (Exception ex) { ShowStatus("无法打开条款页面：" + ex.Message, true); }
        }

        private void OpenTerms_Click(object sender, RoutedEventArgs e) => OpenLegalDoc("terms");
        private void OpenPrivacy_Click(object sender, RoutedEventArgs e) => OpenLegalDoc("privacy");
        private void OpenDisclaimer_Click(object sender, RoutedEventArgs e) => OpenLegalDoc("disclaimer");
        private bool _isLoggingIn;

        /// <summary>提交进行中（防重复提交）。
        /// 🔴 不能复用 _isLoggingIn：它只在**登录成功**的 CompleteLogin 里才置 true，
        ///    而 Enter 触发的两次提交都发生在请求**之前**，守卫等于没有。
        ///    这里由 SetLoading 统一维护：进入提交 true，finally 里回 false。</summary>
        private bool _submitting;

        /// <summary>底部「版本号 · by 小坤ovo · 当前时间」里的秒需要走，用 1s 定时器刷新。</summary>
        private System.Windows.Threading.DispatcherTimer? _clockTimer;

        public LoginDialog()
        {
            InitializeComponent();
            Loaded += async (_, _) => await WindowAnimation.FadeInAsync(this);
            LoadSavedCredentials();
            UsernameBox.Focus();
            StartFooterClock();
        }

        private void StartFooterClock()
        {
            UpdateFooter();
            _clockTimer = new System.Windows.Threading.DispatcherTimer
            {
                Interval = TimeSpan.FromSeconds(1),
            };
            _clockTimer.Tick += (_, _) => UpdateFooter();
            _clockTimer.Start();
        }

        private void UpdateFooter()
            => TxtFooter.Text = $"Ver:{Services.UpdateService.CurrentVersion} · by 小坤ovo · {DateTime.Now:HH:mm:ss}";

        private void LoadSavedCredentials()
        {
            var saved = LoginStorage.LoadCredentials();
            if (saved == null) return;

            UsernameBox.Text = saved.Username;
            // 🔴 2026-10-09：**不再回填密码**。
            //    密码已经不落盘（见 LoginStorage.SaveCredentials），这里没有可回填的值；
            //    老版本写下的 login.dat 里可能残留密码字段，这里也**刻意忽略** ——
            //    否则"历史遗留的密码"会在界面上复活，等于收紧了存储却没收紧展示面。
            //    勾选框改为表示「这台机器记住登录状态（token 免密）」，有 token 即点亮。
            RememberMe.IsChecked = !string.IsNullOrEmpty(saved.Token);
        }

        private void ShowStatus(string message, bool isError)
        {
            StatusBorder.Visibility = Visibility.Visible;
            StatusBorder.Background = new SolidColorBrush(
                (Color)ColorConverter.ConvertFromString(isError ? "#FEF2F2" : "#F0FDF4"));
            StatusIcon.Text = isError ? "\u2717" : "\u2713";
            StatusIcon.Foreground = new SolidColorBrush(
                (Color)ColorConverter.ConvertFromString(isError ? "#DC2626" : "#16A34A"));
            StatusText.Text = message;
            StatusText.Foreground = new SolidColorBrush(
                (Color)ColorConverter.ConvertFromString(isError ? "#DC2626" : "#16A34A"));
        }

        private void HideStatus() => StatusBorder.Visibility = Visibility.Collapsed;
        private void ShowError(string message) => ShowStatus(message, true);

        /// <summary>统一处理登录失败：识别自动封禁（code:'BANNED'）→ 显示带到期时间+申诉方式的文案，
        /// 并自动弹出**免登录**申诉对话框。其它错误按原样提示。</summary>
        private void HandleLoginFailure(JsonElement result, string fallback)
        {
            var code = result.TryGetProperty("code", out var codeEl) && codeEl.ValueKind == JsonValueKind.String
                ? codeEl.GetString() : null;
            var message = result.TryGetProperty("message", out var msgEl) && msgEl.ValueKind == JsonValueKind.String
                ? msgEl.GetString() : null;

            if (string.Equals(code, "BANNED", StringComparison.OrdinalIgnoreCase))
            {
                var username = UsernameBox.Text.Trim();
                ShowStatus(message ?? "账号已被封禁，暂无法登录", true);
                try
                {
                    var dlg = new AppealDialog(username);
                    dlg.Owner = this;
                    dlg.ShowDialog();
                }
                catch { /* 申诉框打不开也不能挡住用户看封禁原因 */ }
            }
            else
            {
                ShowStatus(message ?? fallback, true);
            }
        }

        private void SetLoading(bool loading)
        {
            _submitting = loading;          // 防重入的唯一真源，四个提交入口都查它
            LoginButton.IsEnabled = !loading;
            KeyLoginButton.IsEnabled = !loading;
            EmailLoginButton.IsEnabled = !loading;
            RegisterButton.IsEnabled = !loading;
            LoginButton.Content = loading ? "登录中..." : "登 录";
            KeyLoginButton.Content = loading ? "登录中..." : "密钥登录";
            EmailLoginButton.Content = loading ? "登录中..." : "邮箱登录";
            RegisterButton.Content = loading ? "注册中..." : "创建账户";
        }

        private void SwitchMode(LoginMode mode)
        {
            _currentMode = mode;
            HideStatus();
            ApplyModeHeight(mode);

            LoginPanel.Visibility = Visibility.Collapsed;
            KeyPanel.Visibility = Visibility.Collapsed;
            EmailPanel.Visibility = Visibility.Collapsed;
            RegisterPanel.Visibility = Visibility.Collapsed;

            LoginButton.Visibility = Visibility.Collapsed;
            KeyLoginButton.Visibility = Visibility.Collapsed;
            EmailLoginButton.Visibility = Visibility.Collapsed;
            RegisterButton.Visibility = Visibility.Collapsed;

            ResetTab(LoginTab, LoginTabText);
            ResetTab(KeyTab, KeyTabText);
            ResetTab(EmailTab, EmailTabText);
            ResetTab(RegisterTab, RegisterTabText);

            switch (mode)
            {
                case LoginMode.Password:
                    LoginPanel.Visibility = Visibility.Visible;
                    LoginButton.Visibility = Visibility.Visible;
                    ActivateTab(LoginTab, LoginTabText);
                    TitleText.Text = "欢迎回来";
                    SubText.Text = "登录你的账户以继续使用";
                    break;
                case LoginMode.Key:
                    KeyPanel.Visibility = Visibility.Visible;
                    KeyLoginButton.Visibility = Visibility.Visible;
                    ActivateTab(KeyTab, KeyTabText);
                    TitleText.Text = "密钥登录";
                    SubText.Text = "输入密钥即可自动登录";
                    break;
                case LoginMode.Email:
                    EmailPanel.Visibility = Visibility.Visible;
                    EmailLoginButton.Visibility = Visibility.Visible;
                    ActivateTab(EmailTab, EmailTabText);
                    TitleText.Text = "邮箱登录";
                    SubText.Text = "使用邮箱验证码快速登录";
                    break;
                case LoginMode.Register:
                    RegisterPanel.Visibility = Visibility.Visible;
                    RegisterButton.Visibility = Visibility.Visible;
                    ActivateTab(RegisterTab, RegisterTabText);
                    TitleText.Text = "创建账户";
                    SubText.Text = "注册新账户以使用完整功能";
                    break;
            }

            // 切模式后让表单区「长出来」。原先是从 Collapsed 直接变 Visible —— 硬闪一下，
            // 窗口高度又在同一帧变化，两个突变叠在一起会很跳。位移只给 6px（红线：位移 ≤16px）。
            var active = mode switch
            {
                LoginMode.Password => (FrameworkElement)LoginPanel,
                LoginMode.Key => KeyPanel,
                LoginMode.Email => EmailPanel,
                _ => RegisterPanel,
            };
            _ = Motion.RevealAsync(active, durationMs: 200, slideFrom: 6);
        }

        /// <summary>
        /// 四个页签内容高度差很大（注册页比密钥页高约 150px）。固定高度只能按最高那页留，
        /// 结果密码/密钥页上方空一大截，注册页却又贴边。这里按页签给窗口一个刚好够用的高度，
        /// 并保住窗口中心点，切换时不会上下乱跳。
        /// 数值来自实测：内容高 + 上下各约 36px 留白 + 窗口边框/标题栏 39px。
        /// </summary>
        private static int HeightForMode(LoginMode mode) => mode switch
        {
            // 卡片化之后外层多了「外框留白 16×2 + 卡片内衬 24/18」≈ 74px 的固定 chrome，
            // 四个模式都比原版（484/550/576/766）各加一档。
            // 实测值由 login_shot 探针逐模式量出来后定的，别凭感觉改。
            LoginMode.Key => 512,
            LoginMode.Email => 596,
            LoginMode.Register => 852,
            _ => 632,
        };

        private void ApplyModeHeight(LoginMode mode)
        {
            var target = HeightForMode(mode);
            var area = SystemParameters.WorkArea;
            // 小屏（工作区不够高）时退到能放下的高度，别把登录按钮顶出屏幕外
            if (target > area.Height - 20) target = (int)Math.Max(320, area.Height - 20);
            if (Math.Abs(Height - target) < 1) return;

            var center = Top + Height / 2.0;
            Height = target;
            var top = center - target / 2.0;
            Top = Math.Max(area.Top, Math.Min(top, area.Bottom - target));

            // 表单区是 ScrollViewer（小屏降高后注册页需要滚动才够放），
            // 切页后必须回到顶部，否则会停在上一页的滚动位置、看不见本页开头。
            Dispatcher.BeginInvoke(new Action(() => FormScroll.ScrollToTop()),
                System.Windows.Threading.DispatcherPriority.Loaded);
        }

        /// <summary>选中态：墨绿底线 + 主文字色加粗（下划线页签，不是实心药丸）。</summary>
        private void ActivateTab(Border tab, TextBlock text)
        {
            tab.Background = Brushes.Transparent;
            tab.BorderBrush = (Brush)FindResource("Theme.Accent");
            text.Foreground = (Brush)FindResource("Theme.TextPrimary");
            text.FontWeight = FontWeights.Bold;
        }

        private void ResetTab(Border tab, TextBlock text)
        {
            tab.Background = Brushes.Transparent;
            tab.BorderBrush = Brushes.Transparent;
            text.Foreground = (Brush)FindResource("Theme.TextTertiary");
            text.FontWeight = FontWeights.SemiBold;
        }

        // Tab clicks
        private void LoginTab_Click(object sender, MouseButtonEventArgs e) => SwitchMode(LoginMode.Password);
        private void KeyTab_Click(object sender, MouseButtonEventArgs e) => SwitchMode(LoginMode.Key);
        private void EmailTab_Click(object sender, MouseButtonEventArgs e) => SwitchMode(LoginMode.Email);
        private void RegisterTab_Click(object sender, MouseButtonEventArgs e) => SwitchMode(LoginMode.Register);

        // Enter key handlers
        private void TextBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key != Key.Enter) return;
            // 🔴 必须 Handled：否则冒泡到 Window_KeyDown 会被判成"再登录一次"，
            //    而且此时用户还没输密码就被提交了。Enter 在这里只做焦点下移。
            e.Handled = true;
            PasswordBox.Focus();
        }
        private void PasswordBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key != Key.Enter) return;
            e.Handled = true;   // 别再冒泡到 Window_KeyDown —— 那会让同一次回车提交两次
            Login_Click(sender!, e);
        }
        private void KeyBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key != Key.Enter) return;
            e.Handled = true;
            KeyLogin_Click(sender!, e);
        }
        private void CodeBox_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key != Key.Enter) return;
            e.Handled = true;
            EmailLogin_Click(sender!, e);
        }

        // 合规说明：此处原先会采集「操作系统版本 + 计算机名 + 用户名」并随登录请求上报，
        // 属于可识别到具体个人设备的敏感信息，已按要求彻底移除（不再采集、不再上报）。
        // 服务端对应字段 deviceInfo 已停止写入与展示。
        // ⚠️ ip_address 和上面是**两回事**，别一起删：它不是客户端上报的，而是服务端在登录时
        //    从请求来源（nginx 透传的 X-Forwarded-For）解析后写进登录日志（2026-09-29 恢复采集）。
        //    它仅开发者账号可查看，且只在服务端处理，客户端始终不上报任何网络地址信息。

        // ====== Password Login ======
        private async void Login_Click(object sender, RoutedEventArgs e)
        {
            var username = UsernameBox.Text.Trim();
            var password = PasswordBox.Password;
            if (string.IsNullOrEmpty(username) || string.IsNullOrEmpty(password))
            { ShowStatus("请输入用户名和密码", true); return; }

            if (_submitting) return;   // Enter 冒泡 / 连点 → 只发一次请求
            HideStatus(); SetLoading(true);
            try
            {
                var payload = JsonBody.Of(("username", username), ("password", password));
                // v1：受保护端点统一走 RequestSigner（握手 + HMAC 签名，见 Core.Net.RequestSigner）
                using var response = await Core.Net.RequestSigner.PostJsonAsync(httpClient, $"{serverUrl}/api/v1/login", payload);
                var json = await response.Content.ReadAsStringAsync();
                // 先判【请求有没有走到业务层】：CF 的挑战页是 403 + HTML，
                // 直接反序列化只会抛 JsonException，界面上就成了指不出原因的报错（见 ServerGate 注释）。
                Services.ServerGate.ThrowIfBlocked(response, json);
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (response.IsSuccessStatusCode && result.GetProperty("success").GetBoolean())
                {
                    var data = result.GetProperty("data");
                    var sig = data.TryGetProperty("signature", out var sigEl) ? sigEl.GetString() ?? "" : "";
                    var token = data.TryGetProperty("token", out var tokenEl) ? tokenEl.GetString() : null;
                    var userName = data.TryGetProperty("username", out var userEl) ? userEl.GetString() : null;
                    if (string.IsNullOrEmpty(token) || string.IsNullOrEmpty(userName))
                    {
                        ShowStatus("服务端返回数据不完整，请稍后重试", true);
                        return;
                    }
                                        var vipExp = "";
                                        var vipLv = "";
                    if (data.TryGetProperty("entitlement", out var entEl) && entEl.ValueKind == JsonValueKind.Object)
                    {
                        if (entEl.TryGetProperty("vipExpireAt", out var veEl) && veEl.ValueKind == JsonValueKind.String)
                            vipExp = veEl.GetString() ?? "";
                        if (entEl.TryGetProperty("vipLevel", out var vlEl) && vlEl.ValueKind == JsonValueKind.String)
                            vipLv = vlEl.GetString() ?? "";
                        if (entEl.TryGetProperty("licensed", out var lcEl) && lcEl.ValueKind == JsonValueKind.False)
                        {
                            var why = entEl.TryGetProperty("reason", out var rnEl) && rnEl.ValueKind == JsonValueKind.String ? rnEl.GetString() : null;
                            ShowStatus(string.IsNullOrEmpty(why) ? "授权不可用，请联系管理员" : why, true);
                            return;
                        }
                    }
                    CompleteLogin(token, userName, sig, vipExp, vipLv);
                }
                else
                {
                    HandleLoginFailure(result, "登录失败");
                }
            }
            catch (HttpRequestException) { ShowStatus("无法连接服务器，请检查网络", true); }
            catch (Services.ServerGateException gex) { ShowStatus(gex.Message, true); }
            catch (Exception ex) { ShowStatus($"登录失败: {ex.Message}", true); }
            finally { SetLoading(false); }
        }

        // ====== Key Login ======
        private async void KeyLogin_Click(object sender, RoutedEventArgs e)
        {
            var key = KeyBox.Text.Trim();
            if (string.IsNullOrEmpty(key)) { ShowStatus("请输入密钥", true); return; }

            if (_submitting) return;   // Enter 冒泡 / 连点 → 只发一次请求
            HideStatus(); SetLoading(true);
            try
            {
                var payload = JsonBody.Of(("key", key));
                using var response = await Core.Net.RequestSigner.PostJsonAsync(httpClient, $"{serverUrl}/api/v1/auth/key-login", payload);
                var json = await response.Content.ReadAsStringAsync();
                // 先判【请求有没有走到业务层】：CF 的挑战页是 403 + HTML，
                // 直接反序列化只会抛 JsonException，界面上就成了指不出原因的报错（见 ServerGate 注释）。
                Services.ServerGate.ThrowIfBlocked(response, json);
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (response.IsSuccessStatusCode && result.GetProperty("success").GetBoolean())
                {
                    var data = result.GetProperty("data");
                    var sig = data.TryGetProperty("signature", out var sigEl) ? sigEl.GetString() ?? "" : "";
                    var token = data.TryGetProperty("token", out var tokenEl) ? tokenEl.GetString() : null;
                    var userName = data.TryGetProperty("username", out var userEl) ? userEl.GetString() : null;
                    if (string.IsNullOrEmpty(token) || string.IsNullOrEmpty(userName))
                    {
                        ShowStatus("服务端返回数据不完整，请稍后重试", true);
                        return;
                    }
                                        var vipExp = "";
                                        var vipLv = "";
                    if (data.TryGetProperty("entitlement", out var entEl) && entEl.ValueKind == JsonValueKind.Object)
                    {
                        if (entEl.TryGetProperty("vipExpireAt", out var veEl) && veEl.ValueKind == JsonValueKind.String)
                            vipExp = veEl.GetString() ?? "";
                        if (entEl.TryGetProperty("vipLevel", out var vlEl) && vlEl.ValueKind == JsonValueKind.String)
                            vipLv = vlEl.GetString() ?? "";
                        if (entEl.TryGetProperty("licensed", out var lcEl) && lcEl.ValueKind == JsonValueKind.False)
                        {
                            var why = entEl.TryGetProperty("reason", out var rnEl) && rnEl.ValueKind == JsonValueKind.String ? rnEl.GetString() : null;
                            ShowStatus(string.IsNullOrEmpty(why) ? "授权不可用，请联系管理员" : why, true);
                            return;
                        }
                    }
                    CompleteLogin(token, userName, sig, vipExp, vipLv);
                }
                else
                {
                    HandleLoginFailure(result, "密钥登录失败");
                }
            }
            catch (HttpRequestException) { ShowStatus("无法连接服务器，请检查网络", true); }
            catch (Services.ServerGateException gex) { ShowStatus(gex.Message, true); }
            catch (Exception ex) { ShowStatus($"登录失败: {ex.Message}", true); }
            finally { SetLoading(false); }
        }

        // ====== Email Login ======
        private async void SendCode_Click(object sender, RoutedEventArgs e)
        {
            var email = EmailBox.Text.Trim();
            if (string.IsNullOrEmpty(email)) { ShowStatus("请输入邮箱", true); return; }

            HideStatus();
            try
            {
                var payload = JsonBody.Of(("email", email), ("type", "login"));
                using var content = new StringContent(payload, Encoding.UTF8, "application/json");
                using var response = await httpClient.PostAsync($"{serverUrl}/api/v1/auth/send-code", content);
                var json = await response.Content.ReadAsStringAsync();
                // 先判【请求有没有走到业务层】：CF 的挑战页是 403 + HTML，
                // 直接反序列化只会抛 JsonException，界面上就成了指不出原因的报错（见 ServerGate 注释）。
                Services.ServerGate.ThrowIfBlocked(response, json);
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (response.IsSuccessStatusCode && result.GetProperty("success").GetBoolean())
                {
                    ShowStatus("验证码已发送，请查收邮箱", false);
                    await StartCountdown(SendCodeBtn, 60);
                }
                else
                {
                    ShowStatus(result.GetProperty("message").GetString() ?? "发送失败", true);
                }
            }
            catch (HttpRequestException) { ShowStatus("无法连接服务器", true); }
            catch (Services.ServerGateException gex) { ShowStatus(gex.Message, true); }
            catch (Exception ex) { ShowStatus($"发送失败: {ex.Message}", true); }
        }

        private async void EmailLogin_Click(object sender, RoutedEventArgs e)
        {
            var email = EmailBox.Text.Trim();
            var code = CodeBox.Text.Trim();
            if (string.IsNullOrEmpty(email) || string.IsNullOrEmpty(code))
            { ShowStatus("请输入邮箱和验证码", true); return; }

            if (_submitting) return;   // Enter 冒泡 / 连点 → 只发一次请求
            HideStatus(); SetLoading(true);
            try
            {
                var payload = JsonBody.Of(("email", email), ("code", code));
                using var response = await Core.Net.RequestSigner.PostJsonAsync(httpClient, $"{serverUrl}/api/v1/auth/code-login", payload);
                var json = await response.Content.ReadAsStringAsync();
                // 先判【请求有没有走到业务层】：CF 的挑战页是 403 + HTML，
                // 直接反序列化只会抛 JsonException，界面上就成了指不出原因的报错（见 ServerGate 注释）。
                Services.ServerGate.ThrowIfBlocked(response, json);
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (response.IsSuccessStatusCode && result.GetProperty("success").GetBoolean())
                {
                    var data = result.GetProperty("data");
                    var sig = data.TryGetProperty("signature", out var sigEl) ? sigEl.GetString() ?? "" : "";
                    var token = data.TryGetProperty("token", out var tokenEl) ? tokenEl.GetString() : null;
                    var userName = data.TryGetProperty("username", out var userEl) ? userEl.GetString() : null;
                    if (string.IsNullOrEmpty(token) || string.IsNullOrEmpty(userName))
                    {
                        ShowStatus("服务端返回数据不完整，请稍后重试", true);
                        return;
                    }
                                        var vipExp = "";
                                        var vipLv = "";
                    if (data.TryGetProperty("entitlement", out var entEl) && entEl.ValueKind == JsonValueKind.Object)
                    {
                        if (entEl.TryGetProperty("vipExpireAt", out var veEl) && veEl.ValueKind == JsonValueKind.String)
                            vipExp = veEl.GetString() ?? "";
                        if (entEl.TryGetProperty("vipLevel", out var vlEl) && vlEl.ValueKind == JsonValueKind.String)
                            vipLv = vlEl.GetString() ?? "";
                        if (entEl.TryGetProperty("licensed", out var lcEl) && lcEl.ValueKind == JsonValueKind.False)
                        {
                            var why = entEl.TryGetProperty("reason", out var rnEl) && rnEl.ValueKind == JsonValueKind.String ? rnEl.GetString() : null;
                            ShowStatus(string.IsNullOrEmpty(why) ? "授权不可用，请联系管理员" : why, true);
                            return;
                        }
                    }
                    CompleteLogin(token, userName, sig, vipExp, vipLv);
                }
                else
                {
                    HandleLoginFailure(result, "登录失败");
                }
            }
            catch (HttpRequestException) { ShowStatus("无法连接服务器", true); }
            catch (Services.ServerGateException gex) { ShowStatus(gex.Message, true); }
            catch (Exception ex) { ShowStatus($"登录失败: {ex.Message}", true); }
            finally { SetLoading(false); }
        }

        // ====== Email Register ======
        private async void RegSendCode_Click(object sender, RoutedEventArgs e)
        {
            var email = RegEmailBox.Text.Trim();
            if (string.IsNullOrEmpty(email)) { ShowStatus("请输入邮箱", true); return; }

            HideStatus();
            try
            {
                var payload = JsonBody.Of(("email", email), ("type", "register"));
                using var content = new StringContent(payload, Encoding.UTF8, "application/json");
                using var response = await httpClient.PostAsync($"{serverUrl}/api/v1/auth/send-code", content);
                var json = await response.Content.ReadAsStringAsync();
                // 先判【请求有没有走到业务层】：CF 的挑战页是 403 + HTML，
                // 直接反序列化只会抛 JsonException，界面上就成了指不出原因的报错（见 ServerGate 注释）。
                Services.ServerGate.ThrowIfBlocked(response, json);
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (response.IsSuccessStatusCode && result.GetProperty("success").GetBoolean())
                {
                    ShowStatus("验证码已发送，请查收邮箱", false);
                    await StartCountdown(RegSendCodeBtn, 60);
                }
                else
                {
                    ShowStatus(result.GetProperty("message").GetString() ?? "发送失败", true);
                }
            }
            catch (HttpRequestException) { ShowStatus("无法连接服务器", true); }
            catch (Services.ServerGateException gex) { ShowStatus(gex.Message, true); }
            catch (Exception ex) { ShowStatus($"发送失败: {ex.Message}", true); }
        }

        private async void Register_Click(object sender, RoutedEventArgs e)
        {
            var email = RegEmailBox.Text.Trim();
            var code = RegCodeBox.Text.Trim();
            var username = RegUsernameBox.Text.Trim();
            var password = RegPasswordBox.Password;
            var confirm = RegConfirmBox.Password;

            if (string.IsNullOrEmpty(email) || string.IsNullOrEmpty(code) ||
                string.IsNullOrEmpty(username) || string.IsNullOrEmpty(password))
            { ShowStatus("请填写完整信息", true); return; }
            // 用户名规则（含「不能是纯数字」）—— 服务端 /api/auth/code-register 也会拦，
            // 这里先拦一道，让用户在点按钮的那一刻就看到原因，不用等一次往返。
            if (UsernameRule.Error(username) is string nameErr) { ShowStatus(nameErr, true); return; }
            if (password != confirm) { ShowStatus("两次密码不一致", true); return; }
            // 法务合规：必须先勾选同意条款
            if (RegAgree.IsChecked != true)
            { ShowStatus("请先阅读并同意《用户协议》《隐私政策》《免责声明》", true); return; }

            if (_submitting) return;   // Enter 冒泡 / 连点 → 只发一次请求
            HideStatus(); SetLoading(true);
            try
            {
                // agree / legalVersion 会落进 users.terms_accepted_at 与 legal_version，作为同意凭证
                var payload = JsonBody.Of(
                    ("email", email), ("code", code), ("username", username),
                    ("password", password), ("agree", true), ("legalVersion", LegalVersion));
                using var response = await Core.Net.RequestSigner.PostJsonAsync(httpClient, $"{serverUrl}/api/v1/auth/code-register", payload);
                var json = await response.Content.ReadAsStringAsync();
                // 先判【请求有没有走到业务层】：CF 的挑战页是 403 + HTML，
                // 直接反序列化只会抛 JsonException，界面上就成了指不出原因的报错（见 ServerGate 注释）。
                Services.ServerGate.ThrowIfBlocked(response, json);
                var result = JsonSerializer.Deserialize<JsonElement>(json);

                if (response.IsSuccessStatusCode && result.GetProperty("success").GetBoolean())
                {
                    var data = result.GetProperty("data");
                    var sig = data.TryGetProperty("signature", out var sigEl) ? sigEl.GetString() ?? "" : "";
                    var token = data.TryGetProperty("token", out var tokenEl) ? tokenEl.GetString() : null;
                    var userName = data.TryGetProperty("username", out var userEl) ? userEl.GetString() : null;
                    if (string.IsNullOrEmpty(token) || string.IsNullOrEmpty(userName))
                    {
                        ShowStatus("服务端返回数据不完整，请稍后重试", true);
                        return;
                    }
                                        var vipExp = "";
                                        var vipLv = "";
                    if (data.TryGetProperty("entitlement", out var entEl) && entEl.ValueKind == JsonValueKind.Object)
                    {
                        if (entEl.TryGetProperty("vipExpireAt", out var veEl) && veEl.ValueKind == JsonValueKind.String)
                            vipExp = veEl.GetString() ?? "";
                        if (entEl.TryGetProperty("vipLevel", out var vlEl) && vlEl.ValueKind == JsonValueKind.String)
                            vipLv = vlEl.GetString() ?? "";
                        if (entEl.TryGetProperty("licensed", out var lcEl) && lcEl.ValueKind == JsonValueKind.False)
                        {
                            var why = entEl.TryGetProperty("reason", out var rnEl) && rnEl.ValueKind == JsonValueKind.String ? rnEl.GetString() : null;
                            ShowStatus(string.IsNullOrEmpty(why) ? "授权不可用，请联系管理员" : why, true);
                            return;
                        }
                    }
                    CompleteLogin(token, userName, sig, vipExp, vipLv);
                }
                else
                {
                    ShowStatus(result.GetProperty("message").GetString() ?? "注册失败", true);
                }
            }
            catch (HttpRequestException) { ShowStatus("无法连接服务器", true); }
            catch (Exception ex) { ShowStatus($"注册失败: {ex.Message}", true); }
            finally { SetLoading(false); }
        }

        // ====== Common ======
        private async void CompleteLogin(string token, string username, string signature = "", string vipExpire = "", string vipLevel = "")
        {
            if (_isLoggingIn) return;
            _isLoggingIn = true;

            AuthToken = token;
            LoggedInUsername = username;
            LoggedInVipExpire = vipExpire;
            LoggedInVipLevel = vipLevel;
            IsLoggedIn = true;

            // 登录成功后做一次环境上报（失败静默、超时 15s 且全程吞异常，绝不阻塞主流程，
            // 也绝不强制下线 —— 即使上报触发了自动封禁，也只在下次登录时生效，避免变砖）。
            _ = Task.Run(() => { try { EnvReportService.ReportIfNeeded(token); } catch { } });

            // 无论是否勾选「记住登录状态」，token 都进内存（AuthSession），
            // 否则闸门/EDL 会因取不到 token 直接 noauth → 引导根本不发。
            AuthSession.Set(username, token, DateTime.UtcNow.AddHours(24));
            // 🔴 这里**不再传密码**（2026-10-09）：登录态只落 用户名 + token，密码不落盘。
            //    勾选框现在控制的是「要不要把登录态写进磁盘」。
            LoginStorage.SaveCredentials(username, token,
                DateTime.UtcNow.AddHours(24), persistLogin: RememberMe.IsChecked == true);

            try
            {
                await WindowAnimation.FadeOutAsync(this);
                Hide();

                var downloadDialog = new DownloadDialog();
                downloadDialog.WindowStartupLocation = WindowStartupLocation.CenterScreen;
                downloadDialog.ShowDialog();

                var mainWindow = new MainWindow();
                mainWindow.Show();
                await WindowAnimation.FadeInAsync(mainWindow);
                mainWindow.SetUserInfo(username, signature, vipExpire, vipLevel);

                // 关闭主窗口后的进程退出由 MainWindow.Closed 统一处理（避免重复注册 Shutdown）
                Application.Current.MainWindow = mainWindow;

                // 关键：本窗口由 App.OnStartup 以 ShowDialog 方式模态打开，仅 Hide() 不会让
                // ShowDialog 返回，WPF 会持续禁用其它顶层窗口（表现为主界面点击无响应）。
                // 此处真正关闭登录窗口以解除模态；IsLoggedIn 已为 true，OnClosed 不会触发 Shutdown。
                Close();
            }
            catch (Exception ex)
            {
                App.WriteCrashLog("LoginDialog.CompleteLogin", ex);
                try
                {
                    MessageBox.Show($"启动失败: {ex.Message}\n\n{ex.InnerException?.Message}",
                        "Kun Client", MessageBoxButton.OK, MessageBoxImage.Error);
                }
                catch { }
                Application.Current.Shutdown();
            }
        }

        private async Task StartCountdown(Button btn, int seconds)
        {
            var original = btn.Content?.ToString() ?? "";
            btn.IsEnabled = false;
            try
            {
                for (int i = seconds; i > 0; i--)
                {
                    btn.Content = $"{i}s";
                    await Task.Delay(1000);
                    if (!IsLoaded) return;
                }
            }
            finally
            {
                if (IsLoaded)
                {
                    btn.Content = original;
                    btn.IsEnabled = true;
                }
            }
        }

        private void Cancel_Click(object sender, RoutedEventArgs e) => Application.Current.Shutdown();
        private void CloseButton_Click(object sender, RoutedEventArgs e) => Application.Current.Shutdown();

        protected override void OnClosed(EventArgs e)
        {
            // 不关掉定时器，窗口关了它还在 Tick，白白占着一个 Dispatcher 计时器
            _clockTimer?.Stop();
            base.OnClosed(e);
            if (!IsLoggedIn)
                Application.Current.Shutdown();
        }

        private void Window_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ChangedButton == MouseButton.Left) DragMove();
        }

        private void Window_KeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter && !_isLoggingIn)
            {
                switch (_currentMode)
                {
                    case LoginMode.Password: Login_Click(sender!, e); break;
                    case LoginMode.Key: KeyLogin_Click(sender!, e); break;
                    case LoginMode.Email: EmailLogin_Click(sender!, e); break;
                    case LoginMode.Register: Register_Click(sender!, e); break;
                }
            }
        }
    }
}
