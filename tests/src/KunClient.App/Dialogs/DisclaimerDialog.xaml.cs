using System.ComponentModel;
using System.Windows;
using System.Windows.Input;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// 使用条款 + 隐私说明的确认窗口。两种用法：
    /// <list type="bullet">
    /// <item><c>enforce = true</c>（首次启动）：点「不同意」或直接关窗都会退出程序，
    /// 未同意不留任何口子 —— 这是合规上的「告知—同意」环节。</item>
    /// <item><c>enforce = false</c>（设置页重新查看）：只做展示，关掉不影响使用。</item>
    /// </list>
    /// 以前这个窗口是**死代码**（全项目没有任何地方调用），首次启动从未弹过；
    /// 现在由 <c>App.OnStartup</c> 在登录框之前调用。
    /// </summary>
    public partial class DisclaimerDialog : Window
    {
        /// <summary>
        /// 当前条款文本版本。**改动了免责/协议/隐私内容就要 +1**，
        /// 这样已同意的老用户下次启动会被要求重新阅读并同意
        /// （与 <c>AppSettings.LegalAcceptedVersion</c> 比较）。
        /// </summary>
        public const int LegalVersion = 5;

        private readonly bool _enforce;

        /// <summary>是否点了「同意」/「我已阅读」。</summary>
        public bool IsAgreed { get; private set; }

        public DisclaimerDialog(bool enforce = true)
        {
            InitializeComponent();
            _enforce = enforce;

            if (!enforce)
            {
                // 重新查看模式：不提供「不同意」，也不让关窗退出程序
                BtnDisagree.Visibility = Visibility.Collapsed;
                BtnAgree.Content = "我已阅读";
                TxtFooterHint.Text = "本窗口仅供查看，关闭不影响使用";
            }

            // 打开时必须停在最顶部：实测初次显示时正文会被自动滚掉约 120px，
            // 顶部的大图标与「请仔细阅读以下…」提示段直接看不见，
            // 而这是一份需要用户声明「已阅读」的法规文本，不能一上来就从中间开始。
            // 用 ApplicationIdle 排在首次布局/焦点处理之后，否则会被随后的滚动覆盖掉。
            Loaded += (_, _) => Dispatcher.BeginInvoke(
                new Action(() => DisclaimerScroll.ScrollToTop()),
                System.Windows.Threading.DispatcherPriority.ApplicationIdle);
        }

        private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
        {
            if (e.LeftButton == MouseButtonState.Pressed)
                DragMove();
        }

        private void Agree_Click(object sender, RoutedEventArgs e)
        {
            IsAgreed = true;
            DialogResult = true;
            Close();
        }

        private void Disagree_Click(object sender, RoutedEventArgs e)
        {
            IsAgreed = false;
            // DialogResult 在这里显式设置（它本身就会关窗）。
            // 不要放到 OnClosing 里设：那会在 Closing 期间再触发一次 Close()，属于重入。
            DialogResult = false;
        }

        private void OnClosing(object? sender, CancelEventArgs e)
        {
            // 首次启动且未同意 → 退出程序，不进登录 / 主界面
            if (!IsAgreed && _enforce) Application.Current.Shutdown();
        }
    }
}
