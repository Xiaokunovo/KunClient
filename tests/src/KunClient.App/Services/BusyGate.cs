using System;
using System.Threading;
using System.Windows.Controls;

namespace KunFlashTool.App.Services;

/// <summary>
/// 刷机类页面的**防重入闸门**。
/// <para>
/// 刷机 / 擦除 / 一键刷写 / 备份是**不可并发**的：双击一次会同时开出两个写会话 ——
/// 轻则进度与日志互相覆盖、状态栏读数打架，重则两个会话抢同一条 USB 通道把分区写坏。
/// </para>
/// <para>
/// 之前只有 <c>CloudDriverPage</c> / <c>CommandConsolePage</c> / <c>XiaomiEdlPage</c>
/// 用了 <c>Button.IsEnabled = false</c> 这种**逐按钮**的写法，而三个主刷写页
/// （EDL / FASTBOOT / Payload）**完全没有闸门**。逐按钮写法还会漏：
/// 新加一个按钮就等于新开一个漏洞，所以统一收口到这里。
/// </para>
/// <para>
/// 用法：<c>if (!_gate.TryEnter()) { 提示"上一个操作还在进行中"; return; }</c>
/// …… <c>finally { _gate.Exit(); }</c>。非 UI 线程也能安全调用（Interlocked）。
/// </para>
/// </summary>
public sealed class BusyGate
{
    private int _busy;

    /// <summary>当前是否有操作在进行。</summary>
    public bool IsBusy => Volatile.Read(ref _busy) != 0;

    /// <summary>尝试占用。已被占用时返回 false（调用方应提示并 return）。</summary>
    public bool TryEnter() => Interlocked.CompareExchange(ref _busy, 1, 0) == 0;

    /// <summary>释放。必须放在 finally 里，否则一次异常就把闸门永久锁死。</summary>
    public void Exit() => Interlocked.Exchange(ref _busy, 0);

    /// <summary>占用时禁用这批按钮、释放时恢复 —— 给出**看得见**的反馈（不只是"点了没反应"）。</summary>
    public void SetBusy(params Button?[] buttons)
    {
        foreach (var b in buttons)
        {
            if (b == null) continue;
            b.IsEnabled = !IsBusy;
        }
    }
}
