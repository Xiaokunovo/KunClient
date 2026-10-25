//! KunFlashTool 核心库（Rust）。
//!
//! 目标是把 C# `KunClient.Core` 里那套刷写实现（Protocol / Services / Transport / Modules）
//! 整体搬过来，C# 只留 WPF UI + 平台工具下载。当前进度见 `docs/rust-flash-migration-plan.md`。
//!
//! 模块边界：
//! - [`crc32`]：Firehose 校验用（标准 IEEE CRC-32，和厂商工具同口径）
//! - [`json`]：极小的 JSON 输出助手（**只写不读**）—— 跨 FFI 的统一信封要用
//! - [`xml`]：QFIL 风格 `rawprogram*.xml` / `patch*.xml` 的属性扫描
//! - [`elf`] / [`melf`]：loader 解析。红魔的 `devprg_*.melf` 是「无目录表的 ELF 容器」，
//!   必须能从中挑出真正送 Sahara 的 firehose 编程器（见 [`melf`] 模块注释里的实测表格）
//! - [`vendor`]：小米 / 欧加 / 联想 / 红魔 四家的差异点与**实现状态**（含未验证项的诚实标注）
//! - [`sahara`]：Sahara 协议帧编解码（常量与报文布局 1:1 抄自本仓库 C# 实现）
//! - [`session`]：刷写会话（有界日志队列 + 进度快照 + 状态 + 取消）—— FFI 的 handle 指向它
//!
//! ⚠️ 这里刻意没有任何第三方依赖，理由见 workspace `Cargo.toml`。

pub mod crc32;
pub mod elf;
pub mod json;
pub mod melf;
pub mod sahara;
pub mod session;
pub mod vendor;
pub mod xml;
