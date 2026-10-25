//! KunFlashTool 的 C ABI 出口。
//!
//! C# (WPF) 侧用 `[DllImport("kunflash_ffi")]` 调这里的函数。
//! 三条硬约定（`rust-csharp-ffi-singlefile` skill 里有完整来龙去脉，改边界前先读）：
//!
//! 1. **统一 JSON 信封**：每个返回 `*mut c_char` 的函数都返回
//!    `{"ok":true,"data":…}` 或 `{"ok":false,"error":"人话"}`。
//!    不设计结构体 marshaling —— 字段顺序/对齐错了不会报错，只会静默取到默认值。
//! 2. **每个导出都挂 `catch_unwind`**：`extern "C"` 里 panic 逃逸是 UB
//!    （Rust 2024 起直接 abort 整个进程）。而 workspace 的 release profile 是
//!    `panic = "unwind"`，编译器**不会**自动拦 —— 必须手动接。
//!    ⚠️ unsafe 边界一律收在 `guard` 之外：入参指针先转成 `Result`，再 move 进闭包，
//!    这样闭包体是纯安全的，`catch_unwind` 才覆盖得住全部业务逻辑。
//! 3. **所有权只有一条**：凡返回 `*mut c_char`，**一律**由调用方用
//!    [`kunflash_ffi_string_free`] 释放，**没有例外**。
//!    「大部分要释放、个别不要」这种规则人类一定会记错，而错法是进程级崩溃
//!    —— 初版让 `abi_version` 返回静态串就吃到过 `0xC0000005`。
//!    所以现在连版本号也走一次 JSON 序列化。
//!
//! C# 侧对应写法见 `.workbuddy/tmp/ffi_probe/Program.cs`。

use std::any::Any;
use std::ffi::{CStr, CString, c_char};
use std::panic::{AssertUnwindSafe, catch_unwind};

use kunflash_core::{crc32, json::{self, Obj}, xml};

/// ABI 版本号。**加字段不用动它**（信封天生向后兼容）；只有签名/语义破坏性变更才 +1。
pub const ABI_VERSION: u32 = 1;

/// 版本字符串。前缀 `kunflash-ffi/` 是探针与 C# 侧的断言依据，别改。
const ABI_VERSION_STR: &str = concat!("kunflash-ffi/", env!("CARGO_PKG_VERSION"));

// ───────────────────────────── 信封 ─────────────────────────────

/// `data` 必须**已经是合法 JSON 片段**（对象/数组/带引号的字符串）。
fn envelope_ok(data_json: String) -> *mut c_char {
    to_c_string(Obj::new().bool("ok", true).raw("data", &data_json).finish())
}

fn envelope_err(message: impl Into<String>) -> *mut c_char {
    // 先落成 String 再借用：构造器按值链式，直接 .into() 会撞上 &str / String 的类型不匹配
    let msg: String = message.into();
    to_c_string(Obj::new().bool("ok", false).string("error", &msg).finish())
}

fn to_c_string(s: String) -> *mut c_char {
    // JSON 里 NUL 会被转义成 \u0000，所以正常情况一定不含内部 NUL；这里只是兜底，
    // 绝不能因为「理论上不会」就让 CString::new 的 Err 把 panic 抛过边界（那正是 UB）。
    match CString::new(s) {
        Ok(c) => c.into_raw(),
        Err(_) => CString::new(r#"{"ok":false,"error":"返回值含 NUL 字节"}"#)
            .expect("静态串不含 NUL")
            .into_raw(),
    }
}

/// 统一的出错/panic 护栏。
fn guard<F>(f: F) -> *mut c_char
where
    F: FnOnce() -> Result<String, String>,
{
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(data)) => envelope_ok(data),
        Ok(Err(e)) => envelope_err(e),
        Err(payload) => envelope_err(format!("Rust 内部 panic：{}", panic_payload(payload))),
    }
}

fn panic_payload(payload: Box<dyn Any + Send>) -> String {
    if let Some(s) = payload.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = payload.downcast_ref::<String>() {
        s.clone()
    } else {
        // panic_any(42) 这类非字符串载荷必须**退化**，否则会在 catch 里二次 panic
        "非字符串载荷".to_string()
    }
}

/// 把 C 侧入参转成 `String`（在 unsafe 之外调用，见模块注释第 2 条）。
///
/// # Safety
/// `p` 必须是 NUL 结尾、有效且在本调用期间不会被释放的 C 字符串，或为空指针。
unsafe fn arg_to_string(p: *const c_char) -> Result<String, String> {
    if p.is_null() {
        return Err("入参指针为 null".to_string());
    }
    let c = unsafe { CStr::from_ptr(p) };
    c.to_str()
        .map(str::to_string)
        .map_err(|e| format!("入参不是合法 UTF-8：{e}"))
}

// ───────────────────────────── 导出 ─────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn kunflash_ffi_abi_version() -> *mut c_char {
    // ⚠️ 就算内容是个常量也**必须走信封**：调用方对返回值一视同仁地 free
    guard(|| Ok(kunflash_core::json::quoted(ABI_VERSION_STR)))
}

#[unsafe(no_mangle)]
pub extern "C" fn kunflash_ffi_abi_version_number() -> u32 {
    ABI_VERSION
}

/// 标准 IEEE CRC-32（固定向量：`crc32("123456789") == 0xCBF43926`）。
///
/// 捷径通道：不分配、不过 JSON —— 校验和是刷写热路径上按块调的。
/// 空指针 / 零长度都返回 `crc32(b"") == 0`，绝不 panic 过边界。
///
/// # Safety
/// `data` 必须指向至少 `len` 字节的可读内存，或为 null。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_crc32(data: *const u8, len: usize) -> u32 {
    if data.is_null() || len == 0 {
        return crc32::crc32(&[]);
    }
    let slice = unsafe { std::slice::from_raw_parts(data, len) };
    crc32::crc32(slice)
}

/// 解析 `patch*.xml` → JSON 数组（字段见 `kunflash_core::xml::PatchItem`）。
///
/// # Safety
/// `xml` 必须是 NUL 结尾的合法 UTF-8 C 字符串，或为 null（→ 错误信封）。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_parse_patch_xml(xml: *const c_char) -> *mut c_char {
    let arg = unsafe { arg_to_string(xml) };
    guard(move || {
        let xml = arg?;
        let items = xml::parse_patch(&xml)?;
        Ok(xml::patches_to_json_array(&items))
    })
}

/// 解析 `rawprogram*.xml` → JSON 数组（字段见 `kunflash_core::xml::RawProgramItem`）。
///
/// # Safety
/// 同 [`kunflash_ffi_parse_patch_xml`]。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_parse_rawprogram_xml(xml: *const c_char) -> *mut c_char {
    let arg = unsafe { arg_to_string(xml) };
    guard(move || {
        let xml = arg?;
        let items = xml::parse_rawprogram(&xml)?;
        Ok(xml::rawprogram_to_json_array(&items))
    })
}

/// 释放本库返回的字符串。
///
/// # Safety
/// `ptr` 必须是本库的导出函数返回、且**尚未**被释放过的指针，或为 null。
/// 绝不能用 C# 的 `Marshal.FreeHGlobal`（不同分配器，会毁堆）。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_string_free(ptr: *mut c_char) {
    if ptr.is_null() {
        return;
    }
    // 必须 from_raw —— 由 CString::into_raw 交出去的，只能由 CString::from_raw 收回来
    drop(unsafe { CString::from_raw(ptr) });
}

/// 四家厂商的清单（id / 显示名 / **实现状态** / 预期 loader 形态）。
///
/// UI 的厂商下拉框直接吃这个，别再在 C# 里手抄一份枚举 —— 抄一份就会漏一家
/// （联想就是后加进来的）。`support` 字段把「有没有验过」一起交出去，
/// 免得界面把「只留了接口」的厂商显示成可用。
#[unsafe(no_mangle)]
pub extern "C" fn kunflash_ffi_vendors() -> *mut c_char {
    guard(|| {
        use kunflash_core::vendor::{Support, Vendor};
        let arr = Vendor::ALL.map(|v| {
            let support = match v.support() {
                Support::Ported => "ported",
                Support::SampleVerified => "sample-verified",
                Support::InterfaceOnly => "interface-only",
            };
            Obj::new()
                .string("id", v.as_str())
                .string("name", v.display_name())
                .string("support", support)
                .string("expectedLoader", v.expected_loader().as_str())
                .finish()
        });
        Ok(json::array(arr))
    })
}

/// 解析 loader 文件并报告它的形态与内嵌镜像 —— 红魔 `.melf` 的关键入口。
///
/// 出口结构（`ok:true` 时）：
/// ```json
/// { "path": "…", "size": 1659300, "format": "melf", "isMelf": true,
///   "images": [ { "index":0, "offset":0, "class":"ELF32", "machine":"0xF3",
///                 "entry":"0x22126000", "phnum":2, "span":2308, "isProgrammer":false } ],
///   "programmer": { "index":1, "offset":2308, "class":"ELF32", "phnum":12,
///                   "vaddrBase":"0x20415100", "loadSegments":4, "loadPayloadBytes":177184 },
///   "notes": "…" }
/// ```
///
/// `format` 由**内容**判定，不看扩展名（`.melf` 本身就是 ELF 文件，后缀不可信）。
///
/// # Safety
/// `path` 必须是 NUL 结尾的合法 UTF-8 C 字符串，或为 null（→ 错误信封）。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_loader_inspect(path: *const c_char) -> *mut c_char {
    let arg = unsafe { arg_to_string(path) };
    guard(move || {
        let path_str = arg?;
        let data = std::fs::read(&path_str).map_err(|e| format!("读不到文件 {path_str}：{e}"))?;

        let format = kunflash_core::vendor::detect_loader(&data);
        let mut obj = Obj::new()
            .string("path", &path_str)
            .u64("size", data.len() as u64)
            .string("format", format.as_str())
            .bool("isMelf", format == kunflash_core::vendor::LoaderFormat::MelfContainer);

        match format {
            kunflash_core::vendor::LoaderFormat::Unknown => {
                obj = obj.raw("images", "[]").null("programmer").string(
                    "notes",
                    "不是 ELF —— 四家的 loader 都应是 ELF（单 ELF 或 .melf 容器），请确认选对了文件",
                );
            }
            kunflash_core::vendor::LoaderFormat::SingleElf => {
                // 单 ELF：直接把它的 PT_LOAD 段报出去（送 Sahara 就按这些段发）
                use kunflash_core::elf;
                let h = elf::parse_header(&data, 0)?;
                let segs = elf::program_headers(&data, 0)?
                    .into_iter()
                    .filter(|p| p.p_type == elf::PT_LOAD && p.filesz > 0)
                    .collect::<Vec<_>>();
                let payload: u64 = segs.iter().map(|p| p.filesz).sum();
                obj = obj
                    .raw("images", "[]")
                    .raw(
                        "programmer",
                        &Obj::new()
                            .u64("index", 0)
                            .u64("offset", 0)
                            .string("class", h.class.as_str())
                            .string("machine", &format!("0x{:X}", h.machine))
                            .u64("phnum", h.phnum as u64)
                            .u64("loadSegments", segs.len() as u64)
                            .u64("loadPayloadBytes", payload)
                            .finish(),
                    )
                    .string("notes", "单个 Hexagon ELF；送 Sahara 按 PT_LOAD 段发");
            }
            kunflash_core::vendor::LoaderFormat::MelfContainer => {
                let m = kunflash_core::melf::parse(&data)?;
                let images: Vec<String> = m
                    .images
                    .iter()
                    .enumerate()
                    .map(|(i, img)| {
                        Obj::new()
                            .u64("index", i as u64)
                            .u64("offset", img.offset as u64)
                            .string("class", img.header.class.as_str())
                            .string("machine", &format!("0x{:X}", img.header.machine))
                            .u64("entry", img.header.entry)
                            .u64("phnum", img.header.phnum as u64)
                            .u64("span", img.span as u64)
                            .bool("isProgrammer", img.looks_like_programmer())
                            .finish()
                    })
                    .collect();

                let prog = m.firehose_programmer().expect("parse 已保证存在");
                let idx = m
                    .images
                    .iter()
                    .position(|i| i.offset == prog.offset)
                    .unwrap_or(0);
                let segs = prog.load_segments(&data)?;
                let payload = prog.load_payload_bytes(&data)?;

                obj = obj
                    .raw("images", &json::array(images))
                    .raw(
                        "programmer",
                        &Obj::new()
                            .u64("index", idx as u64)
                            .u64("offset", prog.offset as u64)
                            .string("class", prog.header.class.as_str())
                            .string("machine", &format!("0x{:X}", prog.header.machine))
                            .u64("phnum", prog.header.phnum as u64)
                            .u64("loadSegments", segs.len() as u64)
                            .u64("loadPayloadBytes", payload)
                            .finish(),
                    )
                    .string(
                        "notes",
                        &format!(
                            "MELF 容器：共 {} 个内嵌镜像，编程器是第 {idx} 个（machine=0x{:X} 且 phnum>={}）；\
                             容器内**没有目录表**，镜像是靠扫描 ELF 魔数 + 校验头枚举出来的",
                            m.images.len(),
                            prog.header.machine,
                            kunflash_core::melf::MIN_PROGRAMMER_PHNUM
                        ),
                    );
            }
        }

        Ok(obj.finish())
    })
}

// ───────────────────────── 会话（handle 型 API） ─────────────────────────
//
// 这一组**不返回字符串信封**，而是返回一个 handle（`Box<Session>` 的裸指针）。
// 因此多两条约定：
//
// 1. handle ≠ 0 表示成功；失败返回 0，原因用 [`kunflash_ffi_last_error`] 取
//    （handle 型函数没有信封可挂，总不能把错误吞掉）。
// 2. **句柄注册表**：每个 handle 登记在一张全局表里，所有按 handle 操作的导出
//    先查表。这样「UI 误调一个已释放的 handle」会得到一句干净的
//    `会话句柄无效或已释放`，而不是 use-after-free 直接崩进程 ——
//    界面驱动的 API 里这种误调迟早会出现，不值得赌。

use std::collections::HashSet;
use std::sync::{Mutex, OnceLock};

use kunflash_core::session::Session;
use kunflash_core::vendor::Vendor;

fn live_handles() -> &'static Mutex<HashSet<usize>> {
    static LIVE: OnceLock<Mutex<HashSet<usize>>> = OnceLock::new();
    LIVE.get_or_init(|| Mutex::new(HashSet::new()))
}

fn last_error_slot() -> &'static Mutex<String> {
    static SLOT: OnceLock<Mutex<String>> = OnceLock::new();
    SLOT.get_or_init(|| Mutex::new(String::new()))
}

fn set_last_error(msg: impl Into<String>) {
    if let Ok(mut s) = last_error_slot().lock() {
        *s = msg.into();
    }
}

/// 取走最近一次（handle 型函数）失败的原因，取完即清空。
///
/// 与其它导出一样返回 JSON 信封：`{"ok":true,"data":"…"}`，空串表示没有待取错误。
#[unsafe(no_mangle)]
pub extern "C" fn kunflash_ffi_last_error() -> *mut c_char {
    guard(|| {
        let msg = last_error_slot().lock().map(|mut s| std::mem::take(&mut *s)).unwrap_or_default();
        Ok(json::quoted(&msg))
    })
}

/// 按 handle 取会话引用；句柄无效/已释放一律 Err（见本节顶部第 2 条）。
fn with_session<R>(handle: u64, f: impl FnOnce(&Session) -> R) -> Result<R, String> {
    let key = handle as usize;
    if key == 0 {
        return Err("会话句柄为 0（未创建）".to_string());
    }
    let alive = live_handles().lock().map(|s| s.contains(&key)).unwrap_or(false);
    if !alive {
        return Err("会话句柄无效或已释放".to_string());
    }
    // SAFETY: 只有 session_new 登记过的指针能走到这里，且登记与注销都由本模块独占管理。
    let session = unsafe { &*(key as *const Session) };
    Ok(f(session))
}

/// 包一层「取句柄 → 执行 → 信封」，省掉每个导出都写一遍。
fn on_session(handle: u64, f: impl FnOnce(&Session) -> Result<String, String>) -> *mut c_char {
    guard(move || with_session(handle, f)?)
}

/// 建一个刷写会话。返回 handle（0 = 失败，原因见 [`kunflash_ffi_last_error`]）。
///
/// `vendor` 传 `NULL` 或空串表示「还没选厂商」（读分区表这类操作不需要它）；
/// 传了但认不出来（拼错）会**失败**而不是静默当成没选 —— 静默会让四家适配的
/// 分支悄悄走错路。可选项清单见 [`kunflash_ffi_vendors`]。
///
/// # Safety
/// `vendor` 必须是 NUL 结尾的合法 UTF-8 C 字符串或 NULL。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_session_new(vendor: *const c_char) -> u64 {
    // NULL **不是**错误 —— 它表示「还没选厂商」（读分区表这类操作不需要厂商）。
    // 只有「传了但认不出来」才算错：静默当成没选会让四家适配的分支悄悄走错路。
    let vendor_arg = if vendor.is_null() {
        String::new()
    } else {
        match unsafe { arg_to_string(vendor) } {
            Ok(s) => s,
            Err(e) => {
                set_last_error(format!("vendor 参数有问题：{e}"));
                return 0;
            }
        }
    };

    let parsed = match vendor_arg.trim() {
        "" => None,
        s => match Vendor::parse(s) {
            Ok(v) => Some(v),
            Err(e) => {
                set_last_error(e);
                return 0;
            }
        },
    };

    let boxed = Box::new(Session::new(parsed));
    let handle = Box::into_raw(boxed) as usize as u64;
    if let Ok(mut live) = live_handles().lock() {
        live.insert(handle as usize);
    }
    set_last_error("");
    handle
}

/// 释放会话。**幂等**：`0`、已释放过的、甚至别人家的 handle 都只是静默返回，
/// 不会二次 drop（二次 drop 会直接毁堆）。
///
/// # Safety
/// `handle` 必须来自 [`kunflash_ffi_session_new`]，或为 0。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_session_free(handle: u64) {
    let key = handle as usize;
    if key == 0 {
        return;
    }
    let was_live = live_handles().lock().map(|mut s| s.remove(&key)).unwrap_or(false);
    if !was_live {
        // 重复释放 / 从没登记过的 handle：什么都不做。**不能**照指针去 drop。
        return;
    }
    drop(unsafe { Box::from_raw(key as *mut Session) });
}

/// 置取消位（幂等）。C# 的取消按钮直接调它。
///
/// # Safety
/// 同 [`kunflash_ffi_session_free`]。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_session_cancel(handle: u64) -> *mut c_char {
    on_session(handle, |s| {
        s.cancel();
        Ok(Obj::new().bool("cancelled", s.is_cancelled()).finish())
    })
}

/// 会话状态：`{phase, connected, faultReason}`。
///
/// # Safety
/// 同 [`kunflash_ffi_session_free`]。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_session_state(handle: u64) -> *mut c_char {
    on_session(handle, |s| Ok(s.state().to_json()))
}

/// 进度快照：`{partitionPercent, overallPercent, bytesDone, bytesTotal, speedBps, partition, message}`。
///
/// # Safety
/// 同 [`kunflash_ffi_session_free`]。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_session_poll_progress(handle: u64) -> *mut c_char {
    on_session(handle, |s| Ok(s.progress().to_json()))
}

/// **拉取**日志（取走并清空）：`{items:[{seq,level,text}], dropped:N, nextSeqHint:N}`。
///
/// 为什么要拉取而不是回调：见 `kunflash_core::session` 模块注释（`Dispatcher.Invoke` 死锁）。
/// C# 侧用 `DispatcherTimer` 在 UI 线程上定时取即可，不需要跨线程 Invoke。
///
/// # Safety
/// 同 [`kunflash_ffi_session_free`]。
#[unsafe(no_mangle)]
pub unsafe extern "C" fn kunflash_ffi_session_poll_logs(handle: u64) -> *mut c_char {
    on_session(handle, |s| Ok(s.logs_json()))
}

/// Sahara 命令号 / 子命令号 / 模式值。
///
/// 出口是给**跨语言一致性**用的：C# 侧那份常量与这里的值必须逐项相等，
/// 谁被改动都能一眼看出来（协议数字写错的表现是设备卡在 9008 不动，极难查）。
#[unsafe(no_mangle)]
pub extern "C" fn kunflash_ffi_sahara_commands() -> *mut c_char {
    guard(|| {
        use kunflash_core::sahara::{cmd, exec_cmd, mode};
        let pairs = |items: &[(&str, u32)]| {
            json::array(items.iter().map(|(k, v)| Obj::new().string("name", k).u32("value", *v).finish()))
        };
        Ok(Obj::new()
            .raw(
                "commands",
                &pairs(&[
                    ("hello_request", cmd::HELLO_REQUEST),
                    ("hello_response", cmd::HELLO_RESPONSE),
                    ("read_data", cmd::READ_DATA),
                    ("end_transfer", cmd::END_TRANSFER),
                    ("done_request", cmd::DONE_REQUEST),
                    ("done_response", cmd::DONE_RESPONSE),
                    ("reset_request", cmd::RESET_REQUEST),
                    ("reset_response", cmd::RESET_RESPONSE),
                    ("command_ready", cmd::COMMAND_READY),
                    ("switch_mode", cmd::SWITCH_MODE),
                    ("execute_request", cmd::EXECUTE_REQUEST),
                    ("execute_response", cmd::EXECUTE_RESPONSE),
                    ("execute_data", cmd::EXECUTE_DATA),
                    ("memory_read_data_64", cmd::MEMORY_READ_DATA_64),
                ]),
            )
            .raw(
                "execCommands",
                &pairs(&[
                    ("nop", exec_cmd::NOP),
                    ("serial_num_read", exec_cmd::SERIAL_NUM_READ),
                    ("msm_hw_id_read", exec_cmd::MSM_HW_ID_READ),
                    ("oem_pk_hash_read", exec_cmd::OEM_PK_HASH_READ),
                    ("get_software_version_sbl", exec_cmd::GET_SOFTWARE_VERSION_SBL),
                    ("read_chip_id_v3", exec_cmd::READ_CHIP_ID_V3),
                ]),
            )
            .raw(
                "modes",
                &pairs(&[
                    ("image_tx_pending", mode::IMAGE_TX_PENDING),
                    ("image_tx_complete", mode::IMAGE_TX_COMPLETE),
                    ("memory_debug", mode::MEMORY_DEBUG),
                    ("command", mode::COMMAND),
                ]),
            )
            .u64("helloResponseWireSize", kunflash_core::sahara::HELLO_RESPONSE_WIRE_SIZE as u64)
            .u64("headerLen", kunflash_core::sahara::HEADER_LEN as u64)
            .u64("logCapacity", kunflash_core::session::LOG_CAPACITY as u64)
            .finish())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use kunflash_core::session::{LogLevel, Phase, ProgressSnapshot};
    use std::ffi::CString;
    use std::ptr;
    use std::sync::atomic::{AtomicUsize, Ordering};

    /// 读走信封文本并释放 —— **模拟 C# 侧的真实用法**（`TakeUtf8`）。
    unsafe fn take(p: *mut c_char) -> String {
        assert!(!p.is_null(), "native 返回了空指针");
        let s = unsafe { CStr::from_ptr(p) }.to_str().expect("必须是 UTF-8").to_string();
        unsafe { kunflash_ffi_string_free(p) };
        s
    }

    fn call_patch(xml: &str) -> String {
        let c = CString::new(xml).unwrap();
        unsafe { take(kunflash_ffi_parse_patch_xml(c.as_ptr())) }
    }

    fn call_raw(xml: &str) -> String {
        let c = CString::new(xml).unwrap();
        unsafe { take(kunflash_ffi_parse_rawprogram_xml(c.as_ptr())) }
    }

    #[test]
    fn abi_version_goes_through_envelope() {
        // 这一条是那 0xC0000005 事故的回归测试：版本号也必须是信封，不能是裸串
        let json = unsafe { take(kunflash_ffi_abi_version()) };
        assert!(json.starts_with(r#"{"ok":true,"data":"kunflash-ffi/"#), "{json}");
        assert_eq!(kunflash_ffi_abi_version_number(), 1);
    }

    #[test]
    fn crc32_fast_path() {
        let v = b"123456789";
        assert_eq!(unsafe { kunflash_ffi_crc32(v.as_ptr(), v.len()) }, 0xCBF4_3926);
        // 空指针 / 零长度都不许崩
        assert_eq!(unsafe { kunflash_ffi_crc32(ptr::null(), 0) }, 0);
        assert_eq!(unsafe { kunflash_ffi_crc32(v.as_ptr(), 0) }, 0);
        assert_eq!(unsafe { kunflash_ffi_crc32(ptr::null(), 16) }, 0);
    }

    #[test]
    fn parse_patch_returns_expected_fields() {
        let xml = r#"<patches><patch filename="gpt_main0.bin" start_sector="1"
                      size_in_bytes="4096" what="中文 说明" offset="48"
                      physical_partition_number="0" /></patches>"#;
        let json = call_patch(xml);
        assert!(json.contains(r#""ok":true"#), "{json}");
        assert!(json.contains(r#""size_in_bytes":4096"#), "{json}");
        assert!(json.contains(r#""physical_partition_number":"0""#), "{json}");
        assert!(json.contains(r#""what":"中文 说明""#), "{json}");
        // 契约：byte_offset 恒为 null（不从 offset 推导）
        assert!(json.contains(r#""byte_offset":null"#), "{json}");
    }

    #[test]
    fn parse_rawprogram_keeps_zero_ambiguity_over_the_boundary() {
        let xml = r#"<data>
            <program filename="x.bin" start_sector="NUM_DISK_SECTORS-33" num_partition_sectors="6" SECTOR_SIZE_IN_BYTES="4096" />
            <program filename="z.bin" start_sector="0" num_partition_sectors="1" SECTOR_SIZE_IN_BYTES="4096" />
        </data>"#;
        let json = call_raw(xml);
        assert!(json.contains(r#""start_sector_is_literal":false"#), "{json}");
        assert!(json.contains(r#""start_sector_is_literal":true"#), "{json}");
        assert!(json.contains(r#""sector_size_in_bytes":4096"#), "{json}");
    }

    #[test]
    fn null_pointer_is_an_error_envelope_not_a_crash() {
        // C# 在 string 为 null 时确实会传 0 过来
        let json = unsafe { take(kunflash_ffi_parse_patch_xml(ptr::null())) };
        assert!(json.contains(r#""ok":false"#), "{json}");
        assert!(json.contains("null"), "{json}");
    }

    #[test]
    fn invalid_utf8_is_an_error_envelope() {
        let bad: [u8; 3] = [0xFF, 0xFE, 0x00];
        let json = unsafe { take(kunflash_ffi_parse_rawprogram_xml(bad.as_ptr() as *const c_char)) };
        assert!(json.contains(r#""ok":false"#), "{json}");
        assert!(json.contains("UTF-8"), "{json}");
    }

    #[test]
    fn malformed_xml_is_an_error_envelope() {
        let json = call_raw("<data><program");
        assert!(json.contains("\"ok\":false"), "{json}");
        // 错误信息必须非空且不是空串 —— 上层要把它显示给用户。
        // ⚠️ 这里**不能**写 contains(r#""error":""#)：raw 字符串会在第一个 `"#` 处收尾，
        //    实际内容变成 `"error":"`，断言就永远为真（这个坑刚踩过）。
        assert!(json.contains("\"error\":\"") && !json.contains("\"error\":\"\""), "{json}");
    }

    #[test]
    fn panic_is_converted_to_an_error_envelope() {
        static HITS: AtomicUsize = AtomicUsize::new(0);
        let json = unsafe {
            take(guard(|| {
                HITS.fetch_add(1, Ordering::SeqCst);
                panic!("模拟刷写中途炸了");
            }))
        };
        assert_eq!(HITS.load(Ordering::SeqCst), 1, "闭包必须真的执行到 panic 那一行");
        assert!(json.contains(r#""ok":false"#), "{json}");
        assert!(json.contains("panic"), "{json}");
    }

    #[test]
    fn non_string_panic_payload_degrades_instead_of_double_panicking() {
        let json = unsafe { take(guard(|| std::panic::panic_any(42))) };
        assert!(json.contains(r#""ok":false"#), "{json}");
        assert!(json.contains("非字符串载荷"), "{json}");
    }

    /// 所有权护栏：**每个**返回字符串的导出都按同一条规则释放，能走到最后就说明没有特例。
    /// 这条测试的存在意义是：将来谁加了「这个返回值不用 free」的特例，这里会立刻炸掉。
    #[test]
    fn every_string_returning_export_uses_the_same_ownership_rule() {
        let c = CString::new("<patches><patch filename=\"a\" size_in_bytes=\"1\"/></patches>").unwrap();
        let c2 = CString::new("<data><program filename=\"b\" start_sector=\"1\"/></data>").unwrap();
        let all = [
            kunflash_ffi_abi_version(),
            unsafe { kunflash_ffi_parse_patch_xml(c.as_ptr()) },
            unsafe { kunflash_ffi_parse_rawprogram_xml(c2.as_ptr()) },
            kunflash_ffi_vendors(),
            kunflash_ffi_sahara_commands(),
            kunflash_ffi_last_error(),
        ];
        for p in all {
            let s = unsafe { CStr::from_ptr(p) }.to_str().unwrap().to_string();
            assert!(s.starts_with('{'), "{s}");
            unsafe { kunflash_ffi_string_free(p) };
        }
        // 空指针释放必须安全（C# 侧可能在异常路径上无脑调用）
        unsafe { kunflash_ffi_string_free(ptr::null_mut()) };
    }

    // ── 会话 handle API ──

    #[test]
    fn session_lifecycle_and_polling() {
        let vendor = CString::new("redmagic").unwrap();
        let h = unsafe { kunflash_ffi_session_new(vendor.as_ptr()) };
        assert_ne!(h, 0, "合法厂商必须建出会话");

        // 状态：初始 idle
        let st = unsafe { take(kunflash_ffi_session_state(h)) };
        assert!(st.contains("\"phase\":\"idle\""), "{st}");
        assert!(st.contains("\"connected\":false"), "{st}");

        // 进度：初始全 0
        let pr = unsafe { take(kunflash_ffi_session_poll_progress(h)) };
        assert!(pr.contains("\"bytesDone\":0"), "{pr}");

        // 日志：推进去再拉，拉完必须空
        {
            let s = unsafe { &*(h as usize as *const Session) };
            s.push_log(LogLevel::Ok, "已连接 COM4");
            s.push_log(LogLevel::Warn, "扇区大小按设备值修正");
            s.set_phase(Phase::Connected);
            s.set_progress(ProgressSnapshot {
                overall_percent: 42.0,
                bytes_done: 1024,
                bytes_total: 4096,
                ..Default::default()
            });
        }
        let logs = unsafe { take(kunflash_ffi_session_poll_logs(h)) };
        assert!(logs.contains("已连接 COM4"), "{logs}");
        assert!(logs.contains("\"level\":\"warn\""), "{logs}");
        assert!(logs.contains("\"dropped\":0"), "{logs}");
        let empty = unsafe { take(kunflash_ffi_session_poll_logs(h)) };
        assert!(empty.contains("\"items\":[]"), "拉取式必须取走并清空：{empty}");

        let st2 = unsafe { take(kunflash_ffi_session_state(h)) };
        assert!(st2.contains("\"connected\":true"), "{st2}");
        let pr2 = unsafe { take(kunflash_ffi_session_poll_progress(h)) };
        assert!(pr2.contains("\"overallPercent\":42"), "{pr2}");

        // 取消：幂等
        let c1 = unsafe { take(kunflash_ffi_session_cancel(h)) };
        assert!(c1.contains("\"cancelled\":true"), "{c1}");
        let _ = unsafe { take(kunflash_ffi_session_cancel(h)) };

        unsafe { kunflash_ffi_session_free(h) };
    }

    #[test]
    fn stale_handle_is_a_clean_error_not_a_crash() {
        let vendor = CString::new("xiaomi").unwrap();
        let h = unsafe { kunflash_ffi_session_new(vendor.as_ptr()) };
        unsafe { kunflash_ffi_session_free(h) };
        // 释放之后再调：必须是一句干净的 JSON 错误（句柄注册表拦下来的），
        // 而不是 use-after-free。界面驱动的 API 里这种误调迟早出现。
        let after = unsafe { take(kunflash_ffi_session_state(h)) };
        assert!(after.contains("\"ok\":false"), "{after}");
        assert!(after.contains("已释放"), "{after}");
        // 二次释放不能崩
        unsafe { kunflash_ffi_session_free(h) };
        unsafe { kunflash_ffi_session_free(0) };
    }

    #[test]
    fn invalid_vendor_fails_loudly_with_a_reason() {
        let bad = CString::new("htc").unwrap();
        let h = unsafe { kunflash_ffi_session_new(bad.as_ptr()) };
        assert_eq!(h, 0, "认不出的厂商必须失败，不能静默当成没选");
        let err = unsafe { take(kunflash_ffi_last_error()) };
        assert!(err.contains("未知厂商"), "{err}");
        // 取走即清空：第二次拿到的是空串（仍旧是信封，`data` 为空）
        let again = unsafe { take(kunflash_ffi_last_error()) };
        assert!(again.contains("\"data\":\"\""), "{again}");

        // null / 空串 = 还没选厂商，应当成功
        let h2 = unsafe { kunflash_ffi_session_new(ptr::null()) };
        assert_ne!(h2, 0);
        unsafe { kunflash_ffi_session_free(h2) };
        let empty = CString::new("   ").unwrap();
        let h3 = unsafe { kunflash_ffi_session_new(empty.as_ptr()) };
        assert_ne!(h3, 0);
        unsafe { kunflash_ffi_session_free(h3) };
    }

    #[test]
    fn handle_of_zero_is_never_dereferenced() {
        let j = unsafe { take(kunflash_ffi_session_poll_logs(0)) };
        assert!(j.contains("\"ok\":false"), "{j}");
        assert!(j.contains("未创建"), "{j}");
        let j2 = unsafe { take(kunflash_ffi_session_cancel(0)) };
        assert!(j2.contains("\"ok\":false"), "{j2}");
    }

    #[test]
    fn sahara_constants_are_exported_for_cross_language_drift_check() {
        let j = unsafe { take(kunflash_ffi_sahara_commands()) };
        // C# 侧同样有一份常量（SaharaCommands.cs）；这些数字对不上就等于协议写错了。
        // 出口形状是 [{name,value}] 列表（不是 map）—— 列表能保序，排查时更好读。
        assert!(j.contains(r#"{"name":"hello_response","value":2}"#), "{j}");
        assert!(j.contains(r#"{"name":"read_data","value":3}"#), "{j}");
        assert!(j.contains(r#"{"name":"memory_read_data_64","value":18}"#), "{j}"); // 0x12 == 18
        assert!(j.contains(r#"{"name":"msm_hw_id_read","value":2}"#), "{j}");
        assert!(j.contains("\"helloResponseWireSize\":48"), "{j}");
        assert!(j.contains("\"headerLen\":8"), "{j}");
        assert!(j.contains("\"logCapacity\":5000"), "{j}");
    }
}
