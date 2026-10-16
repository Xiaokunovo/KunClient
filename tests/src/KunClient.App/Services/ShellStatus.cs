using System;
using System.Windows;

namespace KunFlashTool.App.Services;

/// <summary>
/// 外壳状态中枢（底部双进度条 + 分段状态栏的唯一数据源）。
///
/// <para>
/// 版式参考稿里，进度条与状态栏**属于窗口外壳**，不属于任何一页：
/// 无论当前在哪个页签，底部那一行读数（任务 / 分区名 / 速度 / 总大小 / 进度 / 设备）都在，
/// 而且切页签时不会各自闪没。所以页面不再自己画底部状态栏，改为往这里上报。
/// </para>
///
/// <para>
/// ⚠️ 上报是**跨线程**调用的（刷写进度从工作线程推上来），所以事件用
/// <see cref="Application.Current"/> 的 Dispatcher 转发；页面侧不需要自己 Invoke。
/// </para>
/// </summary>
public static class ShellStatus
{
    /// <summary>任一读数变化。外壳订阅它刷新界面。</summary>
    public static event Action? Changed;

    /// <summary>无读数时的占位符 —— 与参考稿一致（未连接设备时状态栏显示「-」而不是空白）。</summary>
    public const string Dash = "-";

    private static string _device = Dash;
    private static string _mode = Dash;
    private static string _task = Dash;
    private static string _partition = Dash;
    private static string _speed = Dash;
    private static string _totalSize = Dash;
    private static string _hint = "就绪";
    private static string _elapsed = "";
    private static double _percent;
    private static double _partitionPercent;
    /// <summary>2026-10-02 新增：第二条（分区）进度条是否参与显示。
    /// 调用方传 partitionPercent = -1 表示「与总进度相同（单分区写入/擦除），这条别画」。</summary>
    private static bool _partitionVisible = true;

    /// <summary>设备名（通常是序列号）。状态栏最右一段。</summary>
    public static string Device => _device;
    /// <summary>设备模式（Bootloader / EDL / ADB …）。</summary>
    public static string Mode => _mode;
    /// <summary>当前任务名（读分区表 / 解析固件 / 写入分区 …）。</summary>
    public static string Task => _task;
    /// <summary>当前分区名。</summary>
    public static string Partition => _partition;
    /// <summary>速度读数（已格式化，如 "18.4 MB/s"）。</summary>
    public static string Speed => _speed;
    /// <summary>总大小读数（已格式化，如 "2.0 GB"）。</summary>
    public static string TotalSize => _totalSize;
    /// <summary>当前提示（外壳左栏与状态栏共用）。</summary>
    public static string Hint => _hint;
    /// <summary>总进度百分比（0-100）。</summary>
    public static double Percent => _percent;
    /// <summary>当前分区进度百分比（0-100）；与总进度相同（单分区操作）时不必单独显示。</summary>
    public static double PartitionPercent => _partitionPercent;
    /// <summary>第二条（分区）进度条是否显示。调用方上报 partitionPercent = -1 时为 false ——
    /// 2026-10-02 修复：原先 -1 会被 Clamp 成 0，第二条进度条卡在 0% 反而更显眼。</summary>
    public static bool PartitionBarVisible => _partitionVisible;
    /// <summary>已耗时读数（如 "00:12"）；没有计时的操作留空。</summary>
    public static string Elapsed => _elapsed;

    /// <summary>已耗时读数（外壳把它并进「任务」段显示，参考稿没有单独的耗时槽位）。</summary>
    public static void SetElapsed(string? elapsed)
    {
        var value = elapsed ?? "";
        if (value == _elapsed) return;
        _elapsed = value;
        Raise();
    }

    /// <summary>设备信息变化（连接/断开/切模式）。空值 = 保持原值。</summary>
    public static void SetDevice(string? device = null, string? mode = null, string? hint = null)
    {
        var dirty = false;
        if (device != null && device != _device) { _device = Blank(device); dirty = true; }
        if (mode != null && mode != _mode) { _mode = Blank(mode); dirty = true; }
        if (hint != null && hint != _hint) { _hint = Blank(hint); dirty = true; }
        if (dirty) Raise();
    }

    /// <summary>开始一个新任务：清空上一轮的进度/分区/速度读数，并把进度条归零。</summary>
    public static void BeginTask(string task)
    {
        _task = Blank(task);
        _partition = Dash;
        _speed = Dash;
        _totalSize = Dash;
        _percent = 0;
        _partitionVisible = true;   // 新任务默认显示第二条，首个 partition 上报可再收起
        Raise();
    }

    /// <summary>任务级读数（阶段 / 明细 / 速度 / 总量 / 百分比）。只传要改的字段。</summary>
    public static void Report(string? task = null, string? partition = null, string? speed = null,
        string? totalSize = null, double? percent = null, double? partitionPercent = null)
    {
        var dirty = false;
        if (task != null && task != _task) { _task = Blank(task); dirty = true; }
        if (partition != null && partition != _partition) { _partition = Blank(partition); dirty = true; }
        if (speed != null && speed != _speed) { _speed = Blank(speed); dirty = true; }
        if (totalSize != null && totalSize != _totalSize) { _totalSize = Blank(totalSize); dirty = true; }
        if (percent.HasValue && Math.Abs(percent.Value - _percent) >= 0.01)
        {
            _percent = Math.Clamp(percent.Value, 0, 100);
            dirty = true;
        }
        if (partitionPercent.HasValue && Math.Abs(partitionPercent.Value - _partitionPercent) >= 0.01)
        {
            // -1 =「单分区操作，第二条进度条别显示」（见 PartitionBarVisible）。负值直接吃掉，
            // 绝不能再落到下面的 Clamp（否则 -1 → 0，出现卡死的 0% 第二条）。
            if (partitionPercent.Value < 0)
            {
                if (_partitionVisible) { _partitionVisible = false; dirty = true; }
            }
            else
            {
                if (!_partitionVisible) { _partitionVisible = true; dirty = true; }
                _partitionPercent = Math.Clamp(partitionPercent.Value, 0, 100);
                dirty = true;
            }
        }
        if (dirty) Raise();
    }

    /// <summary>任务收尾：进度停在 100%，但任务名/分区名保留（用户还要看「刚才做了什么」）。</summary>
    public static void EndTask()
    {
        _speed = Dash;
        _percent = 100;
        _partitionPercent = 100;
        _partitionVisible = true;
        Raise();
    }

    /// <summary>回到空闲态（设备断开 / 切页签时调用）。</summary>
    public static void Reset()
    {
        _task = Dash;
        _partition = Dash;
        _speed = Dash;
        _totalSize = Dash;
        _elapsed = "";
        _percent = 0;
        _partitionPercent = 0;
        _partitionVisible = true;
        Raise();
    }

    private static string Blank(string value) => string.IsNullOrWhiteSpace(value) ? Dash : value;

    private static void Raise()
    {
        var handler = Changed;
        if (handler == null) return;
        var app = Application.Current;
        if (app == null) { handler(); return; }
        if (app.Dispatcher.CheckAccess()) handler();
        else app.Dispatcher.BeginInvoke(new Action(() => handler()));
    }
}
