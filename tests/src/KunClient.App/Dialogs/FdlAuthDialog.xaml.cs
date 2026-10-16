using System;
using System.Windows;
using System.Windows.Input;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// FDL Auth —— blob（设备 token，只读 + 复制）与 sig 的输入窗，点「写入」即代表用户确认。
    ///
    /// 2026-10-03 起接入平台 FDL 计算接口：点「计算签名」会用**平台账号**把 blob 提交给
    /// <c>tool_calc</c>，签名由平台算好返回（<c>tool_login</c> 换 24 小时受限令牌，401 自动重登）。
    /// 手工粘贴 sig 的老路**保留** —— 平台不可用时用户仍可粘贴别人算好的结果。
    /// </summary>
    public partial class FdlAuthDialog : Window
    {
        private readonly FdlApiClient _api = new();

        public FdlAuthDialog() : this("") { }

        public FdlAuthDialog(string blob)
        {
            InitializeComponent();
            TxtBlob.Text = blob ?? "";
            // 上次用过的平台账号回填。
            // 🔴 **只回填用户名，不回填密码**（2026-10-09）：密码已不再落盘（FdlAccountStore），
            // 这里若沿用旧字段会得到空串 —— 那正是期望行为：令牌还在就直接能用，
            // 令牌过期了就请用户重输一次，而不是把长期有效的密码留在磁盘上换"少输一次"。
            try
            {
                var acc = FdlAccountStore.Load();
                if (acc != null)
                {
                    TxtFdlUser.Text = acc.Username;
                    // 拿保存的令牌把会话续上；续不上（没存过 / 已过期）就不动密码框。
                    if (_api.RestoreSession(acc.Username, acc.Token, acc.ExpiryUtc))
                        SetFdlStatus("已沿用上次的平台登录（令牌有效期内无需重填密码）");
                }
            }
            catch { }
            SetAccountBar(TxtFdlUser.Text, null);
            UpdateHint();
        }

        private string User => (TxtFdlUser?.Text ?? "").Trim();
        private string Pass => TxtFdlPass?.Password ?? "";

        private void SetFdlStatus(string text, bool error = false)
        {
            if (TxtFdlStatus == null) return;
            TxtFdlStatus.Text = text;
            TxtFdlStatus.Foreground = (System.Windows.Media.Brush)Application.Current
                .FindResource(error ? "Theme.Error" : "Theme.TextTertiary");
        }

        /// <summary>
        /// 写那条**常驻**的账户条。
        /// 🔴 余额、账号、令牌余量必须与「操作状态」分开：状态行每次计算/报错都会被覆盖，
        ///    混在一起的话算完一次签名，余额就没了 —— 用户要的是"登录后一直看得到余额"。
        /// </summary>
        private void SetAccountBar(string username, double? balance)
        {
            if (TxtFdlAccount == null) return;
            var name = string.IsNullOrWhiteSpace(username) ? "未登录" : username;
            var money = balance.HasValue ? $"　余额 {balance.Value:0.##} 点" : "";
            var token = _api.TokenHoursLeft > 0.05 ? $"　令牌剩余 {_api.TokenHoursLeft:0.#} 小时" : "";
            TxtFdlAccount.Text = $"{name}{money}{token}";
        }

        private async void BtnFdlBalance_Click(object sender, RoutedEventArgs e)
        {
            if (BtnFdlBalance == null) return;
            BtnFdlBalance.IsEnabled = false;
            SetFdlStatus("正在刷新余额…");
            try
            {
                var r = await _api.LoginAsync(User, Pass).ConfigureAwait(true);
                if (r.Ok)
                {
                    FdlAccountStore.Save(User, "", r.Token, r.TokenExpiryUtc);
                    SetAccountBar(r.Username, r.Balance);
                    SetFdlStatus("余额已刷新");
                }
                else
                {
                    SetAccountBar(User, null);
                    SetFdlStatus("刷新失败：" + r.Error, true);
                }
            }
            catch (Exception ex) { SetFdlStatus("刷新异常：" + ex.Message, true); }
            finally { BtnFdlBalance.IsEnabled = true; }
        }

        private async void BtnFdlLogin_Click(object sender, RoutedEventArgs e)
        {
            if (BtnFdlLogin == null) return;
            BtnFdlLogin.IsEnabled = false;
            try
            {
                var r = await _api.LoginAsync(User, Pass).ConfigureAwait(true);
                if (r.Ok)
                {
                    FdlAccountStore.Save(User, "", r.Token, r.TokenExpiryUtc);
                    SetAccountBar(r.Username, r.Balance);
                    SetFdlStatus($"已登录：{r.Username}，余额 {r.Balance:0.##} 点");
                }
                else
                {
                    SetAccountBar(User, null);
                    SetFdlStatus("登录失败：" + r.Error, true);
                }
            }
            catch (Exception ex) { SetFdlStatus("登录异常：" + ex.Message, true); }
            finally { BtnFdlLogin.IsEnabled = true; }
        }

        private async void BtnFdlCalc_Click(object sender, RoutedEventArgs e)
        {
            if (BtnFdlCalc == null || TxtBlob == null || TxtSig == null) return;
            BtnFdlCalc.IsEnabled = false;
            SetFdlStatus("正在提交 blob 给平台计算…");
            try
            {
                var r = await _api.CalcAsync(TxtBlob.Text, User, Pass).ConfigureAwait(true);
                if (r.Ok)
                {
                    TxtSig.Text = r.Sig;
                    TxtSig.CaretIndex = TxtSig.Text.Length;
                    // CalcResult 不带令牌（它可能在内部因 401 自动重登过），
                    // 所以这里取的是 **重登之后** 的最新会话，而不是登录时那枚。
                    FdlAccountStore.Save(User, "", _api.Token, _api.TokenExpiryUtc);
                    // 余额回填常驻账户条（接口每次都会返回扣点后的余额）
                    SetAccountBar(User, r.Balance);
                    SetFdlStatus($"计算完成　本次消耗 {r.Cost:0.##} 点　耗时 {r.Elapsed:0.##} 秒");
                }
                else
                {
                    SetFdlStatus("计算失败：" + r.Error, true);
                }
            }
            catch (Exception ex) { SetFdlStatus("计算异常：" + ex.Message, true); }
            finally { BtnFdlCalc.IsEnabled = true; }
        }

        /// <summary>blob（设备 token）原文。</summary>
        public string BlobText
        {
            get => TxtBlob.Text;
            set => TxtBlob.Text = value ?? "";
        }

        /// <summary>用户粘贴的 sig 原文。</summary>
        public string SigText
        {
            get => TxtSig.Text;
            set => TxtSig.Text = value ?? "";
        }

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.ButtonState == MouseButtonState.Pressed) DragMove();
        }

        private void BtnCopy_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                if (string.IsNullOrEmpty(TxtBlob.Text))
                {
                    SetHint("blob 是空的，没有可复制的内容");
                    return;
                }
                Clipboard.SetText(TxtBlob.Text);
                SetHint($"已把 blob 复制到剪贴板（{TxtBlob.Text.Length} 字符）");
            }
            catch (Exception ex)
            {
                SetHint("复制失败：" + ex.Message);
            }
        }

        private void BtnPaste_Click(object sender, RoutedEventArgs e)
        {
            try
            {
                if (!Clipboard.ContainsText())
                {
                    SetHint("剪贴板里没有文本");
                    return;
                }
                TxtSig.Text = Clipboard.GetText().Trim();
                TxtSig.CaretIndex = TxtSig.Text.Length;
            }
            catch (Exception ex)
            {
                SetHint("粘贴失败：" + ex.Message);
            }
        }

        private void TxtSig_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e) => UpdateHint();

        private void SetHint(string text)
        {
            if (TxtHint != null) TxtHint.Text = text;
        }

        private void UpdateHint()
        {
            // InitializeComponent 期间 TextChanged 就会触发，这时 TxtHint 还没赋值
            if (TxtHint == null) return;

            var sig = SignCodec.TryParseBytes(TxtSig.Text);
            if (sig == null || sig.Length == 0)
            {
                SetHint("粘贴 sig 后点「写入」；十六进制 / Base64 都行");
                return;
            }

            var hexLen = SignCodec.CleanHex(TxtSig.Text).Length;
            SetHint(hexLen > 0
                ? $"sig 已解析：{sig.Length} 字节（按十六进制清洗后 {hexLen} 位）"
                : $"sig 已解析：{sig.Length} 字节（按 Base64）");
        }

        private void BtnWrite_Click(object sender, RoutedEventArgs e)
        {
            var sig = SignCodec.TryParseBytes(TxtSig.Text);
            if (sig == null || sig.Length == 0)
            {
                SetHint("sig 是空的，或格式不认识 —— 请粘贴签名数据后再点「写入」");
                return;
            }

            DialogResult = true;
        }

        private void BtnCancel_Click(object sender, RoutedEventArgs e) => DialogResult = false;
    }
}
