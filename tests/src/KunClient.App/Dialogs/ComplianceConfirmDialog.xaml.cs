using System.Windows;

namespace KunFlashTool.App.Dialogs
{
    /// <summary>
    /// 高风险操作的合规确认对话框。
    /// 设计目的：涉及设备防盗机制（如 FRP 出厂重置保护）的操作不能一点就跑，
    /// 必须先让用户显式声明设备归属，否则本工具可能被用于处理来历不明的设备。
    /// 未勾选确认时「确认执行」保持禁用，拒绝执行不留口子。
    /// </summary>
    public partial class ComplianceConfirmDialog : Window
    {
        public ComplianceConfirmDialog(string actionName, string riskText)
        {
            InitializeComponent();
            TxtTitle.Text = "合规确认 · " + actionName;
            TxtRisk.Text = riskText;
        }

        private void ChkOwn_Changed(object sender, RoutedEventArgs e)
        {
            // 两个确认项都勾上才放行：
            //   ChkOwn   —— 设备归属声明（原有）
            //   ChkLegal —— 已知晓可能构成刑事犯罪（2026-09-24 合规加强）
            // 双重确认不是走过场：FRP 等防盗机制一旦被用于他人设备，法律后果由操作者承担。
            BtnOk.IsEnabled = ChkOwn.IsChecked == true && ChkLegal.IsChecked == true;
        }

        private void Ok_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = true;
            Close();
        }

        private void Cancel_Click(object sender, RoutedEventArgs e)
        {
            DialogResult = false;
            Close();
        }
    }
}
