using System.Diagnostics;
using System.Reflection;
using System.Windows;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Pages
{
    public partial class AboutPage : BasePage
    {
        public AboutPage()
        {
            InitializeComponent();
            var version = Assembly.GetExecutingAssembly().GetName().Version;
            // 🔴 这里**不要**再加 "Ver:" 前缀 —— 左边那格的标签已经写了「版本:」，
            //    加上就渲染成「版本: Ver:1.1.1」，前缀重复。
            //    （当初为了和标题栏/登录框统一前缀而加过，但那几个地方没有独立标签，是句子里的一部分；
            //      本页是「标签: 值」两列读数，值里不该再带前缀。）
            TxtVersion.Text = $"{version?.Major ?? 1}.{version?.Minor ?? 0}.{version?.Build ?? 0}";
        }
    }
}
