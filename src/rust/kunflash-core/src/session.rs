//! 刷写会话：**日志队列 + 进度快照 + 状态 + 取消**（FFI 的 handle 就指向它）。
//!
//! ## 为什么日志/进度走「拉取」而不是回调
//!
//! 上游方案 `docs/rust-ffi-migration.md` §4.1 已经论证过，这里是它的落地：
//! C# 侧的日志回调是 `Dispatcher.Invoke`（**同步**阻塞直到 UI 线程处理完）。
//! 若照搬成「Rust 通过函数指针回调 C# 委托」，就变成
//! 「刷写线程 → Rust → 回调 → Dispatcher.Invoke → 等 UI 线程」，
//! 而 UI 线程可能正卡在 `Task.Run(刷写).Wait()` —— **教科书级死锁**；
//! 而且异常穿越 FFI 边界是 UB。
//!
//! 拉取式没有这些问题：方向永远是「C# 主动调 Rust」，无重入、无栈穿越。
//! C# 侧用 `DispatcherTimer` 在 UI 线程上取，**连 Dispatcher.Invoke 都不需要**。
//!
//! ## 队列必须有上限
//!
//! 整包刷写能跑几十分钟，日志会一直涨。上限 [`LOG_CAPACITY`] 条，
//! 超出**丢最老的**并计数；下次拉取时把「丢了多少条」一并交出去
//! （而不是悄悄丢 —— 悄悄丢会让用户以为日志完整，排障时被误导）。
//!
//! ## 取消怎么传
//!
//! 不把 C# 的 `CancellationToken` 传过 FFI：那东西在 C# 侧是托管对象，
//! 跨边界传等于把 GC 生命周期交给 native 管。改为 [`Session::cancel`] 置一个
//! `Arc<AtomicBool>`；刷写引擎 clone 一份自己持有（见 [`Session::cancel_token`]）。
//! C# 的取消按钮直接调到 `kunflash_ffi_session_cancel`。

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use crate::json::{Obj, array};
use crate::vendor::Vendor;

/// 日志队列上限。超出丢最老的（见模块注释）。
pub const LOG_CAPACITY: usize = 5000;

/// 日志级别。字符串形式跨 FFI（`info`/`ok`/`warn`/`error`），与 C# 侧 `PageLog` 对齐。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LogLevel {
    Info,
    Ok,
    Warn,
    Error,
}

impl LogLevel {
    pub fn as_str(self) -> &'static str {
        match self {
            LogLevel::Info => "info",
            LogLevel::Ok => "ok",
            LogLevel::Warn => "warn",
            LogLevel::Error => "error",
        }
    }

    /// 认不出来的一律当 `info`：**不报错**。日志级别是展示信息，
    /// 为了一个未知级别让整条刷写流程失败，代价完全不成比例。
    pub fn parse(s: &str) -> LogLevel {
        match s.trim().to_ascii_lowercase().as_str() {
            "ok" | "success" => LogLevel::Ok,
            "warn" | "warning" => LogLevel::Warn,
            "error" | "err" | "fail" => LogLevel::Error,
            _ => LogLevel::Info,
        }
    }
}

/// 一条日志。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LogItem {
    /// 单调递增序号：C# 侧可以据此判断「有没有漏批次」。
    pub seq: u64,
    pub level: LogLevel,
    pub text: String,
}

impl LogItem {
    pub fn to_json(&self) -> String {
        Obj::new()
            .u64("seq", self.seq)
            .string("level", self.level.as_str())
            .string("text", &self.text)
            .finish()
    }
}

/// 有界日志队列。
#[derive(Debug, Default)]
pub struct LogQueue {
    items: VecDeque<LogItem>,
    next_seq: u64,
    /// 累计丢弃条数（丢的是**最老的**）。
    dropped_total: u64,
    /// 上次拉取时已经交待过的丢弃数，避免同一笔反复提醒。
    dropped_reported: u64,
}

impl LogQueue {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&mut self, level: LogLevel, text: impl Into<String>) {
        let seq = self.next_seq;
        self.next_seq += 1;
        self.items.push_back(LogItem { seq, level, text: text.into() });

        // 上限：丢最老的并计数。**不要**在这里直接插一条「已截断」的日志 ——
        // 那会让队列永远处在满/超之间抖动，而且丢的那条自己也会被丢。
        // 提醒统一在 drain 时按计数补，见下。
        while self.items.len() > LOG_CAPACITY {
            self.items.pop_front();
            self.dropped_total += 1;
        }
    }

    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn dropped_total(&self) -> u64 {
        self.dropped_total
    }

    /// 取走并清空。返回 `(条目, 本次新丢弃的条数)`；
    /// 若有新丢弃，会在**队首**补一条 Warn 说明（用户看日志能直接看到「少了多少」）。
    pub fn drain(&mut self) -> (Vec<LogItem>, u64) {
        let newly_dropped = self.dropped_total - self.dropped_reported;
        self.dropped_reported = self.dropped_total;

        let mut out: Vec<LogItem> = self.items.drain(..).collect();
        if newly_dropped > 0 {
            out.insert(
                0,
                LogItem {
                    seq: u64::MAX, // 合成条目，不占真实序号
                    level: LogLevel::Warn,
                    text: format!("日志过多，已丢弃最早的 {newly_dropped} 条（上限 {LOG_CAPACITY}）"),
                },
            );
        }
        (out, newly_dropped)
    }
}

/// 进度快照。字段与 C# 侧 `FlashProgressEventArgs` / `ShellStatus.Report` 对齐。
#[derive(Debug, Clone, PartialEq)]
pub struct ProgressSnapshot {
    /// 当前分区进度 0..100。
    pub partition_percent: f64,
    /// 整包进度 0..100。
    pub overall_percent: f64,
    pub bytes_done: u64,
    pub bytes_total: u64,
    pub speed_bps: f64,
    pub partition: String,
    pub message: String,
}

impl Default for ProgressSnapshot {
    fn default() -> Self {
        Self {
            partition_percent: 0.0,
            overall_percent: 0.0,
            bytes_done: 0,
            bytes_total: 0,
            speed_bps: 0.0,
            partition: String::new(),
            message: String::new(),
        }
    }
}

impl ProgressSnapshot {
    /// 百分比一律夹到 0..100：刷写过程中出现 101% 或 NaN 会让界面进度条爆表，
    /// 而这种数在「按块累加」的实现里很容易出现（解压后字节数 ≠ Content-Length 那种）。
    fn clamp_percent(v: f64) -> f64 {
        if v.is_nan() { 0.0 } else { v.clamp(0.0, 100.0) }
    }

    pub fn to_json(&self) -> String {
        Obj::new()
            .raw("partitionPercent", &fmt_f64(Self::clamp_percent(self.partition_percent)))
            .raw("overallPercent", &fmt_f64(Self::clamp_percent(self.overall_percent)))
            .u64("bytesDone", self.bytes_done)
            .u64("bytesTotal", self.bytes_total)
            .raw("speedBps", &fmt_f64(if self.speed_bps.is_nan() { 0.0 } else { self.speed_bps }))
            .string("partition", &self.partition)
            .string("message", &self.message)
            .finish()
    }
}

/// JSON 数字格式化：非有限值退成 0，避免输出 `NaN`/`inf` 让 C# 侧 `JsonDocument.Parse` 抛。
fn fmt_f64(v: f64) -> String {
    if v.is_finite() { format!("{v}") } else { "0".to_string() }
}

/// 会话阶段。C# 侧的「就绪 / 连接中 / 已连接 / 读分区中 / 刷写中」直接映射它。
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    Idle,
    Connecting,
    Connected,
    ReadingPartitions,
    Flashing,
    Done,
    Failed,
}

impl Phase {
    pub fn as_str(self) -> &'static str {
        match self {
            Phase::Idle => "idle",
            Phase::Connecting => "connecting",
            Phase::Connected => "connected",
            Phase::ReadingPartitions => "reading-partitions",
            Phase::Flashing => "flashing",
            Phase::Done => "done",
            Phase::Failed => "failed",
        }
    }
}

/// 会话状态快照。
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionState {
    pub phase: Phase,
    /// 失败原因（人话）。只有 `phase == Failed` 时才有意义。
    pub fault_reason: Option<String>,
}

impl Default for SessionState {
    fn default() -> Self {
        Self { phase: Phase::Idle, fault_reason: None }
    }
}

impl SessionState {
    pub fn to_json(&self) -> String {
        Obj::new()
            .string("phase", self.phase.as_str())
            .bool("connected", matches!(self.phase, Phase::Connected | Phase::ReadingPartitions | Phase::Flashing))
            .opt_string("faultReason", self.fault_reason.as_deref())
            .finish()
    }
}

/// 一个刷写会话。FFI 的 handle 指向它（`Box<Session>` 的裸指针）。
///
/// 所有可变状态都在 `Mutex` / `Atomic*` 后面：UI 线程读、刷写线程写。
#[derive(Debug)]
pub struct Session {
    /// 分配的会话号（日志用；不是 handle 本身）。
    pub id: u64,
    /// 选中的厂商。`None` = 还没选（读分区表这类操作不需要它）。
    pub vendor: Option<Vendor>,
    logs: Mutex<LogQueue>,
    progress: Mutex<ProgressSnapshot>,
    state: Mutex<SessionState>,
    cancel: Arc<AtomicBool>,
    next_id: AtomicU64,
}

impl Session {
    pub fn new(vendor: Option<Vendor>) -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        Self {
            id: NEXT.fetch_add(1, Ordering::Relaxed),
            vendor,
            logs: Mutex::new(LogQueue::new()),
            progress: Mutex::new(ProgressSnapshot::default()),
            state: Mutex::new(SessionState::default()),
            cancel: Arc::new(AtomicBool::new(false)),
            next_id: AtomicU64::new(0),
        }
    }

    pub fn push_log(&self, level: LogLevel, text: impl Into<String>) {
        if let Ok(mut q) = self.logs.lock() {
            q.push(level, text);
        }
    }

    /// 取走日志。返回 `(条目, 本次新丢弃数)`。
    pub fn drain_logs(&self) -> (Vec<LogItem>, u64) {
        match self.logs.lock() {
            Ok(mut q) => q.drain(),
            Err(_) => (Vec::new(), 0), // 锁中毒也要给个空结果，不能 panic 过 FFI
        }
    }

    pub fn log_len(&self) -> usize {
        self.logs.lock().map(|q| q.len()).unwrap_or(0)
    }

    pub fn progress(&self) -> ProgressSnapshot {
        self.progress.lock().map(|p| p.clone()).unwrap_or_default()
    }

    pub fn set_progress(&self, p: ProgressSnapshot) {
        if let Ok(mut cur) = self.progress.lock() {
            *cur = p;
        }
    }

    pub fn state(&self) -> SessionState {
        self.state.lock().map(|s| s.clone()).unwrap_or_default()
    }

    pub fn set_phase(&self, phase: Phase) {
        if let Ok(mut s) = self.state.lock() {
            s.phase = phase;
            if phase != Phase::Failed {
                s.fault_reason = None;
            }
        }
    }

    pub fn fail(&self, reason: impl Into<String>) {
        if let Ok(mut s) = self.state.lock() {
            s.phase = Phase::Failed;
            s.fault_reason = Some(reason.into());
        }
    }

    /// 置取消位。**幂等**：重复调不报错（C# 侧可能在多个路径上收尾）。
    pub fn cancel(&self) {
        self.cancel.store(true, Ordering::SeqCst);
    }

    pub fn is_cancelled(&self) -> bool {
        self.cancel.load(Ordering::SeqCst)
    }

    /// 给（将来的）刷写引擎 clone 的取消令牌。
    pub fn cancel_token(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.cancel)
    }

    /// 日志的 JSON 信封数据（`{items:[…], dropped:N, nextSeqHint:N}`）。
    pub fn logs_json(&self) -> String {
        let (items, dropped) = self.drain_logs();
        let next_seq = items.iter().filter(|i| i.seq != u64::MAX).map(|i| i.seq + 1).max();
        Obj::new()
            .raw("items", &array(items.iter().map(LogItem::to_json)))
            .u64("dropped", dropped)
            .opt_u64("nextSeqHint", next_seq)
            .finish()
    }

    pub fn vendor_json(&self) -> String {
        match self.vendor {
            Some(v) => Obj::new()
                .string("id", v.as_str())
                .string("name", v.display_name())
                .finish(),
            None => "null".to_string(),
        }
    }

    /// 这个会话的 `next_id`（占位，保留字段以免以后加东西时 ABI 变动）。
    pub fn probe_counter(&self) -> u64 {
        self.next_id.load(Ordering::Relaxed)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn log_queue_caps_and_counts_dropped() {
        let mut q = LogQueue::new();
        for i in 0..(LOG_CAPACITY + 25) {
            q.push(LogLevel::Info, format!("line {i}"));
        }
        assert_eq!(q.len(), LOG_CAPACITY, "队列不能超过上限");
        assert_eq!(q.dropped_total(), 25);

        let (items, dropped) = q.drain();
        assert_eq!(dropped, 25);
        // 丢了多少条必须在**队首**明说，而不是悄悄丢
        assert_eq!(items.len(), LOG_CAPACITY + 1);
        assert_eq!(items[0].level, LogLevel::Warn);
        assert!(items[0].text.contains("已丢弃最早的 25 条"), "{}", items[0].text);
        // 真正的第一条是第 25 条（0..24 被丢了）
        assert_eq!(items[1].text, "line 25");
        assert!(q.is_empty(), "drain 之后必须清空");
    }

    #[test]
    fn dropped_notice_is_not_repeated_every_drain() {
        let mut q = LogQueue::new();
        for i in 0..(LOG_CAPACITY + 3) {
            q.push(LogLevel::Info, format!("l{i}"));
        }
        let (_, first) = q.drain();
        assert_eq!(first, 3);
        q.push(LogLevel::Info, "after");
        let (items, second) = q.drain();
        assert_eq!(second, 0, "没有新丢弃就不该再报一次");
        assert_eq!(items.len(), 1);
        assert_eq!(items[0].text, "after");
    }

    #[test]
    fn seq_is_monotonic_across_drains() {
        let mut q = LogQueue::new();
        q.push(LogLevel::Info, "a");
        q.push(LogLevel::Info, "b");
        let (first, _) = q.drain();
        q.push(LogLevel::Info, "c");
        let (second, _) = q.drain();
        assert_eq!(first.iter().map(|i| i.seq).collect::<Vec<_>>(), vec![0, 1]);
        assert_eq!(second[0].seq, 2, "序号跨批次不能重置");
    }

    #[test]
    fn unknown_level_degrades_to_info_instead_of_failing() {
        assert_eq!(LogLevel::parse(""), LogLevel::Info);
        assert_eq!(LogLevel::parse("VERBOSE"), LogLevel::Info);
        assert_eq!(LogLevel::parse(" Error "), LogLevel::Error);
        assert_eq!(LogLevel::parse("OK"), LogLevel::Ok);
    }

    #[test]
    fn session_state_and_cancel() {
        let s = Session::new(Some(Vendor::RedMagic));
        assert_eq!(s.state().phase, Phase::Idle);
        assert!(!s.is_cancelled());

        s.set_phase(Phase::Connecting);
        assert_eq!(s.state().phase, Phase::Connecting);

        s.fail("设备未进入 9008");
        let st = s.state();
        assert_eq!(st.phase, Phase::Failed);
        assert_eq!(st.fault_reason.as_deref(), Some("设备未进入 9008"));
        assert!(!st.to_json().is_empty());

        // 恢复阶段时要顺手清掉失败原因，否则界面会一直挂着上次的错误
        s.set_phase(Phase::Connecting);
        assert_eq!(s.state().fault_reason, None);

        // 取消幂等
        s.cancel();
        s.cancel();
        assert!(s.is_cancelled());
        assert!(s.cancel_token().load(Ordering::SeqCst));
    }

    #[test]
    fn progress_json_clamps_nan_and_out_of_range() {
        // 101% / NaN 会让界面进度条爆表 —— 出口一律夹住
        let p = ProgressSnapshot {
            partition_percent: 101.5,
            overall_percent: f64::NAN,
            bytes_done: 1024,
            bytes_total: 2048,
            speed_bps: f64::INFINITY,
            partition: "super".into(),
            message: "写入中".into(),
        };
        let j = p.to_json();
        assert!(j.contains("\"partitionPercent\":100"), "{j}");
        assert!(j.contains("\"overallPercent\":0"), "{j}");
        assert!(j.contains("\"speedBps\":0"), "{j}");
        assert!(!j.contains("NaN") && !j.contains("inf"), "不能输出 NaN/inf：{j}");
        assert!(j.contains("\"partition\":\"super\""), "{j}");
    }

    #[test]
    fn logs_json_drains_and_reports_dropped() {
        let s = Session::new(None);
        s.push_log(LogLevel::Ok, "已连接 COM4");
        s.push_log(LogLevel::Warn, "扇区大小按设备值修正");
        let j = s.logs_json();
        assert!(j.contains("\"dropped\":0"), "{j}");
        assert!(j.contains("已连接 COM4"), "{j}");
        assert!(j.contains("\"level\":\"warn\""), "{j}");
        assert_eq!(s.log_len(), 0, "拉取后必须清空");
        // 第二次拉取是空的
        assert!(s.logs_json().contains("\"items\":[]"));
    }

    #[test]
    fn vendor_json_null_when_unset() {
        assert_eq!(Session::new(None).vendor_json(), "null");
        let j = Session::new(Some(Vendor::Lenovo)).vendor_json();
        assert!(j.contains("\"id\":\"lenovo\"") && j.contains("\"联想\""), "{j}");
    }
}
