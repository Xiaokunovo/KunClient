using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Threading;
using System.Windows.Threading;

namespace KunFlashTool.App.Services;

/// <summary>
/// 进程 / 串口**逐行输出 → UI** 的批量投递（命令行页、投屏页、小米 EDL 页共用）。
///
/// 原来的写法是每行一次**同步** <c>Dispatcher.Invoke</c>：
/// <list type="bullet">
///   <item>读线程阻塞等 UI 线程 —— 界面一忙，整条管道跟着卡（日志越多越明显）；</item>
///   <item>UI 线程侧一行一次调度 + 一行一次渲染，几百行的命令直接掉帧。</item>
/// </list>
///
/// 现在生产者只往 <see cref="ConcurrentQueue{T}"/> 里塞（不阻塞、完全不碰 UI 线程），
/// UI 侧最多排**一次** Background 优先级回调，醒来把队列一次性取空 ——
/// 一次调度渲染一整批，行数再多也只多一次调度。
///
/// 🔴 唤醒语义（三处必须配合，缺一就会丢行）：
/// ① 入队后用 <c>Interlocked.Exchange</c> 抢「已排片」标志，只有抢到的那一次才排回调；
/// ② 回调里**先清标志再排空** —— 否则排空期间新进来的行看到标志还是 1，永远没人再叫它；
/// ③ 清完标志若队列非空，重新排一次（排空期间 / 排空之后新到的行由此兜住）。
/// </summary>
public sealed class LogLineQueue<T>
{
    /// <summary>一次回调最多取多少行：防止堆积几万行时一次占死 UI 线程，剩下的下一批再排。</summary>
    private const int MaxLinesPerFlush = 512;

    private readonly ConcurrentQueue<T> _queue = new();
    private readonly List<T> _batch = new(64);
    private readonly Dispatcher _dispatcher;
    private readonly Action<List<T>> _onFlush;
    private int _scheduled;

    public LogLineQueue(Dispatcher dispatcher, Action<List<T>> onFlush)
    {
        _dispatcher = dispatcher ?? throw new ArgumentNullException(nameof(dispatcher));
        _onFlush = onFlush ?? throw new ArgumentNullException(nameof(onFlush));
    }

    /// <summary>还没落到日志框的行数（收尾 / 排错用）。</summary>
    public int PendingCount => _queue.Count;

    /// <summary>入队。可在任意线程调用，**不阻塞**调用方。</summary>
    public void Enqueue(T item)
    {
        _queue.Enqueue(item);
        if (Interlocked.Exchange(ref _scheduled, 1) != 0) return;

        // Background：排在正常消息之后，界面绘制优先；这一批之前进的行都会并进这次回调
        _dispatcher.BeginInvoke(new Action(Flush), DispatcherPriority.Background);
    }

    /// <summary>
    /// 立刻把队列取空（**必须在 UI 线程调用**）。
    /// 给「输出结束 / 退出码 / 进程已退出」这类必须排在最后一行输出**之后**的收尾行兜底 ——
    /// 不先排空的话收尾行先渲染、真正的输出行还在队列里，顺序就反了。
    /// </summary>
    public void FlushNow()
    {
        Drain();
        EnsureScheduled();
    }

    private void Flush()
    {
        Interlocked.Exchange(ref _scheduled, 0);   // ② 先清标志
        Drain();
        EnsureScheduled();                          // ③ 兜住排空期间新到的行
    }

    private void EnsureScheduled()
    {
        if (_queue.IsEmpty) return;
        if (Interlocked.Exchange(ref _scheduled, 1) != 0) return;
        _dispatcher.BeginInvoke(new Action(Flush), DispatcherPriority.Background);
    }

    private void Drain()
    {
        _batch.Clear();
        while (_batch.Count < MaxLinesPerFlush && _queue.TryDequeue(out var item))
            _batch.Add(item);
        if (_batch.Count == 0) return;

        try
        {
            _onFlush(_batch);
        }
        finally
        {
            // 回调拿到的是一次性视图，绝不能把 List 留在外面继续用
            _batch.Clear();
        }
    }
}
