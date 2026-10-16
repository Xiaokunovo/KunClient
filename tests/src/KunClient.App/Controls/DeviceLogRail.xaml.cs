using System;
using System.Windows.Controls;
using KunFlashTool.App.Services;

namespace KunFlashTool.App.Controls;

/// <summary>
/// 外壳共用左栏「设备信息 + 日志」。
/// <para>
/// 参考稿里这一列是三个页签共用的一列，所以它属于外壳而不是页面；
/// 页面自带左栏时（Qualcomm / FASTBOOT / Payload 三页），外壳把整列收起（见 MainWindow.UpdateRailLayout）。
/// </para>
/// <para>
/// 数据来源分两路：<br/>
///   · 设备 / 模式 —— <see cref="DeviceStateHub"/>（**谁探测谁汇报**，外壳不自己起轮询）；<br/>
///   · 任务 / 进度 —— <see cref="ShellStatus"/>（页面在跑的长操作往这里上报）。<br/>
/// 日志框交给 PageLog 当全局镜像 —— 页面不需要认识这个控件。
/// </para>
/// </summary>
public partial class DeviceLogRail : UserControl
{
    public DeviceLogRail()
    {
        InitializeComponent();

        Loaded += (_, _) =>
        {
            ShellStatus.Changed += SyncStatus;
            DeviceStateHub.Changed += SyncDevice;
            SyncStatus();
            if (DeviceStateHub.Current is { } st) SyncDevice(st);
        };

        // 静态事件必须摘：外壳是长生命周期控件，不摘会在关闭后继续被回调
        Unloaded += (_, _) =>
        {
            ShellStatus.Changed -= SyncStatus;
            DeviceStateHub.Changed -= SyncDevice;
        };
    }

    /// <summary>全局日志镜像的落点（PageLog.GlobalMirror）。</summary>
    public RichTextBox LogBox => TxtRailLog;

    private void SyncDevice(DeviceStateHub.State st)
    {
        try
        {
            TxtDevice.Text = st.Device;
            TxtMode.Text = st.Mode;
        }
        catch
        {
            // 外壳读数不该影响任何功能路径：控件卸载后回调仍可能到（事件晚一步），忽略
        }
    }

    private void SyncStatus()
    {
        try
        {
            TxtTask.Text = ShellStatus.Task;
            TxtHint.Text = ShellStatus.Hint;
            TxtPercent.Text = $"{ShellStatus.Percent:F0}%";
        }
        catch
        {
            // 同上：读数刷新失败不影响功能
        }
    }
}
