using System;
using System.Collections.Generic;
using System.Linq;

namespace KunFlashTool.App.Services;

/// <summary>
/// 最轻的一层「设备状态广播」。
///
/// 为什么要有它：设备探测各页**自己**做（主页用 adb/fastboot 进程，EDL 页走自己的通道），
/// 外壳左栏「设备信息」卡要显示同一份结论，但**不能让外壳自己再起一套轮询** ——
/// 多一个进程探测既费时，又容易和页面正在跑的 adb/fastboot 互相打断（同一台设备两条命令并发）。
///
/// 所以做成单向汇报：**页探测完往这里说一句，外壳照着显示**。
/// 页面不需要知道外壳存在（静态类，无窗口引用），外壳也不需要知道谁在探测。
/// </summary>
public static class DeviceStateHub
{
    /// <summary>设备信息卡里的一行键值（参考稿那 6 行就是这个形状）。</summary>
    public readonly record struct Row(string Key, string Value);

    /// <summary>一次设备状态的快照。<paramref name="Device"/> 是机型/序列号之类的摘要，
    /// <paramref name="Mode"/> 是「ADB 模式 / Fastboot 模式 / EDL 模式 (9008) / 未连接」这类人话结论，
    /// <paramref name="Rows"/> 是补充键值行（可为空）。</summary>
    public readonly record struct State(string Device, string Mode, IReadOnlyList<Row> Rows);

    /// <summary>最近一次汇报（没人汇报过就是 null → 外壳显示占位文案）。</summary>
    public static State? Current { get; private set; }

    /// <summary>状态变化。订阅者（外壳左栏）自己负责切回 UI 线程。</summary>
    public static event Action<State>? Changed;

    /// <summary>页面探测完调它。空串按「未知」处理，避免出现空白读数。</summary>
    public static void Report(string? device, string? mode, params Row[] rows)
    {
        var d = string.IsNullOrWhiteSpace(device) ? "未知设备" : device.Trim();
        var m = string.IsNullOrWhiteSpace(mode) ? "未检测" : mode.Trim();
        // 值全空的行直接丢掉：宁可少一行，也不要在读数区里留一排「-」
        var rs = (rows ?? Array.Empty<Row>())
                 .Where(r => !string.IsNullOrWhiteSpace(r.Value) && r.Value.Trim() is not "-" and not "—")
                 .ToArray();
        var s = new State(d, m, rs);
        if (Current.HasValue && SameState(Current.Value, s)) return;   // 5s 一轮轮询，同值不重放
        Current = s;
        try { Changed?.Invoke(s); } catch { /* 广播失败不能反过来影响探测 */ }
    }

    private static bool SameState(State a, State b)
    {
        if (a.Device != b.Device || a.Mode != b.Mode || a.Rows.Count != b.Rows.Count) return false;
        for (int i = 0; i < a.Rows.Count; i++)
            if (a.Rows[i] != b.Rows[i]) return false;
        return true;
    }
}
